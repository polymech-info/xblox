#include "blocks/shell_blocks.hpp"
#include "json/json_utils.hpp"

#include "blocks/block_variables.hpp"
#include "blocks/json_templates.hpp"
#include "core/app_exe_directory.hpp"
#include "process/process_helpers.hpp"
#include "llm/tools/run/RunTool.hpp"
#include "logging/logger.h"
#include "pm_branding_config.hpp"
#include "string/string_utils.hpp"
#include "xblox_constants.hpp"
#include "xblox_commands.hpp"
#include "xblox_prepared_run.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shellapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace media::xblox::blocks {
namespace {

namespace fs = std::filesystem;

struct BackgroundXbloxRun {
    std::shared_ptr<std::atomic_bool> cancel;
};

std::mutex g_xblox_run_mutex;
std::unordered_map<std::string, BackgroundXbloxRun> g_xblox_runs;

// build_argv_command operates on a copy of the raw block (after per-token ${var}
// resolution), so it keeps these small raw-JSON readers; the handler itself
// reads through BlockIO.
std::string json_string(const nlohmann::json& o, const char* key)
{
    return o.is_object() && o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string{};
}

std::vector<std::string> json_string_array(const nlohmann::json& o, const char* key)
{
    std::vector<std::string> out;
    if (!o.is_object() || !o.contains(key) || !o[key].is_array())
        return out;
    for (const auto& item : o[key]) {
        if (item.is_string())
            out.push_back(item.get<std::string>());
    }
    return out;
}

nlohmann::json parse_xblox_arg_overrides(const std::vector<std::string>& args)
{
    nlohmann::json out = nlohmann::json::object();
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string token = args[i];
        if (token == "--")
            break;
        if (token.rfind("--", 0) != 0 || token.size() <= 2)
            continue;
        token.erase(0, 2);
        std::string value;
        const std::size_t eq = token.find('=');
        if (eq != std::string::npos) {
            value = token.substr(eq + 1);
            token = token.substr(0, eq);
        } else if (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) {
            value = args[++i];
        } else {
            out[token] = true;
            continue;
        }
        if (token.empty())
            continue;
        out[token] = media::json::parse_or(value, nlohmann::json(value));
    }
    return out;
}

std::vector<std::string> value_string_array(const Value* value)
{
    std::vector<std::string> out;
    if (!value)
        return out;
    const nlohmann::json j = value->to_json();
    if (!j.is_array())
        return out;
    for (const auto& item : j) {
        if (item.is_string())
            out.push_back(item.get<std::string>());
        else if (!item.is_null())
            out.push_back(item.dump());
    }
    return out;
}

ExecutionOptions child_xblox_options(BlockRuntime& runtime,
                                     const fs::path& script_path,
                                     bool loop,
                                     bool has_loop,
                                     long long loop_interval_ms,
                                     bool has_loop_interval,
                                     bool loop_reset,
                                     bool has_loop_reset,
                                     bool background,
                                     std::shared_ptr<std::atomic_bool> cancel_flag = {})
{
    ExecutionOptions options = runtime.options;
    options.source_path = script_path;
    options.default_cwd = script_path.parent_path();
    options.collect_events = true;
    options.process_args = {"xbloxRun", media::path::path_to_utf8(script_path)};
    options.process_command_line = "xbloxRun " + media::path::path_to_utf8(script_path);
    options.extra_args.clear();
    if (has_loop)
        options.loop_override = loop;
    if (has_loop_interval)
        options.loop_interval_ms_override = loop_interval_ms;
    if (has_loop_reset)
        options.loop_reset_override = loop_reset;
    if (background) {
        options.event_sink = {};
        options.resolve_hooks.clear();
        options.pull_external_vars = {};
        options.publish_vars = {};
    }
    if (cancel_flag)
        options.cancel_requested = [cancel_flag]() { return cancel_flag->load(std::memory_order_relaxed); };
    return options;
}

fs::path resolve_xblox_script_path(const std::string& raw, const ExecutionOptions& options)
{
    fs::path path = media::path::path_from_utf8(raw);
    if (path.is_relative() && !options.default_cwd.empty())
        path = options.default_cwd / path;
    return path;
}

std::string xblox_run_key(const fs::path& script_path)
{
    std::error_code ec;
    fs::path absolute = fs::absolute(script_path, ec);
    if (ec)
        absolute = script_path;
    return media::path::path_to_utf8(absolute.lexically_normal());
}

fs::path resolve_open_path(const std::string& raw, const ExecutionOptions& options)
{
    fs::path path = media::path::path_from_utf8(raw);
    if (path.is_relative() && !options.default_cwd.empty())
        path = options.default_cwd / path;
    return path.lexically_normal();
}

// openPath.openIn → host app command, or empty for the OS default handler.
std::string app_command_for_open_in(std::string raw)
{
    raw = media::strutil::lower_ascii(std::move(raw));
    if (raw.empty() || raw == "system" || raw == "default" || raw == "os" || raw == "shell")
        return {};
    if (raw == "edit" || raw == "editor")
        return "edit";
    if (raw == "app" || raw == "open" || raw == "internal" || raw == "viewer")
        return "open";
    return {};
}

// Headless / CLI fallback: spawn this product's GUI sibling.
// POLYMECH_APP_TANIT_VIEWER is the SKU define (pins workbench to viewer); it
// does not name a different file — branding already yields tanit-viewer.exe.
// FEATURE_VIEWER_WEB is the WebView2 dock and is ON in both SKUs — do not
// gate the launch on it, and do not prefer k_viewer_exe_basename_w (that is
// only the Open-with copy of full Tanit).
bool launch_gui_open_path(const std::string& path, bool edit, std::string* message)
{
    const fs::path gui = media::app::exe_gui_executable();
    if (gui.empty()) {
        if (message) {
            *message = std::string("could not resolve ") + pm::brand::k_exe_basename_u8
                + " next to this process";
        }
        return false;
    }

#if defined(_WIN32)
    std::wstring cmd = media::process::quote_process_arg(gui.wstring());
    cmd += L" --ui-preset viewer --src ";
    cmd += media::process::quote_process_arg(media::process::widen_utf8(path));
    if (edit)
        cmd += L" --edit";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        if (message)
            *message = "failed to launch " + media::path::path_to_utf8(gui);
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (message)
            *message = "failed to launch " + media::path::path_to_utf8(gui);
        return false;
    }
    if (pid == 0) {
        if (edit)
            ::execl(gui.c_str(), gui.c_str(), "--ui-preset", "viewer", "--src", path.c_str(),
                    "--edit", static_cast<char*>(nullptr));
        else
            ::execl(gui.c_str(), gui.c_str(), "--ui-preset", "viewer", "--src", path.c_str(),
                    static_cast<char*>(nullptr));
        ::_exit(127);
    }
#endif
    if (message)
        *message = edit ? "opened in editor" : "opened in viewer";
    return true;
}

PreparedXbloxRun* runner_for_xblox_run(BlockIO& io,
                                       const fs::path& script_path,
                                       const std::string& key,
                                       bool reload,
                                       std::unique_ptr<PreparedXbloxRun>& local)
{
    const std::string cache_key = "xbloxRun:document:" + key;
    if (RunScope* scope = io.scope()) {
        if (reload)
            scope->release(cache_key);
        auto& runner = scope->acquire<PreparedXbloxRun>(cache_key, [&] {
            return std::make_shared<PreparedXbloxRun>(script_path);
        });
        if (!runner.ok()) {
            scope->release(cache_key);
            return &runner;
        }
        return &runner;
    }

    local = std::make_unique<PreparedXbloxRun>(script_path);
    return local.get();
}

std::string shell_quote_arg(const std::string& arg)
{
#if defined(_WIN32)
    std::string out = "\"";
    for (char ch : arg) {
        if (ch == '"' || ch == '\\')
            out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (char ch : arg) {
        if (ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
#endif
}

std::string powershell_literal_arg(const std::string& arg)
{
    std::string out = "'";
    for (char ch : arg) {
        if (ch == '\'')
            out += "''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

std::string build_argv_command(const nlohmann::json& block,
                               const std::string& shell)
{
    std::string command = json_string(block, "command");
    if (command.empty())
        command = json_string(block, "exe");
    if (command.empty())
        return {};
    std::string line;
#if defined(_WIN32)
    // RunTool's Windows "auto" shell is PowerShell. A quoted executable path
    // is only a string expression there unless invoked with the call operator.
    if (shell == "auto" || shell == "powershell")
        line = "& ";
#else
    (void)shell;
#endif
    line += shell_quote_arg(command);
    for (const auto& arg : json_string_array(block, "args")) {
        line.push_back(' ');
        line += shell_quote_arg(arg);
    }
    return line;
}

std::string wrap_cwd_command(const std::string& command, const std::string& cwd)
{
    if (cwd.empty())
        return command;
#if defined(_WIN32)
    return "Push-Location -LiteralPath " + powershell_literal_arg(cwd) + "; try { " + command + " } finally { Pop-Location }";
#else
    return "cd " + shell_quote_arg(cwd) + " && " + command;
#endif
}

std::string normalize_mode(std::string mode)
{
    mode = media::strutil::lower_ascii(std::move(mode));
    return mode.empty() ? "shell" : mode;
}

std::string normalize_log_level(std::string level, const char* fallback)
{
    level = media::strutil::lower_ascii(std::move(level));
    if (level.empty())
        return fallback;
    if (level == "warning")
        return "warn";
    if (level == "err" || level == "critical")
        return "error";
    if (level == "off" || level == "none")
        return "off";
    if (level == "trace" || level == "debug" || level == "info" || level == "warn" || level == "error")
        return level;
    return fallback;
}

void log_at_level(const std::string& level, const std::string& message)
{
    if (level == "off" || message.empty())
        return;
    const std::string line = "[xblox:shell] " + message;
    if (level == "error")
        logger::error(line);
    else if (level == "warn")
        logger::warn(line);
    else if (level == "info")
        logger::info(line);
    else if (level == "debug")
        logger::debug(line);
    else
        logger::trace(line);
}

std::vector<std::string> split_output_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(text);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    if (!text.empty() && (text.back() == '\n' || text.back() == '\r') && !lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

void log_output_lines(const std::string& stream, const std::string& level, const std::vector<std::string>& lines)
{
    for (const auto& line : lines)
        log_at_level(level, stream + ": " + line);
}

void log_xblox_run_result(const std::string& level, const std::string& path, const ExecutionResult& result)
{
    if (level == "off")
        return;
    log_at_level(level, "xbloxRun: " + path + " exitCode=" + std::to_string(result.exit_code)
        + " events=" + std::to_string(result.event_count)
        + " errors=" + std::to_string(result.error_count));
    for (const auto& ev : result.events) {
        const std::string status = ev.status.empty() ? "event" : ev.status;
        const std::string kind = ev.kind.empty() ? "unknown" : ev.kind;
        log_at_level(level, "xbloxRun/" + status + ": " + kind + (ev.message.empty() ? "" : " - " + ev.message));
    }
}

void emit_xblox_run_stream_events(BlockRuntime& runtime,
                                  const std::string& path,
                                  const std::string& owner,
                                  const std::string& stdout_level,
                                  const std::string& stderr_level,
                                  const ExecutionResult& result)
{
    if (!runtime.events_enabled)
        return;
    const auto emit_lines = [&](const ExecutionEvent& child,
                                const char* stream,
                                const std::string& level,
                                const std::vector<std::string>& lines) {
        if (level == "off")
            return;
        for (const std::string& line : lines) {
            if (line.empty())
                continue;
            ExecutionEvent event{
                path,
                "xbloxRun",
                "ok",
                owner + "/" + (child.block_id.empty() ? child.kind : child.block_id)
                    + " " + stream + ": " + line,
                0,
                {{"level", level}, {"stream", stream}, {"owner", owner}},
            };
            event.block_id = runtime.block_id;
            event.type = "commandLog";
            runtime.emit(runtime.user_data, std::move(event));
        }
    };
    for (const ExecutionEvent& child : result.events) {
        emit_lines(child, "stdout", stdout_level, child.stdout_lines);
        emit_lines(child, "stderr", stderr_level, child.stderr_lines);
    }
}

BlockResult open_path_block(BlockIO& io)
{
    const std::string id = io.str("id");
    const std::string raw_path = io.str("path");
    if (raw_path.empty()) {
        io.error("openPath", "openPath: path is empty", 2).id(id).type("openPath");
        return BlockResult::Error;
    }

    const fs::path target = resolve_open_path(raw_path, io.runtime().options);
    const std::string target_string = media::path::path_to_utf8(target);
    const std::string app_command = app_command_for_open_in(io.str("openIn", "system"));

    int code = 0;
    std::string message;
    if (!app_command.empty()) {
        if (io.runtime().options.app_command_handler) {
            CommandInvocation invocation;
            invocation.path = io.path();
            invocation.id = id;
            invocation.action = "app";
            invocation.command = {
                {"appCommand", app_command},
                {"path", target_string},
                {"args", nlohmann::json::array({target_string})},
            };
            invocation.payload = invocation.command;
            code = io.runtime().options.app_command_handler(invocation, &message);
        } else if (!launch_gui_open_path(target_string, app_command == "edit", &message)) {
            io.error("openPath", message.empty() ? "openPath: could not launch GUI" : message, 1)
                .id(id).type("openPath")
                .payload({{"ok", false}, {"path", target_string}, {"openIn", app_command}});
            return BlockResult::Error;
        }
    } else if (io.runtime().options.open_path_handler) {
        CommandInvocation invocation;
        invocation.path = io.path();
        invocation.id = id;
        invocation.action = "path";
        invocation.payload = {{"path", target_string}};
        invocation.command = invocation.payload;
        code = io.runtime().options.open_path_handler(invocation, &message);
    } else {
#if defined(_WIN32)
        HINSTANCE result = ::ShellExecuteW(nullptr, L"open", media::process::widen_utf8(target_string).c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
        code = reinterpret_cast<intptr_t>(result) > 32 ? 0 : 1;
        message = code == 0 ? "path opened" : "path open failed";
#elif defined(__APPLE__)
        code = std::system((std::string("open ") + shell_quote_arg(target_string)).c_str());
        message = code == 0 ? "path opened" : "path open failed";
#else
        code = std::system((std::string("xdg-open ") + shell_quote_arg(target_string)).c_str());
        message = code == 0 ? "path opened" : "path open failed";
#endif
    }

    nlohmann::json payload = {
        {"ok", code == 0},
        {"path", target_string},
        {"message", message},
        {"exitCode", code},
    };
    if (code == 0) {
        io.ok("openPath", message.empty() ? "path opened" : message)
            .id(id).type("openPath")
            .payload(payload)
            .result(Value{target_string}, ParamKind::file_path);
        return BlockResult::Ok;
    }
    io.error("openPath", message.empty() ? "path open failed" : message, code)
        .id(id).type("openPath")
        .payload(std::move(payload));
    return BlockResult::Error;
}

BlockResult shell_block(BlockIO& io)
{
    BlockRuntime& runtime = io.runtime();
    const std::string id = io.str("id");
    const std::string mode = normalize_mode(io.str("mode"));
    std::string shell = io.str("shell");
    if (shell.empty()) shell = "auto";
    const bool background = io.flag("background", false);
    const int timeout_ms = std::max(1, static_cast<int>(io.i64("timeoutMs", runtime.options.default_timeout_ms)));
    // `log` defaults on when stdout/stderr levels are explicitly set; check raw
    // presence so the declared default (false) doesn't mask that intent.
    const bool log_streams = io.has("log") ? io.flag("log") : (io.has("stdout") || io.has("stderr"));
    const std::string stdout_level = normalize_log_level(io.str("stdout"), "info");
    const std::string stderr_level = normalize_log_level(io.str("stderr"), "error");
    std::string cwd = io.str("cwd"); // declared file-ish string -> ${var} interpolated by the resolver
    if (cwd.empty() && !runtime.options.default_cwd.empty())
        cwd = media::path::path_to_utf8(runtime.options.default_cwd);

    std::string command;
    if (mode == "argv") {
        // argv tokens are a structured array the resolver does not interpolate;
        // resolve each ${var} on a copy of the raw block before quoting.
        nlohmann::json resolved_block = io.raw();
        if (resolved_block.contains("command") && resolved_block["command"].is_string())
            resolved_block["command"] = bv::resolve(resolved_block["command"].get<std::string>(), runtime, false, bv::kMaxDeepPasses);
        if (resolved_block.contains("exe") && resolved_block["exe"].is_string())
            resolved_block["exe"] = bv::resolve(resolved_block["exe"].get<std::string>(), runtime, false, bv::kMaxDeepPasses);
        if (resolved_block.contains("args") && resolved_block["args"].is_array()) {
            for (auto& arg : resolved_block["args"]) {
                if (arg.is_string())
                    arg = bv::resolve(arg.get<std::string>(), runtime, false, bv::kMaxDeepPasses);
            }
        }
        command = build_argv_command(resolved_block, shell);
    } else {
        command = io.str("command");
        if (command.empty()) {
            const nlohmann::json& raw = io.raw();
            if (raw.contains("line") && raw["line"].is_string())
                command = bv::resolve(raw["line"].get<std::string>(), runtime, false, bv::kMaxDeepPasses);
        }
    }

    if (command.empty()) {
        io.error("shell", "shell command is empty", 2).id(id).type("shell");
        return BlockResult::Error;
    }
    if (background && runtime.options.console_command_handler) {
        std::string message;
        const int code = runtime.options.console_command_handler(
            wrap_cwd_command(command, cwd),
            true,
            io.flag("closeOnExit", false),
            &message);
        const bool ok = code == 0;
        nlohmann::json meta = {
            {"ok", ok}, {"mode", mode}, {"shell", shell}, {"command", command},
            {"cwd", cwd}, {"background", true}, {"terminal", true},
            {"result", message}, {"exitCode", code},
        };
        const std::string msg = message.empty()
            ? (ok ? "shell command sent to terminal" : "shell terminal dispatch failed")
            : message;
        if (ok) {
            io.ok("shell", msg).id(id).type("shell").payload(std::move(meta));
            return BlockResult::Ok;
        }
        io.error("shell", msg, code).id(id).type("shell").payload(std::move(meta));
        return BlockResult::Error;
    }

    std::mutex chunk_mutex;
    nlohmann::json chunks = nlohmann::json::array();
    media::llm::run::set_output_chunk_hook([&](const std::string& stream, const std::string& chunk) {
        std::lock_guard<std::mutex> lk(chunk_mutex);
        chunks.push_back({{"stream", stream}, {"chunk", chunk}});
    });

    nlohmann::json args = {
        {"command", command},
        {"shell", shell},
        {"timeout_ms", timeout_ms},
    };
    const auto run = media::llm::run::execute(args, cwd);
    media::llm::run::clear_output_chunk_hook();

    const nlohmann::json envelope = run.envelope.is_object() ? run.envelope : nlohmann::json::object();
    const std::string stdout_text = envelope.value("stdout", std::string{});
    const std::string stderr_text = envelope.value("stderr", std::string{});
    const int exit_code = envelope.value("exit_code", run.ok ? 0 : 1);
    const bool timed_out = envelope.value("timed_out", false);
    const bool cancelled = envelope.value("cancelled", false);
    const bool ok = run.ok && exit_code == 0;
    const std::vector<std::string> stdout_lines = split_output_lines(stdout_text);
    const std::vector<std::string> stderr_lines = split_output_lines(stderr_text);
    if (log_streams) {
        log_output_lines("stdout", stdout_level, stdout_lines);
        log_output_lines("stderr", stderr_level, stderr_lines);
    }

    // Structured event metadata (the primary `result`/PREVIOUS is the stdout
    // string, set via io.result below).
    nlohmann::json meta = {
        {"ok", ok}, {"mode", mode}, {"shell", shell}, {"command", command},
        {"cwd", cwd}, {"background", background}, {"log", log_streams},
        {"stdoutLevel", stdout_level}, {"stderrLevel", stderr_level},
        {"exitCode", exit_code}, {"stdout", stdout_text}, {"stderr", stderr_text},
        {"stdoutLines", stdout_lines}, {"stderrLines", stderr_lines},
        {"timedOut", timed_out}, {"cancelled", cancelled}, {"chunks", chunks},
    };
    if (!run.error.empty())
        meta["error"] = run.error;

    if (ok) {
        io.ok("shell", "shell command complete")
          .id(id).type("shell").payload(std::move(meta)).lines(stdout_lines, stderr_lines)
          .result(stdout_text, ParamKind::string_);
        return BlockResult::Ok;
    }
    io.error("shell", run.error.empty() ? "shell command failed" : run.error, exit_code)
      .id(id).type("shell").payload(std::move(meta)).lines(stdout_lines, stderr_lines)
      .result(stdout_text, ParamKind::string_);
    return BlockResult::Error;
}

BlockResult xblox_run_block(BlockIO& io)
{
    BlockRuntime& runtime = io.runtime();
    std::string raw_path = io.str("path");
    if (raw_path.empty()) {
        raw_path = io.str_any({"src", "file"});
        if (media::strutil::has_variable_tokens(raw_path))
            raw_path = bv::resolve(raw_path, runtime, false, bv::kMaxDeepPasses);
    }
    if (raw_path.empty()) {
        io.error("xbloxRun", "xbloxRun path is empty", 2);
        return BlockResult::Error;
    }

    const fs::path script_path = resolve_xblox_script_path(raw_path, runtime.options);
    const std::string key = xblox_run_key(script_path);
    const std::string action = normalize_mode(io.str("action")).empty() ? "run" : normalize_mode(io.str("action"));
    const std::string log_level = normalize_log_level(io.str("logLevel"), "off");
    const bool log_streams = io.has("log") ? io.flag("log") : (io.has("stdout") || io.has("stderr"));
    const std::string stdout_level = normalize_log_level(io.str("stdout"), "info");
    const std::string stderr_level = normalize_log_level(io.str("stderr"), "error");
    const bool reload = io.flag("reload", false);

    if (action == "stop") {
        bool stopped = false;
        {
            std::lock_guard<std::mutex> lock(g_xblox_run_mutex);
            auto it = g_xblox_runs.find(key);
            if (it != g_xblox_runs.end()) {
                it->second.cancel->store(true, std::memory_order_relaxed);
                g_xblox_runs.erase(it);
                stopped = true;
            }
        }
        nlohmann::json payload{{"ok", true}, {"action", "stop"}, {"stopped", stopped}, {"path", media::path::path_to_utf8(script_path)}, {"key", key}};
        io.ok("xbloxRun", stopped ? "XBlox script stop requested" : "XBlox script was not running")
          .set("path", media::path::path_to_utf8(script_path))
          .result(Value::from_json(std::move(payload)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "status") {
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(g_xblox_run_mutex);
            running = g_xblox_runs.find(key) != g_xblox_runs.end();
        }
        nlohmann::json payload{{"ok", true}, {"action", "status"}, {"running", running}, {"path", media::path::path_to_utf8(script_path)}, {"key", key}};
        io.ok("xbloxRun", running ? "XBlox script is running" : "XBlox script is not running")
          .set("path", media::path::path_to_utf8(script_path))
          .result(Value::from_json(std::move(payload)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    std::unique_ptr<PreparedXbloxRun> local_runner;
    PreparedXbloxRun* runner = runner_for_xblox_run(io, script_path, key, reload, local_runner);
    if (!runner || !runner->ok()) {
        const std::string err = runner ? runner->error() : std::string{"failed to prepare script"};
        io.error("xbloxRun", "xbloxRun: " + err, 2).set("path", media::path::path_to_utf8(script_path));
        return BlockResult::Error;
    }

    nlohmann::json context_overrides = nlohmann::json::object();
    if (const Value* ctx_value = io.value("context")) {
        nlohmann::json ctx = resolve_json_templates(io, ctx_value->to_json());
        if (ctx.is_object()) {
            for (auto it = ctx.begin(); it != ctx.end(); ++it)
                context_overrides[it.key()] = it.value();
        }
    }

    std::vector<std::string> arg_tokens = value_string_array(io.value("args"));
    for (std::string& token : arg_tokens) {
        if (token.find("${") != std::string::npos)
            token = bv::resolve(token, runtime, false, bv::kMaxDeepPasses);
    }
    const nlohmann::json arg_overrides = parse_xblox_arg_overrides(arg_tokens);

    const bool background = io.flag("background", false);
    const bool has_loop = io.has("loop");
    const bool loop = io.flag("loop", false);
    const bool has_loop_interval = io.has("loopIntervalMs");
    const long long loop_interval_ms = std::clamp<long long>(
        io.i64("loopIntervalMs", 100), 0, constants::kLoopIntervalMaxMs);
    const bool has_loop_reset = io.has("loopReset");
    const bool loop_reset = io.flag("loopReset", false);

    ExecutionOptions child_options = child_xblox_options(runtime, script_path,
                                                         loop, has_loop,
                                                         loop_interval_ms, has_loop_interval,
                                                         loop_reset, has_loop_reset,
                                                         background);
    const bool parent_observes_child_events = runtime.options.collect_events || runtime.options.event_sink;
    if (log_level == "off" && !parent_observes_child_events) {
        child_options.collect_events = false;
        child_options.event_sink = {};
    }

    if (background) {
        const std::string child_path = media::path::path_to_utf8(script_path);
        nlohmann::json document = runner->instantiate(context_overrides, arg_overrides);
        const auto cancel_flag = std::make_shared<std::atomic_bool>(false);
        {
            std::lock_guard<std::mutex> lock(g_xblox_run_mutex);
            if (g_xblox_runs.find(key) != g_xblox_runs.end()) {
                nlohmann::json payload{{"ok", false}, {"action", "run"}, {"running", true}, {"path", child_path}, {"key", key}};
                io.error("xbloxRun", "XBlox script is already running for this path", 2)
                  .set("path", child_path)
                  .result(Value::from_json(std::move(payload)), ParamKind::json_value);
                return BlockResult::Error;
            }
            g_xblox_runs[key] = {cancel_flag};
        }
        child_options = child_xblox_options(runtime, script_path,
                                            loop, has_loop,
                                            loop_interval_ms, has_loop_interval,
                                            loop_reset, has_loop_reset,
                                            background,
                                            cancel_flag);
        if (log_level == "off" && !parent_observes_child_events) {
            child_options.collect_events = false;
            child_options.event_sink = {};
        }
        std::thread([document = std::move(document), options = std::move(child_options), child_path, key, log_level]() mutable {
            ExecutionResult result = media::xblox::run_blocks_file(document, options);
            log_xblox_run_result(log_level, child_path, result);
            std::lock_guard<std::mutex> lock(g_xblox_run_mutex);
            g_xblox_runs.erase(key);
        }).detach();
        nlohmann::json launched{
            {"ok", true},
            {"action", "run"},
            {"background", true},
            {"path", media::path::path_to_utf8(script_path)},
            {"key", key},
            {"logLevel", log_level},
            {"args", arg_overrides},
        };
        io.ok("xbloxRun", "XBlox script started in background")
          .set("path", media::path::path_to_utf8(script_path))
          .result(Value::from_json(std::move(launched)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    ExecutionResult result = runner->run_once(context_overrides, arg_overrides, child_options);
    if (log_streams) {
        emit_xblox_run_stream_events(runtime, io.path(), media::path::path_to_utf8(script_path.filename()),
                                     stdout_level, stderr_level, result);
    }
    log_xblox_run_result(log_level, media::path::path_to_utf8(script_path), result);
    if (!parent_observes_child_events) {
        nlohmann::json payload{
            {"ok", result.ok},
            {"exitCode", result.exit_code},
            {"path", media::path::path_to_utf8(script_path)},
            {"background", false},
            {"logLevel", log_level},
            {"eventCount", static_cast<long long>(result.event_count)},
            {"errorCount", static_cast<long long>(result.error_count)},
            {"lastBlockResult", result.last_block_result},
            {"args", arg_overrides},
        };
        runtime.result.last_block_result = payload;
        ++runtime.result.event_count;
        io.set_result(std::move(payload), ParamKind::json_value);
        if (!result.ok) {
            runtime.result.ok = false;
            if (runtime.result.exit_code == 0)
                runtime.result.exit_code = result.exit_code == 0 ? 1 : result.exit_code;
            return BlockResult::Error;
        }
        return BlockResult::Ok;
    }
    nlohmann::json payload{
        {"ok", result.ok},
        {"exitCode", result.exit_code},
        {"path", media::path::path_to_utf8(script_path)},
        {"background", false},
        {"logLevel", log_level},
        {"eventCount", static_cast<long long>(result.event_count)},
        {"errorCount", static_cast<long long>(result.error_count)},
        {"lastBlockResult", result.last_block_result},
        {"events", nlohmann::json::array()},
        {"args", arg_overrides},
    };
    for (const auto& ev : result.events) {
        payload["events"].push_back({
            {"kind", ev.kind},
            {"status", ev.status},
            {"message", ev.message},
            {"data", ev.data},
            {"exitCode", ev.exit_code},
            {"path", ev.path},
        });
    }

    if (result.ok) {
        io.ok("xbloxRun", "XBlox script complete")
          .set("path", media::path::path_to_utf8(script_path))
          .set("exitCode", static_cast<long long>(result.exit_code))
          .result(Value::from_json(std::move(payload)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    io.error("xbloxRun", "XBlox script failed", result.exit_code)
      .set("path", media::path::path_to_utf8(script_path))
      .set("exitCode", static_cast<long long>(result.exit_code))
      .result(Value::from_json(std::move(payload)), ParamKind::json_value);
    return BlockResult::Error;
}

} // namespace

BlockHandler shell_block_handler(const std::string& kind)
{
    if (kind == "shell" || kind == "Shell")
        return shell_block;
    if (kind == "openPath")
        return open_path_block;
    if (kind == "xbloxRun")
        return xblox_run_block;
    return nullptr;
}

void register_shell_blocks(BlockRegistry& registry)
{
    const std::vector<ParamDef> shell_params = {
        pd_enum("mode", {"shell","powershell","cmd","bash","auto","argv"}).opt().dflt("shell")
            .desc("Shell interpreter: shell|powershell|cmd|bash|auto|argv."),
        pd_enum("shell", {"auto","cmd","powershell","bash","sh"}).adv().dflt("auto")
            .desc("Shell binary override. auto = platform default."),
        pd_str("command").in().req().dflt("echo hello").deep()
            .control("variable_text")
            .rows(3)
            .desc("Command or script to execute."),
        pd_args("args").in().dflt(nlohmann::json::array())
            .ui({{"visible_when", {{"mode", "argv"}}}})
            .desc("Argument tokens for argv mode. Each token supports variable interpolation and is quoted separately."),
        pd_str("cwd").in().dflt("").deep()
            .control("path_picker")
            .desc("Working directory (supports ${var} interpolation). Empty = default."),
        pd_ms("timeoutMs").adv()
            .dflt(media::xblox::constants::kShellDefaultTimeoutMs)
            .range(0, media::xblox::constants::kShellTimeoutMaxMs)
            .desc("Maximum execution time in milliseconds. 0 = no timeout."),
        pd_bool("log").adv().dflt(false)
            .desc("Echo stdout/stderr to the host logger."),
        pd_enum("stdout", {"trace","debug","info","warn","error","off"}).adv().dflt("info")
            .desc("Log level for stdout lines."),
        pd_enum("stderr", {"trace","debug","info","warn","error","off"}).adv().dflt("error")
            .desc("Log level for stderr lines."),
        pd_store_as().dflt("stdout"),
    };
    registry["shell"] = bdb(
        "shell", shell_block, "Shell", "Shell",
        "Run a shell command through the native RunTool.",
        {{"kind","shell"},{"mode","shell"},{"shell","auto"},{"command","echo hello"},{"args",nlohmann::json::array()},
         {"timeoutMs", media::xblox::constants::kShellDefaultTimeoutMs},{"log",false},{"stdout","info"},{"stderr","error"},{"storeAs","stdout"}})
        .params(shell_params)
        .dynamic_primary_output(
            ParamKind::string_, {ParamKind::string_}, "Stdout", true)
        .exec({true, true, true});
    registry["Shell"] = bdb(
        "Shell", shell_block, "Shell", "Shell",
        "Run a shell command through the native RunTool.",
        {{"kind","Shell"},{"mode","shell"},{"shell","auto"},{"command","echo hello"},{"args",nlohmann::json::array()},
         {"timeoutMs", media::xblox::constants::kShellDefaultTimeoutMs},{"log",false},{"stdout","info"},{"stderr","error"},{"storeAs","stdout"}})
        .params(shell_params)
        .dynamic_primary_output(
            ParamKind::string_, {ParamKind::string_}, "Stdout", true)
        .exec({true, true, true})
        .hidden();
    registry["openPath"] = bdb(
        "openPath", open_path_block, "Open Path", "Shell",
        "Open a file or folder with the OS default app, or inside Tanit.",
        {{"kind","openPath"},{"path",""},{"openIn","system"},{"storeAs","openedPath"}})
        .params({
            pd_file("path").in().req().deep()
                .ui({{"control","path_picker"}})
                .desc("File or folder path to open."),
            pd_enum("openIn", {"system", "app", "edit"}).dflt("system").lbl("Open in")
                .desc("system = OS default application. app = Tanit viewer (app command open). edit = Tanit editor (app command edit)."),
            pd_store_as().dflt("openedPath"),
        })
        .primary_output(ParamKind::file_path, "Opened path")
        .exec({false, false, true});

#if defined(FEATURE_XBLOX) && FEATURE_XBLOX \
 && defined(FEATURE_COMMAND_XBLOX) && FEATURE_COMMAND_XBLOX
    registry["xbloxRun"] = bdb(
        "xbloxRun", xblox_run_block, "XBlox Script", "Shell",
        "Run another XBlox document directly, with context overrides and optional document-loop controls.",
        {{"kind","xbloxRun"},{"action","Run"},{"path",""},{"context", nlohmann::json::object()},
         {"args", nlohmann::json::array()},{"reload",false},{"background",false},
         {"log",false},{"stdout","info"},{"stderr","error"},{"logLevel","off"},{"storeAs","xbloxResult"},
         {"items", nlohmann::json::array()}})
        .params({
            pd_enum("action", {"Run","Stop","Status"}).grp("lifecycle").dflt("Run")
                .desc("Run the script, Stop a background run keyed by path, or report Status."),
            pd_file("path").in().req().must_exist()
                .ui({{"control","path_picker"}, {"extension",".xblox"}, {"path_action","edit_script"}})
                .desc("XBlox document to run."),
            pd_json("context").in().dflt(nlohmann::json::object())
                .ui({{"control","xblox_context"},
                     {"script_path_field","path"},
                     {"options_path_template","xblox.scriptContext.{path}"}})
                .desc("Context variables merged into the child script before it runs."),
            pd_args("args").adv().dflt(nlohmann::json::array())
                .desc("Advanced CLI-style overrides, e.g. --CURRENT_FILE path or --name=value. Merged after context."),
            pd_bool("reload").adv().dflt(false)
                .desc("Reload the child script from disk before this run. Off reuses the run-scoped cached document."),
            pd_bool("loop").grp("loop").dflt(false)
                .desc("Override the child document loop setting and run it as a document loop."),
            pd_ms("loopIntervalMs").grp("loop").dflt(100)
                .range(0, media::xblox::constants::kLoopIntervalMaxMs)
                .desc("Loop interval override in milliseconds. 0 = as fast as possible."),
            pd_bool("loopReset").grp("loop").dflt(false)
                .desc("Reset child scope every loop pass instead of persisting variables."),
            pd_bool("background").adv().dflt(false)
                .desc("Start the child script on a detached background thread and return immediately."),
            pd_bool("log").adv().dflt(false)
                .desc("Forward captured child stdout/stderr lines to the parent xBlox run log."),
            pd_enum("stdout", {"trace","debug","info","warn","error","off"}).adv().dflt("info")
                .desc("Parent run-log level for child stdout lines."),
            pd_enum("stderr", {"trace","debug","info","warn","error","off"}).adv().dflt("error")
                .desc("Parent run-log level for child stderr lines."),
            pd_enum("logLevel", {"off","trace","debug","info","warn","error"}).adv().dflt("off")
                .desc("Optional host log level for child-run summaries and collected child events."),
            pd_store_as().dflt("xbloxResult"),
        })
        .primary_output(ParamKind::json_value, "Run result")
        .exec({true, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});
#endif
}

} // namespace media::xblox::blocks
