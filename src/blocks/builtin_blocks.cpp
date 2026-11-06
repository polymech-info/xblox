#include "blocks/builtin_blocks.hpp"
#include "json/json_utils.hpp"

#include "constants.hpp"
#include "blocks/app_blocks.hpp"
#include "blocks/browser_blocks.hpp"
#include "blocks/bluetooth_blocks.hpp"
#include "blocks/data_blocks.hpp"
#include "blocks/fs_blocks.hpp"
#if (defined(FEATURE_COMMAND_RESIZE) && FEATURE_COMMAND_RESIZE) \
 || (defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM) \
 || (defined(FEATURE_VIDEO) && FEATURE_VIDEO)
#include "blocks/image_blocks.hpp"
#endif
#include "blocks/input_blocks.hpp"
#include "blocks/json_query.hpp"
#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM
#if defined(PM_PLUGIN_CHAT) && PM_PLUGIN_CHAT
#include "plugins/chat/host_chat.hpp"
#else
#include "blocks/llm_blocks.hpp"
#endif
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA || defined(FEATURE_ONNX) && FEATURE_ONNX \
 || defined(FEATURE_WHISPER) && FEATURE_WHISPER || defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
#include "blocks/model_blocks.hpp"
#endif
#if defined(FEATURE_ONNX) && FEATURE_ONNX || defined(FEATURE_LLAMA) && FEATURE_LLAMA || defined(FEATURE_VLM) && FEATURE_VLM
#include "blocks/ocr_blocks.hpp"
#endif
#if defined(FEATURE_MQTT_XBLOX) && FEATURE_MQTT_XBLOX
#include "blocks/mqtt_blocks.hpp"
#endif
#if defined(FEATURE_MODBUS_XBLOX) && FEATURE_MODBUS_XBLOX
#include "blocks/modbus_blocks.hpp"
#endif
#if defined(FEATURE_SERVICE_XBLOX) && FEATURE_SERVICE_XBLOX
#include "blocks/service_blocks.hpp"
#endif
#if defined(FEATURE_VECTOR_XBLOX) && FEATURE_VECTOR_XBLOX
#include "blocks/vector_blocks.hpp"
#endif
#if defined(FEATURE_VECTOR_LOCAL_XBLOX) && FEATURE_VECTOR_LOCAL_XBLOX
#include "blocks/vector_blocks_local.hpp"
#endif
#include "blocks/network_blocks.hpp"
#include "blocks/shell_blocks.hpp"
#if PM_HAS_AUDIO_IO
#include "blocks/audio_blocks_tts_scripted.hpp"
#include "blocks/audio_blocks.hpp"
#include "blocks/audio_blocks_voice_commands.hpp"
#endif
#if (defined(FEATURE_VIDEO) && FEATURE_VIDEO) \
 || (defined(FEATURE_VIDEO_DETECTION) && FEATURE_VIDEO_DETECTION)
#include "blocks/video_blocks.hpp"
#endif
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "blocks/video_filter_blocks.hpp"
#include "blocks/video_vcam_blocks.hpp"
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA || defined(FEATURE_VLM) && FEATURE_VLM
#include "blocks/vision_blocks.hpp"
#endif
#include "blocks/block_variables.hpp"
#include "blocks/json_templates.hpp"
#include "fs/glob_paths.hpp"
#include "fs/path_sanitizer.hpp"
#include "logging/logger.h"
#include "policy/app_policy.hpp"
#include "xblox_constants.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

std::string json_string(const nlohmann::json& o, const char* key)
{
    return o.is_object() && o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string{};
}

std::string json_string_any(const nlohmann::json& o, std::initializer_list<const char*> keys, const std::string& fallback)
{
    if (o.is_object()) {
        for (const char* key : keys) {
            if (o.contains(key) && o[key].is_string() && !o[key].get<std::string>().empty())
                return o[key].get<std::string>();
        }
    }
    return fallback;
}

const nlohmann::json& json_array_or_empty(const nlohmann::json& o, const char* key)
{
    static const nlohmann::json empty = nlohmann::json::array();
    if (!o.is_object() || !o.contains(key) || !o[key].is_array())
        return empty;
    return o[key];
}

std::vector<std::string> json_string_array_local(const nlohmann::json& o, const char* key)
{
    std::vector<std::string> out;
    if (!o.is_object() || !o.contains(key) || !o[key].is_array())
        return out;
    for (const auto& item : o[key])
        if (item.is_string() && !item.get<std::string>().empty())
            out.push_back(item.get<std::string>());
    return out;
}

bool read_json_file_local(const std::filesystem::path& p, nlohmann::json& out, std::string& err)
{
    try {
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            err = "cannot open: " + media::path::path_to_utf8(p);
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        out = nlohmann::json::parse(ss.str());
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool write_json_file_local(const std::filesystem::path& p, const nlohmann::json& doc, std::string& err)
{
    std::filesystem::path tmp = p;
    tmp += media::path::path_from_utf8(".tmp~");
    try {
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) {
                err = "cannot open tmp: " + media::path::path_to_utf8(tmp);
                return false;
            }
            const std::string text = doc.dump(2) + "\n";
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!f) {
                err = "write failed: " + media::path::path_to_utf8(tmp);
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            err = "rename failed: " + ec.message();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        err = e.what();
        return false;
    }
}

// RAII scope frame for container bodies (S3). Pushes a child frame on the
// runtime's scope stack and pops it on every exit path (loops use Isolate so
// body-local variables are discarded while assignments to outer vars persist).
struct ScopeGuard {
    BlockRuntime& rt;
    bool active = false;
    ScopeGuard(BlockRuntime& r, ScopePolicy policy) : rt(r) {
        if (rt.push_scope) {
            rt.push_scope(rt.user_data, policy);
            active = true;
        }
    }
    ~ScopeGuard() {
        if (active && rt.pop_scope)
            rt.pop_scope(rt.user_data);
    }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
};

void emit_ok(BlockRuntime& runtime, ExecutionEvent event)
{
    if (runtime.events_enabled)
        runtime.emit(runtime.user_data, std::move(event));
    else
        record_muted_event(runtime.result, event, runtime.options.collect_error_events);
}

std::string indexed_child_path(const BlockRuntime& runtime, const std::string& path, const char* prefix, size_t index, const char* suffix)
{
    if (!runtime.events_enabled)
        return {};
    return path + prefix + std::to_string(index) + suffix;
}

std::string trim_ascii(std::string s)
{
    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool compare_numbers(double lhs, const std::string& comparator, double rhs)
{
    if (comparator == "!=" || comparator == "!==" || comparator == "<>")
        return std::abs(lhs - rhs) > 1e-12;
    if (comparator == ">" || comparator == "gt")
        return lhs > rhs;
    if (comparator == ">=" || comparator == "gte")
        return lhs >= rhs;
    if (comparator == "<" || comparator == "lt")
        return lhs < rhs;
    if (comparator == "<=" || comparator == "lte")
        return lhs <= rhs;
    return std::abs(lhs - rhs) <= 1e-12;
}

bool is_raw_constant_set_loop(const nlohmann::json& block, std::string& name, nlohmann::json& value)
{
    if (json_string(block, "condition") != "1")
        return false;
    const auto& items = json_array_or_empty(block, "items");
    if (!items.is_array() || items.size() != 1 || !items[0].is_object())
        return false;
    const auto& child = items[0];
    if (json_string(child, "kind") != "setVariable" || child.contains("expression") || !child.contains("value"))
        return false;
    name = json_string(child, "name");
    if (name.empty())
        return false;
    value = child["value"];
    return true;
}

int block_loop_limit(const nlohmann::json& block, const ExecutionOptions& options)
{
    const int fallback = std::max(1, options.max_loop_iterations);
    return std::max(0, block.value("loopLimit", fallback));
}

std::string normalize_log_level(std::string level)
{
    level = trim_ascii(std::move(level));
    level = media::strutil::lower_ascii(std::move(level));
    if (level.empty())
        return "trace";
    if (level == "warning")
        return "warn";
    if (level == "err")
        return "error";
    if (level == "critical")
        return "error";
    if (level == "off" || level == "none")
        return "off";
    if (level == "trace" || level == "debug" || level == "info" || level == "warn" || level == "error")
        return level;
    return "trace";
}

bool cli_log_level_off(const ExecutionOptions& options)
{
    static constexpr const char kLogLevelPrefix[] = "--log-level=";
    const auto& args = options.process_args;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--log-level") {
            if (i + 1 < args.size() && normalize_log_level(args[i + 1]) == "off")
                return true;
            ++i; // skip value if present
        } else if (arg.rfind(kLogLevelPrefix, 0) == 0) {
            if (normalize_log_level(arg.substr(sizeof(kLogLevelPrefix) - 1)) == "off")
                return true;
        }
    }
    return false;
}

std::string json_value_to_string(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_null())
        return "null";
    return value.dump();
}

bool block_has_json_query(const nlohmann::json& block)
{
    return block.is_object() &&
        (block.contains("input") || block.contains("from") || block.contains("filter") || block.contains("query"));
}

bool resolve_json_query_message(const nlohmann::json& block,
                                const std::string& path,
                                const char* kind,
                                BlockRuntime& runtime,
                                std::string& message)
{
    const std::string input_name = json_string_any(block, {"input", "from"}, "PREVIOUS");
    const nlohmann::json* input = runtime.context_value(runtime.user_data, input_name);
    if (!input) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, kind, "error", std::string(kind) + " input is missing: " + input_name, 2});
        return false;
    }

    const std::string filter = json_string_any(block, {"filter", "query"}, ".");
    nlohmann::json result;
    std::string err;
    if (!json_query::evaluate(*input, filter, result, err, {.string_fallback = true})) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, kind, "error", std::string(kind) + " jq failed: " + err, 2});
        return false;
    }

    const std::string format = json_string_any(block, {"format"}, "auto");
    message = (format == "json" || format == "JSON") ? result.dump() : json_value_to_string(result);
    return true;
}

void write_logger_line(const std::string& level, const std::string& message)
{
    if (level == "off" || message.empty())
        return;
    const std::string line = "[xblox] " + message;
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

bool wait_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};
    const int ms = static_cast<int>(std::max<long long>(0, io.i64("ms", 0)));
    if (runtime.options.wait_blocks && ms > 0) {
        int remaining = ms;
        while (remaining > 0 && !runtime.cancel_requested(runtime.user_data)) {
            const int slice = std::min(remaining, 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
        if (runtime.cancel_requested(runtime.user_data))
            return true;
    }
    runtime.emit(runtime.user_data, ExecutionEvent{path, "wait", "ok", "wait complete"});
    return true;
}

BlockResult log_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    BlockRuntime& runtime = io.runtime();
    const std::string& path = io.path();
    const std::string level = normalize_log_level(io.str("level", "info"));
    const bool silent = level == "off" || cli_log_level_off(runtime.options);
    std::string message;
    if (block_has_json_query(block)) {
        if (!resolve_json_query_message(block, path, "log", runtime, message))
            return BlockResult::Error;
    } else {
    const bool has_value   = block.is_object() && block.contains("value");
    message = io.str("message");
    if (message.empty() && has_value)
        message = json_value_to_string(block["value"]);

    if (message.empty()) {
        message = json_value_to_string(runtime.context);
    }
    }

    if (!silent)
        write_logger_line(level, message);
    nlohmann::json result = {{"level", level}, {"message", message}};
    io.ok("log", message).payload(std::move(result));
    return BlockResult::Ok;
}

BlockResult stdout_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    BlockRuntime& runtime = io.runtime();
    const std::string& path = io.path();
    std::string message;
    if (block_has_json_query(block)) {
        if (!resolve_json_query_message(block, path, "stdout", runtime, message))
            return BlockResult::Error;
    } else {
    const bool has_value   = block.is_object() && block.contains("value");
    message = io.str("message");
    if (message.empty() && has_value)
        message = json_value_to_string(block["value"]);

    if (message.empty()) {
        message = json_value_to_string(runtime.context);
    }
    }

    const bool newline = io.flag("newline", true);
    if (!runtime.options.quiet) {
        if (newline)
            std::cout << message << std::endl;
        else {
            std::cout << message;
            std::cout.flush();
        }
    }

    nlohmann::json result = {{"message", message}, {"newline", newline}};
    io.ok("stdout", message).payload(std::move(result));
    return BlockResult::Ok;
}

// Deliberate run termination with an authoritative process exit code. Marks
// exit_requested so the executor's generic Fatal bookkeeping (ok=false,
// exit_code=1) does not override an intentional `exit 0`.
BlockResult exit_block(BlockIO& io)
{
    const int code = static_cast<int>(io.i64("code", 0));
    std::string message = io.str("message");
    if (message.empty())
        message = "exit " + std::to_string(code);

    io.event("exit", "ok", message).set("code", Value{code});

    BlockRuntime& rt = io.runtime();
    rt.result.exit_requested = true;
    rt.result.ok = (code == 0);
    rt.result.exit_code = code;
    return BlockResult::Fatal;
}

// Read piped input from stdin. Binary input is fenced off for now (NUL bytes
// rejected); parse modes cover the remaining primitives � text (string), json
// (object/array/scalar), number (int/float � feeds muParser via the numeric
// store), boolean, and auto (JSON-scalar detection with text fallback).
BlockResult stdin_block(BlockIO& io)
{
    const std::string mode  = io.str("mode", "all");
    const std::string parse = io.str("parse", "text");
    const bool trim = io.flag("trim", true);

    std::string input;
    if (mode == "line") {
        if (!std::getline(std::cin, input)) {
            io.error("stdin", "stdin: no input available (EOF)", 1);
            return BlockResult::Error;
        }
        if (!input.empty() && input.back() == '\r')
            input.pop_back();
    } else {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        input = ss.str();
    }

    if (input.find('\0') != std::string::npos) {
        io.error("stdin", "stdin: binary input is not supported (NUL byte found)", 1)
          .set("bytes", Value{static_cast<long long>(input.size())});
        return BlockResult::Error;
    }

    if (trim) {
        const size_t end = input.find_last_not_of(" \t\r\n");
        input.erase(end == std::string::npos ? 0 : end + 1);
    }

    const long long bytes = static_cast<long long>(input.size());
    auto done = [&](const Value& v, ParamKind kind) {
        io.ok("stdin", "read " + std::to_string(bytes) + " byte(s) from stdin")
          .set("mode", Value{mode})
          .set("parse", Value{parse})
          .set("bytes", Value{bytes})
          .result(v, kind);
        return BlockResult::Ok;
    };
    auto preview = [&]() {
        return input.size() > 64 ? input.substr(0, 64) + "�" : input;
    };

    if (parse == "json") {
        nlohmann::json parsed = media::json::try_parse(input);
        if (parsed.is_discarded()) {
            io.error("stdin", "stdin: input is not valid JSON: " + preview(), 1)
              .set("bytes", Value{bytes});
            return BlockResult::Error;
        }
        return done(Value::from_json(std::move(parsed)), ParamKind::json_value);
    }

    if (parse == "number") {
        try {
            size_t pos = 0;
            const double d = std::stod(input, &pos);
            while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
                ++pos;
            if (pos != input.size() || !std::isfinite(d))
                throw std::invalid_argument("trailing characters");
            if (d == std::floor(d) && std::abs(d) < 9.2e18)
                return done(Value{static_cast<long long>(d)}, ParamKind::integer_);
            return done(Value{d}, ParamKind::float_);
        } catch (...) {
            io.error("stdin", "stdin: input is not a number: " + preview(), 1);
            return BlockResult::Error;
        }
    }

    if (parse == "boolean") {
        std::string s = input;
        s = media::strutil::lower_ascii(std::move(s));
        if (s == "true" || s == "1" || s == "yes" || s == "on")
            return done(Value{true}, ParamKind::boolean_);
        if (s == "false" || s == "0" || s == "no" || s == "off")
            return done(Value{false}, ParamKind::boolean_);
        io.error("stdin", "stdin: input is not a boolean (true/false/1/0/yes/no/on/off): " + preview(), 1);
        return BlockResult::Error;
    }

    if (parse == "auto") {
        // JSON scalar/structure detection; anything unparseable stays plain text.
        nlohmann::json parsed = media::json::try_parse(input);
        if (!parsed.is_discarded() && !input.empty()) {
            if (parsed.is_number_integer() || parsed.is_number_unsigned())
                return done(Value{parsed.get<long long>()}, ParamKind::integer_);
            if (parsed.is_number_float())
                return done(Value{parsed.get<double>()}, ParamKind::float_);
            if (parsed.is_boolean())
                return done(Value{parsed.get<bool>()}, ParamKind::boolean_);
            if (parsed.is_object() || parsed.is_array())
                return done(Value::from_json(std::move(parsed)), ParamKind::json_value);
            if (parsed.is_string())
                return done(Value{parsed.get<std::string>()}, ParamKind::string_);
        }
        return done(Value{input}, ParamKind::string_);
    }

    return done(Value{input}, ParamKind::string_); // text (default)
}

bool break_block(const nlohmann::json&, const std::string& path, BlockRuntime& runtime)
{
    if (runtime.loop_depth && *runtime.loop_depth <= 0) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "break", "warning", "break outside a loop  -  ignored"});
        return true;
    }
    runtime.break_requested = true;
    runtime.emit(runtime.user_data, ExecutionEvent{path, "break", "ok", "break requested"});
    return true;
}

// RAII for loop handlers: tracks loop depth (for `break` orphan detection) and
// clears break_requested on exit so `break` only exits the nearest loop  - 
// execution continues after it (docs/xblox/language.md D3). A Fatal abort is
// tracked separately via FlowState::abort_requested, so clearing is safe.
struct LoopGuard {
    BlockRuntime& rt;
    explicit LoopGuard(BlockRuntime& r) : rt(r) { if (rt.loop_depth) ++*rt.loop_depth; }
    ~LoopGuard() { if (rt.loop_depth) --*rt.loop_depth; rt.break_requested = false; }
};

BlockResult set_variable_block(BlockIO& io)
{
    BlockRuntime& runtime = io.runtime();
    const nlohmann::json& block = io.raw();
    const std::string name = bv::resolve(io.str("name"), runtime);
    if (name.empty()) {
        io.error("setVariable", "setVariable is missing name", 2);
        return BlockResult::Error;
    }

    nlohmann::json value = nullptr;
    const std::string value_from = json_string(block, "valueFrom").empty() ? json_string(block, "from") : json_string(block, "valueFrom");
    if (!value_from.empty()) {
        if (const nlohmann::json* found = runtime.context_value(runtime.user_data, value_from))
            value = *found;
    } else if (block.contains("expression") && block["expression"].is_string() && !block["expression"].get<std::string>().empty()) {
        const std::string expression = trim_ascii(block["expression"].get<std::string>());
        if (const nlohmann::json* found = runtime.context_value(runtime.user_data, expression)) {
            value = *found;
        } else {
            double evaluated = 0.0;
            std::string err;
            if (!runtime.eval_expression(runtime.user_data, expression, evaluated, &err)) {
                io.error("setVariable", "setVariable expression failed: " + err, 2);
                return BlockResult::Error;
            }
            value = evaluated;
        }
    } else if (block.contains("value")) {
        value = resolve_json_templates(io, block["value"]);
    }

    const auto empty_scalar = [](const nlohmann::json& v) {
        if (v.is_null())
            return true;
        if (v.is_string())
            return trim_ascii(v.get<std::string>()).empty();
        return false;
    };
    const std::string fallback_from = json_string(block, "fallbackFrom").empty()
        ? json_string(block, "fallback")
        : json_string(block, "fallbackFrom");
    if (empty_scalar(value) && !fallback_from.empty()) {
        if (const nlohmann::json* found = runtime.context_value(runtime.user_data, fallback_from))
            value = *found;
    }

    runtime.set_context_value(runtime.user_data, name, value);
    io.ok("setVariable", "variable set")
      .field("name", name)
      .field("value", Value::from_json(value))
      .result(Value::from_json(std::move(value)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult get_variable_block(BlockIO& io)
{
    BlockRuntime& runtime = io.runtime();
    const std::string name = io.str("name");
    if (name.empty()) {
        io.error("getVariable", "getVariable is missing name", 2);
        return BlockResult::Error;
    }

    const nlohmann::json* found = runtime.context_value(runtime.user_data, name);
    nlohmann::json value = found ? *found : nullptr;
    const std::string target = io.str("target");
    io.ok("getVariable", "variable read")
      .field("name", name)
      .field("target", target)
      .field("value", Value::from_json(value))
      .result(Value::from_json(std::move(value)), ParamKind::json_value);
    return BlockResult::Ok;
}

bool save_state_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    std::filesystem::path target = runtime.options.source_path;
    if (target.empty()) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "saveState", "error", "saveState needs a source path", 2});
        return true;
    }
    if (target.is_relative() && !runtime.options.default_cwd.empty())
        target = runtime.options.default_cwd / target;

    nlohmann::json doc;
    std::string err;
    if (!read_json_file_local(target, doc, err)) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "saveState", "error", "saveState: " + err, 2});
        return true;
    }
    if (!doc.is_object())
        doc = nlohmann::json::object();
    nlohmann::json& saved_context = doc["context"];
    if (!saved_context.is_object())
        saved_context = nlohmann::json::object();

    const std::string mode = json_string(block, "mode").empty() ? "existing" : json_string(block, "mode");
    std::vector<std::string> keys = json_string_array_local(block, "keys");
    if (keys.empty()) {
        if (mode == "all") {
            if (runtime.context.is_object())
                for (auto it = runtime.context.begin(); it != runtime.context.end(); ++it)
                    keys.push_back(it.key());
        } else {
            for (auto it = saved_context.begin(); it != saved_context.end(); ++it)
                keys.push_back(it.key());
        }
    }

    nlohmann::json persisted = nlohmann::json::object();
    for (const std::string& key : keys) {
        if (const nlohmann::json* value = runtime.context_value(runtime.user_data, key)) {
            saved_context[key] = *value;
            persisted[key] = *value;
        }
    }

    if (runtime.options.simulate) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "saveState", "ok", "state save simulated", 0,
                                                       {{"path", media::path::path_to_utf8(target)}, {"count", persisted.size()}, {"result", persisted}}});
        runtime.set_context_value(runtime.user_data, "PREVIOUS", persisted);
        return true;
    }

    if (!write_json_file_local(target, doc, err)) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "saveState", "error", "saveState: " + err, 2});
        return true;
    }

    runtime.set_context_value(runtime.user_data, "PREVIOUS", persisted);
    runtime.emit(runtime.user_data, ExecutionEvent{path, "saveState", "ok", "state saved", 0,
                                                   {{"path", media::path::path_to_utf8(target)}, {"count", persisted.size()}, {"result", persisted}}});
    return true;
}

// Sibling-chain if/elseIf/else (docs/xblox/language.md D1): `if` opens a chain
// in the current list; contiguous `elseIf`/`else` siblings continue it. The
// list runner owns ChainState and resets it on any other kind; handlers only
// read/update it through runtime.chain.
bool if_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};
    const bool matched = runtime.condition_truthy(runtime.user_data, io.str("condition"));
    if (matched)
        io.run("consequent");
    if (runtime.chain) {
        runtime.chain->active = true;
        runtime.chain->matched = matched;
    }
    emit_ok(runtime, ExecutionEvent{path, "if", runtime.result.ok ? "ok" : "error",""});
    return true;
}

bool else_if_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};
    ChainState* chain = runtime.chain;
    if (!chain || !chain->active) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "elseIf", "warning",
                                                       "orphan elseIf  -  no preceding if/elseIf sibling"});
        return true;
    }
    if (chain->matched) {
        emit_ok(runtime, ExecutionEvent{path, "elseIf", "skipped", "chain already matched"});
        return true;
    }
    const bool matched = runtime.condition_truthy(runtime.user_data, io.str("condition"));
    if (matched) {
        io.run("items");
        chain->matched = true;
    }
    emit_ok(runtime, ExecutionEvent{path, "elseIf", runtime.result.ok ? "ok" : "error",
                                    matched ? "elseIf matched" : "elseIf not matched"});
    return true;
}

bool else_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};
    ChainState* chain = runtime.chain;
    if (!chain || !chain->active) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "else", "warning",
                                                       "orphan else  -  no preceding if/elseIf sibling"});
        return true;
    }
    const bool run_body = !chain->matched;
    if (run_body)
        io.run("items");
    chain->matched = true;
    chain->active = false; // chain consumed  -  a following else/elseIf is an orphan
    emit_ok(runtime, ExecutionEvent{path, "else", run_body ? (runtime.result.ok ? "ok" : "error") : "skipped",
                                    run_body ? "else ran" : "chain already matched"});
    return true;
}

// `case` / `switchDefault` bodies are driven by the enclosing switch_block; a
// directly dispatched one was moved outside a switch  -  allowed by design in
// the UI, a no-op warning at run time (docs/xblox/language.md D2).
bool case_orphan_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    const std::string kind = json_string(block, "kind");
    runtime.emit(runtime.user_data, ExecutionEvent{path, kind, "warning", kind + " outside a switch  -  skipped"});
    return true;
}

bool for_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};
    const std::string variable = io.str("variable", "i");
    const std::string comparator = io.str("comparator");
    const std::string modifier = trim_ascii(io.str("modifier"));
    const int limit = block_loop_limit(block, runtime.options);

    double current = 0.0;
    double final_value = 0.0;
    std::string err;
    if (!runtime.eval_expression(runtime.user_data, io.str("initial"), current, &err)) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "for", "error", "for initial expression failed: " + err, 2});
        return true;
    }
    if (!runtime.eval_expression(runtime.user_data, io.str("final"), final_value, &err)) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "for", "error", "for final expression failed: " + err, 2});
        return true;
    }

    // Loop counter + body locals live in an isolated child frame: assignments to
    // outer variables (e.g. accumulators) still update the parent via the scope
    // chain, but the counter and freshly-declared body vars are discarded on exit.
    ScopeGuard guard(runtime, ScopePolicy::Isolate);
    LoopGuard loop_guard(runtime);

    int iterations = 0;
    while (iterations < limit && compare_numbers(current, comparator, final_value) && !runtime.break_requested && !runtime.cancel_requested(runtime.user_data)) {
        runtime.set_context_value(runtime.user_data, variable, current);
        runtime.set_context_value(runtime.user_data, "loop.index", iterations);
        io.run("items");
        if (runtime.break_requested)
            break;

        if (modifier.empty()) {
            current += 1.0;
        } else if (modifier[0] == '+' || modifier[0] == '-') {
            double delta = 0.0;
            if (!runtime.eval_expression(runtime.user_data, modifier, delta, &err)) {
                runtime.emit(runtime.user_data, ExecutionEvent{path, "for", "error", "for modifier expression failed: " + err, 2});
                return true;
            }
            current += delta;
        } else {
            runtime.set_context_value(runtime.user_data, variable, current);
            double next = 0.0;
            if (!runtime.eval_expression(runtime.user_data, modifier, next, &err)) {
                runtime.emit(runtime.user_data, ExecutionEvent{path, "for", "error", "for modifier expression failed: " + err, 2});
                return true;
            }
            current = next;
        }
        ++iterations;
    }
    runtime.set_context_value(runtime.user_data, variable, current);
    emit_ok(runtime, ExecutionEvent{path, "for", runtime.result.ok ? "ok" : "error", "for body complete", 0, {{"iterations", iterations}}});
    return true;
}

bool while_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    const int limit = block_loop_limit(block, runtime.options);
    std::string raw_name;
    nlohmann::json raw_value;
    if (!runtime.events_enabled && !runtime.break_requested && is_raw_constant_set_loop(block, raw_name, raw_value)) {
        constexpr int kCancelPollInterval = media::xblox::constants::kWhileFastPathCancelPollInterval;
        int iterations = 0;
        volatile double raw_sink = 0.0;
        double raw_number = 0.0;
        if (raw_value.is_number())
            raw_number = raw_value.get<double>();
        else if (raw_value.is_boolean())
            raw_number = raw_value.get<bool>() ? 1.0 : 0.0;
        while (iterations < limit && !runtime.break_requested) {
            const int remaining = limit - iterations;
            const int chunk = std::min(kCancelPollInterval, remaining);
            for (int i = 0; i < chunk; ++i) {
                raw_sink = raw_number;
                runtime.result.event_count += 2;
                ++iterations;
            }
            if (runtime.cancel_requested(runtime.user_data))
                break;
        }
        (void)raw_sink;
        runtime.set_context_value(runtime.user_data, raw_name, std::move(raw_value));
        emit_ok(runtime, ExecutionEvent{path, "while", runtime.result.ok ? "ok" : "error", "while raw body complete", 0, {{"iterations", iterations}}});
        return true;
    }

    BlockIO io{block, path, runtime};
    const std::string condition = io.str("condition"); // constant across iterations
    ScopeGuard guard(runtime, ScopePolicy::Isolate);
    LoopGuard loop_guard(runtime);
    int iterations = 0;
    for (; iterations < limit && runtime.condition_truthy(runtime.user_data, condition) && !runtime.break_requested && !runtime.cancel_requested(runtime.user_data); ++iterations)
        io.run("items");
    emit_ok(runtime, ExecutionEvent{path, "while", runtime.result.ok ? "ok" : "error", "while body complete", 0, {{"iterations", iterations}}});
    return true;
}

bool switch_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};
    const std::string switch_expr = io.str("variable");
    double switch_number = 0.0;
    const bool has_switch_number = runtime.numeric_value(runtime.user_data, switch_expr, switch_number);
    const std::string switch_value = has_switch_number ? std::string{} : runtime.string_value(runtime.user_data, switch_expr);
    // `items` is a list of case/switchDefault objects (structured), each opened
    // by case_matches � it stays on the raw boundary.
    const auto& items = json_array_or_empty(io.raw(), "items");
    int fallback = -1;
    int matched = -1;
    if (items.is_array()) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (json_string(items[i], "kind") == "switchDefault")
                fallback = static_cast<int>(i);
            else if (matched < 0 && runtime.case_matches(runtime.user_data, items[i], switch_number, has_switch_number, switch_value))
                matched = static_cast<int>(i);
        }
    }
    const int index = matched >= 0 ? matched : fallback;
    if (index >= 0)
        runtime.execute_block_list(runtime.user_data, json_array_or_empty(items[static_cast<size_t>(index)], "consequent"),
                                   indexed_child_path(runtime, path, "/items/", static_cast<size_t>(index), "/consequent"));
    emit_ok(runtime, ExecutionEvent{path, "switch", runtime.result.ok ? "ok" : "error", "switch complete"});
    return true;
}

bool group_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    // Transparent container: runs its child blocks in the current (inherited)
    // scope � no isolation, no own behaviour. This is also the landing spot for
    // the deprecated `runScript` kind, so legacy documents degrade into a plain
    // group instead of a dead no-op.
    BlockIO io{block, path, runtime};
    const std::string kind = io.str("kind");
    io.run("items");
    emit_ok(runtime, ExecutionEvent{path, kind.empty() ? "group" : kind, runtime.result.ok ? "ok" : "error", "group complete"});
    return true;
}

BlockResult on_start_block(BlockIO& io)
{
    io.ok("onStart", "lifecycle hook registered").set("event", Value{io.str("event", "On Start")});
    return BlockResult::Ok;
}

} // namespace

// Diagnostic logging sink behind BlockIO::log()/trace()/debug()/info() � keeps
// the logger dependency out of block_types.hpp. Defined at namespace scope (not
// in the anonymous namespace above) so other block TUs can link against it.
void block_log(const char* level, const std::string& message)
{
    write_logger_line(level ? level : "trace", message);
}

static void validate_registered_block_descriptors(const BlockRegistry& registry)
{
    auto fail = [](const BlockDescriptor& descriptor, const std::string& message) {
        throw std::logic_error(
            "invalid output contract for block '" + descriptor.kind + "': " + message);
    };

    for (const auto& [registry_kind, descriptor] : registry) {
        if (descriptor.kind.empty())
            fail(descriptor, "kind is empty (registry key '" + registry_kind + "')");

        std::unordered_set<std::string> slots;
        std::unordered_set<std::string> exports;
        std::size_t primary_count = 0;
        std::size_t previous_writer_count = 0;

        for (const OutputDef& output : descriptor.output_defs) {
            if (output.slot.empty())
                fail(descriptor, "output slot is empty");
            if (!slots.insert(output.slot).second)
                fail(descriptor, "duplicate output slot '" + output.slot + "'");

            if (output.primary) {
                ++primary_count;
                if (output.slot != "result")
                    fail(descriptor, "primary output slot must be 'result', got '" + output.slot + "'");
            }

            if (output.writes_previous) {
                ++previous_writer_count;
                if (!output.primary)
                    fail(descriptor, "only the primary output may write PREVIOUS");
            }

            const bool fixed_export = !output.export_name.empty();
            const bool param_export = !output.export_from_param.empty();
            if (fixed_export && param_export)
                fail(descriptor, "slot '" + output.slot + "' mixes fixed and parameter-derived exports");
            if (output.scope_export != (fixed_export || param_export))
                fail(descriptor, "scopeExport metadata disagrees for slot '" + output.slot + "'");
            if (!output.export_name.empty()) {
                if (output.export_name == "PREVIOUS" ||
                    output.export_name.find("${") != std::string::npos ||
                    std::all_of(output.export_name.begin(), output.export_name.end(),
                                [](unsigned char c) { return std::isspace(c) != 0; }))
                    fail(descriptor, "exportName for slot '" + output.slot + "' is not a fixed scope name");
                if (!exports.insert(output.export_name).second)
                    fail(descriptor, "duplicate exportName '" + output.export_name + "'");
            }
            if (param_export) {
                if (std::all_of(output.export_from_param.begin(), output.export_from_param.end(),
                                [](unsigned char c) { return std::isspace(c) != 0; }) ||
                    output.export_suffix.find("${") != std::string::npos)
                    fail(descriptor, "invalid parameter-derived export for slot '" + output.slot + "'");
                const bool export_param_declared = std::any_of(
                    descriptor.param_defs.begin(), descriptor.param_defs.end(),
                    [&](const ParamDef& param) { return param.name == output.export_from_param; });
                if (!export_param_declared)
                    fail(descriptor, "exportFromParam '" + output.export_from_param +
                                     "' is not a declared parameter");
                const std::string export_key =
                    "param:" + output.export_from_param + ":" + output.export_suffix;
                if (!exports.insert(export_key).second)
                    fail(descriptor, "duplicate parameter-derived export for slot '" + output.slot + "'");
            } else if (!output.export_suffix.empty()) {
                fail(descriptor, "exportSuffix requires exportFromParam for slot '" + output.slot + "'");
            }

            if (output.dynamic) {
                std::unordered_set<ParamKind> supported;
                for (const ParamKind kind : output.supported_kinds) {
                    if (!supported.insert(kind).second)
                        fail(descriptor, "duplicate supported kind for dynamic slot '" + output.slot + "'");
                }
                if (supported.empty() || supported.find(output.kind) == supported.end())
                    fail(descriptor, "dynamic slot '" + output.slot + "' must include its fallback kind");
            } else if (!output.supported_kinds.empty()) {
                fail(descriptor, "non-dynamic slot '" + output.slot + "' declares supported kinds");
            }
        }

        if (primary_count > 1)
            fail(descriptor, "multiple primary outputs");
        if (!descriptor.output_defs.empty() && primary_count != 1)
            fail(descriptor, "output-producing block must declare exactly one primary output");

        if (descriptor.previous_behavior == PreviousBehavior::Writes) {
            if (primary_count != 1 || previous_writer_count != 1)
                fail(descriptor, "previousBehavior=writes requires one primary PREVIOUS-writing output");
        } else if (previous_writer_count != 0) {
            fail(descriptor, "writesPrevious requires previousBehavior=writes");
        }

        if (descriptor.palette_visible &&
            descriptor.output_defs.empty() &&
            descriptor.previous_behavior == PreviousBehavior::Unknown)
            fail(descriptor, "GUI-visible block must declare outputs or previous behavior");
    }
}

// Built once and cached: the registry is read-only after init, so lookups (and
// the stable param_defs pointers handed to the executor) are safe to share,
// including across background-block worker threads.
static const BlockRegistry& cached_registry()
{
    static const BlockRegistry registry = [] {
        BlockRegistry r;
        register_builtin_blocks(r);
        return r;
    }();
    return registry;
}

// Returns false when a GPO policy disables the entire group that block belongs to.
// Called at both run-time (handler dispatch) and list-time (registered_block_definitions).
//
// Group → policy mapping:
//   "Audio"  → CLI.EnableAudio
//   "Video"  → CLI.EnableVideo
//   "OCR"    → Local.EnableOnnx
//   "Vision" → Local.EnableLlama  (VLM / llama.cpp multimodal)
//   "Vector" → Local.EnableLlama  (local GGUF embeddings)
static bool is_block_group_allowed(const std::string& group)
{
    if (group == "AI") {
#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM
        return true;
#else
        return false;
#endif
    }
    if (group == "Audio")  return pm::policy::cli_group_allowed("audio");
    if (group == "Video")  return pm::policy::cli_group_allowed("video");
    if (group == "OCR")    return pm::policy::local_onnx_enabled();
    if (group == "Vision") return pm::policy::local_llama_enabled();
    if (group == "Vector") return pm::policy::local_llama_enabled();
    return true;
}

BlockHandler builtin_block_handler(const std::string& kind)
{
    const BlockRegistry& registry = cached_registry();
    const auto it = registry.find(kind);
    if (it == registry.end()) return nullptr;
    if (!is_block_group_allowed(it->second.group)) return nullptr;
    if (!network_block_allowed(kind)) return nullptr;
    return it->second.handler;
}

const std::vector<ParamDef>* builtin_block_param_defs(const std::string& kind)
{
    const BlockRegistry& registry = cached_registry();
    const auto it = registry.find(kind);
    return it == registry.end() ? nullptr : &it->second.param_defs;
}

const BlockDescriptor* builtin_block_descriptor(const std::string& kind)
{
    const BlockRegistry& registry = cached_registry();
    const auto it = registry.find(kind);
    return it == registry.end() ? nullptr : &it->second;
}

const ExecFlags* builtin_block_exec_flags(const std::string& kind)
{
    const BlockRegistry& registry = cached_registry();
    const auto it = registry.find(kind);
    return it == registry.end() ? nullptr : &it->second.exec_flags;
}

// ?????????????????????????????????????????????????????????????????????????????
// Parameter transform/cast pipeline (the "zod-like" stage)
//
// Given the *sourced* raw json for a param, produce a typed Value that obeys
// the schema declared on the ParamDef: numeric kinds coerce + clamp to range,
// enum validates membership, booleans normalise, json_value stays opaque, and
// everything else collapses to a string. Validation issues are recorded in
// `diag` (param-prefixed) rather than aborting � strict/simulate modes can act
// on them later; normal execution remains best-effort with a sensible value.
// ?????????????????????????????????????????????????????????????????????????????
namespace {

std::string param_scalar_to_string(const nlohmann::json& j)
{
    if (j.is_string())          return j.get<std::string>();
    if (j.is_boolean())         return j.get<bool>() ? "true" : "false";
    if (j.is_number_integer())  return std::to_string(j.get<long long>());
    if (j.is_number_unsigned()) return std::to_string(j.get<unsigned long long>());
    if (j.is_number_float())    return nlohmann::json(j.get<double>()).dump();
    if (j.is_null())            return {};
    return j.dump();
}

Value transform_param(const ParamDef& d, const nlohmann::json* raw,
                      std::vector<Diagnostic>& diag)
{
    auto note = [&](const std::string& msg) { diag.push_back(Diagnostic{d.name, msg, 1}); };

    // Nothing sourced and no default ? leave unset; accessors return their dflt.
    if (!raw || raw->is_null())
        return Value{};
    const nlohmann::json& j = *raw;

    switch (d.kind) {
    case ParamKind::integer_:
    case ParamKind::duration_ms:
    case ParamKind::flags: {
        long long n = 0; bool ok = true;
        if      (j.is_number_integer())  n = j.get<long long>();
        else if (j.is_number_unsigned()) n = static_cast<long long>(j.get<unsigned long long>());
        else if (j.is_number_float())    n = static_cast<long long>(j.get<double>());
        else if (j.is_boolean())         n = j.get<bool>() ? 1 : 0;
        else if (j.is_string()) { try { n = std::stoll(j.get<std::string>()); } catch (...) { ok = false; } }
        else ok = false;
        if (!ok) { note("expected integer, got " + param_scalar_to_string(j)); return Value{}; }
        if (d.has_min && n < static_cast<long long>(d.min_val)) { note("below min"); n = static_cast<long long>(d.min_val); }
        if (d.has_max && n > static_cast<long long>(d.max_val)) { note("above max"); n = static_cast<long long>(d.max_val); }
        return Value{n};
    }
    case ParamKind::float_: {
        double x = 0; bool ok = true;
        if      (j.is_number())  x = j.get<double>();
        else if (j.is_boolean()) x = j.get<bool>() ? 1.0 : 0.0;
        else if (j.is_string())  { try { x = std::stod(j.get<std::string>()); } catch (...) { ok = false; } }
        else ok = false;
        if (!ok) { note("expected number, got " + param_scalar_to_string(j)); return Value{}; }
        if (d.has_min && x < d.min_val) { note("below min"); x = d.min_val; }
        if (d.has_max && x > d.max_val) { note("above max"); x = d.max_val; }
        return Value{x};
    }
    case ParamKind::boolean_: {
        if (j.is_boolean()) return Value{j.get<bool>()};
        if (j.is_number())  return Value{j.get<double>() != 0.0};
        if (j.is_string())  { const std::string s = j.get<std::string>(); return Value{s == "true" || s == "1" || s == "yes" || s == "on"}; }
        return Value{false};
    }
    case ParamKind::enum_: {
        const std::string s = j.is_string() ? j.get<std::string>() : param_scalar_to_string(j);
        if (!d.enum_values.empty() &&
            std::find(d.enum_values.begin(), d.enum_values.end(), s) == d.enum_values.end())
            note("\"" + s + "\" is not one of the allowed values");
        return Value{s};
    }
    case ParamKind::json_value:
    case ParamKind::stt_route:
    case ParamKind::args_list:
        // Structured input � keep it opaque for the handler to open via json().
        return Value::from_json(j);
    default:
        // string_, *_path, color_, device_name, prompt � all string-shaped.
        // ${var} interpolation is applied centrally by resolve_block_inputs.
        return Value{ j.is_string() ? j.get<std::string>() : param_scalar_to_string(j) };
    }
}

// Typed kinds that accept "${var}" document literals: interpolate before the
// kind-specific transform so "${hud}" expands to "true" and then becomes bool.
bool is_variable_templatable(ParamKind k)
{
    switch (k) {
    case ParamKind::boolean_:
    case ParamKind::enum_:
    case ParamKind::integer_:
    case ParamKind::duration_ms:
    case ParamKind::flags:
    case ParamKind::float_:
        return true;
    default:
        return false;
    }
}

// String-shaped kinds whose values carry ${var} placeholders the resolver
// expands after transform (scalar -> string). Excludes expression_ (its own
// evaluator), variable_ref (a name), and structured kinds. Typed kinds use
// is_variable_templatable() and interpolate before transform instead.
bool is_interpolatable(ParamKind k)
{
    switch (k) {
    case ParamKind::string_:
    case ParamKind::file_path:  case ParamKind::dir_path:
    case ParamKind::audio_path: case ParamKind::image_path:
    case ParamKind::video_path: case ParamKind::output_path:
    case ParamKind::color_:     case ParamKind::device_name:
    case ParamKind::video_input:
    case ParamKind::screen_input:
    case ParamKind::prompt:
        return true;
    default:
        return false;
    }
}

// Path-shaped kinds are *locations*, typically document-authored templates �
// they get ParamResolve::Deep implicitly so a context variable holding
// "${KNOWNFOLDER:Desktop}/scans" expands fully. Free-text kinds (string_,
// prompt, �) stay single-pass on purpose: they carry untrusted runtime data
// (${PREVIOUS} passthroughs) that must remain data, not become templates.
bool is_path_kind(ParamKind k)
{
    switch (k) {
    case ParamKind::file_path:  case ParamKind::dir_path:
    case ParamKind::audio_path: case ParamKind::image_path:
    case ParamKind::video_path: case ParamKind::output_path:
        return true;
    default:
        return false;
    }
}

// Expand a filesystem glob pattern into a sorted list of matching paths.
// The pattern is split into a literal base directory (the longest prefix with
// no glob token) and a glob remainder; the base is enumerated and each entry
// matched against the full pattern via the shared media glob matcher so `*`/`?`
// stay single-segment and `**` spans depth. Returns an empty list when the base
// is absent � the resolver surfaces "no matches" as an empty array, not an error.
std::vector<std::string> expand_glob(const std::string& pattern)
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;

    std::string pat = pattern;
    std::replace(pat.begin(), pat.end(), '\\', '/');

    const std::size_t first_glob = pat.find_first_of("*?");
    const std::size_t base_end =
        pat.rfind('/', first_glob == std::string::npos ? std::string::npos : first_glob);
    std::string base = (base_end == std::string::npos) ? std::string{"."} : pat.substr(0, base_end);
    if (base.empty()) base = "/";

    std::error_code ec;
    const fs::path basep = media::path::path_from_utf8(base);
    if (!fs::exists(basep, ec) || !fs::is_directory(basep, ec)) return out;

    const bool recursive = pat.find("**") != std::string::npos;
    auto consider = [&](const fs::path& p) {
        if (media::path_matches_path_glob(media::path::path_to_generic_utf8(p), pat))
            out.push_back(media::path::path_to_generic_utf8(p));
    };

    if (recursive) {
        for (fs::recursive_directory_iterator it(basep, ec), end; !ec && it != end; it.increment(ec))
            consider(it->path());
    } else {
        for (fs::directory_iterator it(basep, ec), end; !ec && it != end; it.increment(ec))
            consider(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Run app-level resolve hooks (ParamResolve::Custom) against a resolved value.
// Hooks are registered at xblox initiation in ExecutionOptions::resolve_hooks and
// may rewrite the value and/or veto it. A scalar veto pushes the hook's diagnostic
// (severity 2 ? the executor skips the block); for a glob-expanded list a veto
// filters that single entry as a non-fatal advisory so the remaining matches flow.
void apply_resolve_hooks(const ParamDef& d, const std::string& block_kind,
                         Value& v, BlockRuntime& rt, std::vector<Diagnostic>& diag)
{
    const auto& hooks = rt.options.resolve_hooks;
    if (hooks.empty() || v.is_null()) return;

    ResolveHookInput in;
    in.block_kind = block_kind;
    in.param      = d.name;
    in.param_kind = param_kind_str(d.kind);
    in.writable   = constraint_has(d.constraints, ParamConstraint::Writable) ||
                    constraint_has(d.constraints, ParamConstraint::CreateParents);
    in.simulate   = rt.options.simulate;

    struct Verdict { std::string value; bool denied = false; int severity = 2; std::string message; };
    const auto run_chain = [&](std::string value) -> Verdict {
        Verdict r; r.value = std::move(value);
        for (const auto& hook : hooks) {
            in.value = r.value;
            const ResolveHookOutput out = hook(in);
            if (out.rewrite) r.value = out.value;
            if (out.deny) {
                r.denied = true;
                r.severity = out.severity;
                r.message = out.message.empty() ? "denied by policy" : out.message;
                break; // first veto wins
            }
        }
        return r;
    };

    if (v.is_array()) {
        const nlohmann::json arr = v.to_json();
        nlohmann::json kept = nlohmann::json::array();
        for (const auto& el : arr) {
            if (!el.is_string()) { kept.push_back(el); continue; }
            Verdict r = run_chain(el.get<std::string>());
            if (r.denied)
                diag.push_back(Diagnostic{d.name, "filtered: " + r.message, 1}); // advisory, keep listing
            else
                kept.push_back(r.value);
        }
        v = Value::from_json(kept);
    } else {
        Verdict r = run_chain(v.as_string());
        v = Value{ r.value };
        if (r.denied)
            diag.push_back(Diagnostic{d.name, r.message, r.severity});
    }
}

// Validate a resolved value against its declared ParamConstraint preconditions.
// Cheap, read-only filesystem probes (existence/type) run in every mode;
// CreateParents is the lone effectful flag and is gated on !simulate. Findings
// are pushed as Diagnostics � severity 2 makes the executor skip the handler.
// (Host-mediated/async constraints like RequiresPermission are intentionally
// not handled here; a future capability gate in the executor owns those.)
void validate_constraints(const ParamDef& d, const Value& v, BlockRuntime& rt,
                          std::vector<Diagnostic>& diag)
{
    const ParamConstraint c = d.constraints;
    if (c == ParamConstraint::None) return;

    auto fail = [&](const std::string& m) { diag.push_back(Diagnostic{d.name, m, 2}); };

    const std::string s = v.is_null() ? std::string{} : v.as_string();

    // A value is *required* only for constraints whose whole purpose is presence
    // (NonEmpty) or that assert something about the referenced path existing
    // (MustExist / IsFile / IsDir). Writable / CreateParents / ParentExists are
    // "validate the location *if* a path is given" checks: an empty output path is
    // a legitimate "no output / auto temp file" signal, so they must NOT force a
    // value. (Baking Writable into pd_outpath otherwise made every optional output
    // path fail "required but empty".) Required output paths still opt in via
    // .req()/.nonempty().
    const bool needs_value =
        constraint_has(c, ParamConstraint::NonEmpty) || constraint_has(c, ParamConstraint::MustExist) ||
        constraint_has(c, ParamConstraint::IsFile)   || constraint_has(c, ParamConstraint::IsDir);
    if (s.empty()) {
        if (needs_value) fail("is required but empty");
        return;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p = media::path::path_from_utf8(s);
    const bool exists = fs::exists(p, ec);

    if (constraint_has(c, ParamConstraint::MustExist) && !exists) {
        fail("path does not exist: " + s);
        return; // dependent checks are moot
    }
    if (exists && constraint_has(c, ParamConstraint::IsFile) && !fs::is_regular_file(p, ec))
        fail("expected a file: " + s);
    if (exists && constraint_has(c, ParamConstraint::IsDir) && !fs::is_directory(p, ec))
        fail("expected a directory: " + s);

    const bool creates = constraint_has(c, ParamConstraint::CreateParents);
    if (constraint_has(c, ParamConstraint::Writable) || constraint_has(c, ParamConstraint::ParentExists) || creates) {
        const fs::path parent = p.has_parent_path() ? p.parent_path() : media::path::path_from_utf8(".");
        bool parent_exists = fs::exists(parent, ec);
        if (!parent_exists && creates) {
            // CreateParents means a missing parent is acceptable � it would be
            // built. Actually create it now (real run); under simulate we only
            // *predict* it, so the precondition is satisfied without touching disk.
            if (!rt.options.simulate) {
                fs::create_directories(parent, ec);
                parent_exists = fs::exists(parent, ec);
            }
        } else if (!parent_exists &&
                   (constraint_has(c, ParamConstraint::Writable) || constraint_has(c, ParamConstraint::ParentExists))) {
            fail("parent directory does not exist: " + media::path::path_to_utf8(parent));
        }
        if (parent_exists && constraint_has(c, ParamConstraint::Writable) && exists &&
            (fs::status(p, ec).permissions() & fs::perms::owner_write) == fs::perms::none)
            fail("path is not writable: " + s);
    }

    if (constraint_has(c, ParamConstraint::Absolute) && !p.is_absolute())
        diag.push_back(Diagnostic{d.name, "path should be absolute: " + s, 1});
}

} // namespace

ResolvedInputs resolve_block_inputs(const nlohmann::json&        block,
                                    const std::vector<ParamDef>& defs,
                                    BlockRuntime&                rt)
{
    ResolvedInputs out;
    out.values.reserve(defs.size());
    for (const ParamDef& d : defs) {
        const ParamBindingResolution binding = resolve_param_binding(block, d, rt);
        const auto literal_it = block.is_object() ? block.find(d.name) : block.end();
        const bool has_explicit_literal = block.is_object() &&
            literal_it != block.end() && !literal_it->is_null() &&
            !(literal_it->is_string() &&
              literal_it->get_ref<const std::string&>().empty() &&
              (d.uses_previous || d.source == ParamSource::Previous ||
               d.source == ParamSource::Variable));
        const nlohmann::json* raw = binding.present
            ? binding.value
            : source_param(block, d, rt);
        if (binding.present && !binding.diagnostic_code.empty()) {
            out.diagnostics.push_back(Diagnostic{
                d.name,
                binding.message,
                2,
                binding.diagnostic_code,
            });
        }
        const bool missing_required = d.required &&
            (!raw || raw->is_null() ||
             (raw->is_string() && raw->get_ref<const std::string&>().empty()));
        if (missing_required) {
            out.diagnostics.push_back(Diagnostic{
                d.name,
                "required parameter is missing",
                2,
                "required_parameter_missing",
            });
        }
        // Compose resolution features on top of the kind's default: string-shaped
        // kinds interpolate ${var} after transform; typed kinds interpolate before.
        ParamResolve features = d.resolve;
        if (is_interpolatable(d.kind))       features |= ParamResolve::Variables;
        if (is_variable_templatable(d.kind)) features |= ParamResolve::Variables;
        if (is_path_kind(d.kind))            features |= ParamResolve::Deep;  // paths are templates

        nlohmann::json pre_interpolated;
        const nlohmann::json* transform_raw = raw;
        if (raw && raw->is_string()) {
            const std::string& tmpl = raw->get_ref<const std::string&>();
            if (tmpl.find("${") != std::string::npos &&
                resolve_has(features, ParamResolve::Variables) &&
                is_variable_templatable(d.kind)) {
                const int passes = resolve_has(features, ParamResolve::Deep)
                    ? bv::kMaxDeepPasses : 1;
                pre_interpolated = bv::resolve(tmpl, rt, /*quiet=*/false, passes);
                transform_raw = &pre_interpolated;
            }
        }

        Value v = transform_param(d, transform_raw, out.diagnostics);

        if (!binding.present && !v.is_null() &&
            (resolve_has(features, ParamResolve::Variables) ||
             resolve_has(features, ParamResolve::Globs))) {
            std::string s = v.as_string();

            // 1) Variables � expand ${var}/${ENV:..}/${KNOWNFOLDER:..} up front so
            //    the frame holds final values and downstream probes see real paths.
            //    `.deep()` params additionally re-expand tokens carried inside
            //    substituted variable values (bounded; circular refs abort).
            if (resolve_has(features, ParamResolve::Variables) && s.find("${") != std::string::npos) {
                const int passes = resolve_has(features, ParamResolve::Deep) ? bv::kMaxDeepPasses : 1;
                s = bv::resolve(s, rt, /*quiet=*/false, passes);
                v = Value{ s };
            }

            // 2) Globs � expand *,?,** into an array of matching paths. A pattern
            //    with no glob tokens stays a scalar (e.g. a plain directory path).
            if (resolve_has(features, ParamResolve::Globs) && media::has_glob_tokens(s)) {
                nlohmann::json arr = expand_glob(s);
                v = Value::from_json(arr);
            }
        }

        // Path-shaped params: collapse duplicate separators and unify `/` vs `\`
        // after ${KNOWNFOLDER:...} / ${VAR} substitution (same as installer paths).
        if (is_path_kind(d.kind) && !v.is_null() && !v.is_array()) {
            const std::string s = v.as_string();
            if (!s.empty()) {
                const std::string norm = media::path::normalize_local_path(s, true);
                if (!norm.empty())
                    v = Value{ norm };
            }
        }

        // Expression-like text controls are a resolver concern. Evaluate only
        // explicit document literals: values supplied by bindings or declared
        // sources are already final typed data and must not be reinterpreted.
        if (!binding.present && has_explicit_literal && !v.is_null() &&
            d.binding_syntax == ParamBindingSyntax::Expression) {
            const std::string expression = v.as_string();
            if (rt.context_value) {
                if (const nlohmann::json* found =
                        rt.context_value(rt.user_data, expression)) {
                    v = Value::from_json(*found);
                } else if (rt.eval_expression) {
                    double evaluated = 0.0;
                    std::string expression_error;
                    if (rt.eval_expression(rt.user_data, expression, evaluated,
                                           &expression_error))
                        v = Value{evaluated};
                }
            }
        }

        // 3) Custom � app-level resolve hooks (policy/guard/redirect). Runs after
        //    variable/glob expansion so hooks see the final value(s).
        if (resolve_has(features, ParamResolve::Custom))
            apply_resolve_hooks(d, block.value("kind", std::string{}), v, rt, out.diagnostics);

        validate_constraints(d, v, rt, out.diagnostics);
        out.values.emplace_back(d.name, std::move(v));
    }
    return out;
}

void register_builtin_blocks(BlockRegistry& registry)
{
    register_data_blocks(registry);
    register_network_blocks(registry);
    register_shell_blocks(registry);
    register_app_blocks(registry);
    register_browser_blocks(registry);
    register_fs_blocks(registry);
#if (defined(FEATURE_COMMAND_RESIZE) && FEATURE_COMMAND_RESIZE) \
 || (defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM) \
 || (defined(FEATURE_VIDEO) && FEATURE_VIDEO)
    register_image_blocks(registry);
#endif
    register_input_blocks(registry);
#if defined(FEATURE_ONNX) && FEATURE_ONNX || defined(FEATURE_LLAMA) && FEATURE_LLAMA || defined(FEATURE_VLM) && FEATURE_VLM
    register_ocr_blocks(registry);
#endif
#if PM_HAS_AUDIO_IO
    register_audio_tts_scripted_blocks(registry);
    register_audio_blocks(registry);
    register_audio_voice_command_blocks(registry);
#endif
#if (defined(FEATURE_VIDEO) && FEATURE_VIDEO) \
 || (defined(FEATURE_VIDEO_DETECTION) && FEATURE_VIDEO_DETECTION)
    register_video_blocks(registry);
#endif
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    register_video_filter_blocks(registry);
    register_video_vcam_blocks(registry);
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA || defined(FEATURE_VLM) && FEATURE_VLM
    register_vision_blocks(registry);
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA || defined(FEATURE_ONNX) && FEATURE_ONNX \
 || defined(FEATURE_WHISPER) && FEATURE_WHISPER || defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
    register_model_blocks(registry);
#endif
    register_bluetooth_blocks(registry);
#if defined(PM_PLUGIN_CHAT) && PM_PLUGIN_CHAT
    pm::plugins::chat_host::register_llm_blocks(registry);
#elif defined(PM_CHAT_SURFACE) && PM_CHAT_SURFACE
    register_llm_blocks(registry);
#endif
#if defined(FEATURE_MODBUS_XBLOX) && FEATURE_MODBUS_XBLOX
    register_modbus_blocks(registry);
#endif
#if defined(FEATURE_MQTT_XBLOX) && FEATURE_MQTT_XBLOX
    register_mqtt_blocks(registry);
#endif
#if defined(FEATURE_SERVICE_XBLOX) && FEATURE_SERVICE_XBLOX
    register_service_blocks(registry);
#endif
#if defined(FEATURE_VECTOR_XBLOX) && FEATURE_VECTOR_XBLOX
//    register_vector_blocks(registry);
#endif
#if defined(FEATURE_VECTOR_LOCAL_XBLOX) && FEATURE_VECTOR_LOCAL_XBLOX
    register_vector_local_blocks(registry);
#endif
    registry["wait"] = bdb(
        "wait", wait_block, "Wait", "Flow",
        "Sleep for a fixed number of milliseconds.",
        {{"kind", "wait"}, {"ms", media::xblox::constants::kWaitDefaultMs}})
        .params({
            pd_ms("ms").in().req().dflt(media::xblox::constants::kWaitDefaultMs)
                .desc("Duration to sleep in milliseconds.")
                .range(0, media::xblox::constants::kWaitMaxMs),
        })
        .preserves_previous();

    registry["log"] = bdb(
        "log", log_block, "Log", "Context",
        "Log a message, expression, variable, or the whole scope.",
        {{"kind", "log"}, {"level", "info"}, {"message", "PREVIOUS"}})
        .params({
            pd_enum("level", {"trace","debug","info","warn","error"})
                .in().dflt("info").desc("Log level."),
            pd_str("message").in().prev().syntax(ParamBindingSyntax::Expression).dflt("PREVIOUS")
                .control("variable_text")
                .desc("Message or variable expression. Leave empty to log the whole scope."),
            pd_str("input").grp("query").dflt("PREVIOUS")
                .desc("Optional context variable to query with jq. When set with filter/query, it replaces message output."),
            pd_str("filter").grp("query").dflt(".")
                .desc("jq filter applied to input, e.g. .items[0].name."),
            pd_enum("format", {"auto","json"}).grp("query").dflt("auto")
                .desc("auto prints scalars naturally and objects/arrays as JSON; json always emits compact JSON."),
        })
        .pure()
        .preserves_previous();

    registry["stdout"] = bdb(
        "stdout", stdout_block, "Stdout", "Context",
        "Write a message directly to stdout (pipe-friendly).",
        {{"kind", "stdout"}, {"message", "PREVIOUS"}, {"newline", true}})
        .params({
            pd_str("message").in().prev().syntax(ParamBindingSyntax::Expression).dflt("PREVIOUS").deep()
                .control("variable_text")
                .desc("Message or variable expression. Leave empty to output the whole scope."),
            pd_str("input").grp("query").dflt("PREVIOUS")
                .desc("Optional context variable to query with jq. When set with filter/query, it replaces message output."),
            pd_str("filter").grp("query").dflt(".")
                .desc("jq filter applied to input, e.g. .items[0].name."),
            pd_enum("format", {"auto","json"}).grp("query").dflt("auto")
                .desc("auto prints scalars naturally and objects/arrays as JSON; json always emits compact JSON."),
            pd_bool("newline").in().dflt(true).desc("Append a newline."),
        })
        .pure()
        .preserves_previous();

    registry["stdin"] = bdb(
        "stdin", stdin_block, "Stdin", "Context",
        "Read piped input from stdin (whole stream or one line) into PREVIOUS/storeAs.",
        {{"kind", "stdin"}, {"mode", "all"}, {"parse", "text"}, {"trim", true}})
        .params({
            pd_enum("mode", {"all", "line"}).in().dflt("all")
                .desc("all = read the entire stream to EOF; line = read the next line (streamable inside loops)."),
            pd_enum("parse", {"text", "json", "number", "boolean", "auto"}).in().dflt("text")
                .desc("Result type for PREVIOUS/storeAs: text (string), json (object/array/scalar), "
                      "number (int/float � usable in expressions), boolean, or auto (JSON-scalar "
                      "detection with text fallback). Binary input (NUL bytes) is rejected."),
            pd_bool("trim").in().dflt(true)
                .desc("Trim trailing whitespace/newlines before parsing."),
            pd_store_as(),
        })
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::string_, ParamKind::integer_, ParamKind::float_,
             ParamKind::boolean_, ParamKind::json_value},
            "Input");

    registry["exit"] = bdb(
        "exit", exit_block, "Exit", "Flow",
        "Stop the run immediately and set the process exit code.",
        {{"kind", "exit"}, {"code", 0}})
        .params({
            pd_int("code").in().dflt(0).range(0, 255)
                .desc("Process exit code (0-255). Standard meanings: 0 = success, 1 = general error, "
                      "2 = misuse/invalid input, 126 = not executable, 127 = not found, 130 = interrupted "
                      "(Ctrl+C). Any other value is allowed for app-specific signalling to pipelines."),
            pd_str("message").in().dflt("")
                .desc("Optional message recorded on the exit event."),
        })
        .pure()
        .preserves_previous();

    registry["break"] = bdb(
        "break", break_block, "Break", "Flow",
        "Break out of the current loop.",
        {{"kind", "break"}})
        .pure()
        .preserves_previous();

    registry["command"] = bdb(
        "command", group_block, "Command", "Commands",
        "Run a saved custom command or an inline CLI/app/external/url/path payload.",
        {{"kind", "command"},
         {"commandId", ""},
         {"command", {{"appCommand", "open"}, {"args", nlohmann::json::array()}}},
         {"log", false},
         {"stdout", "info"},
         {"stderr", "error"},
         {"storeAs", ""}})
        .params({
            pd_str("commandId").in().lbl("Saved command")
                .ui({{"control", "select"},
                     {"options_path", "commands.custom"},
                     {"empty_label", "(inline call)"}})
                .desc("Optional: look up a commands.json entry by id. Leave empty to "
                      "call an inline app/CLI/external action below."),
            pd_json("command").in().lbl("Call")
                .control("command_payload")
                .wide()
                .desc("Inline call: app/CLI/external/url/path plus args. "
                      "Args support ${var} from the xblox context (deep-resolved)."),
            pd_args("args").adv().dflt(nlohmann::json::array())
                .desc("Extra argv tokens appended after the saved/inline command. "
                      "Use two tokens for xblox scope, e.g. --CURRENT_FILE and ${output}."),
            pd_bool("log").adv().dflt(false)
                .desc("Echo captured stdout/stderr to the host logger and xBlox run log, "
                      "prefixed with this block's id."),
            pd_enum("stdout", {"trace","debug","info","warn","error","off"}).adv().dflt("info")
                .desc("Log level for captured stdout lines."),
            pd_enum("stderr", {"trace","debug","info","warn","error","off"}).adv().dflt("error")
                .desc("Log level for captured stderr lines."),
            pd_store_as(),
        })
        .preserves_previous()
        .exec({true, true, true});

    registry["setVariable"] = bdb(
        "setVariable", set_variable_block, "Set Variable", "Context",
        "Write a value into root scope.",
        {{"kind", "setVariable"}, {"name", "value"}, {"value", nullptr}})
        .params({
            pd_str("name").in().req().desc("Variable name to write."),
            pd_json("value").in().desc("Literal value (JSON). String values interpolate ${var}."),
            pd_str("valueFrom").in().desc("Copy this context variable. Wins over value when set."),
            pd_str("fallbackFrom").in().desc("If the chosen value is empty, copy this context variable instead."),
            pd_expr("expression").in().desc("Expression evaluated to the stored value. Overrides value when set."),
        })
        .pure()
        .primary_output(ParamKind::json_value, "Value");

    registry["getVariable"] = bdb(
        "getVariable", get_variable_block, "Get Variable", "Context",
        "Read a value from root scope.",
        {{"kind", "getVariable"}, {"name", ""}, {"target", "PREVIOUS"}})
        .params({
            pd_varref("name").in().req().desc("Variable name to read."),
            pd_str("target").in().dflt("PREVIOUS").desc("Variable to copy the value into. PREVIOUS updates the pipeline value only."),
        })
        .pure()
        .primary_output(ParamKind::json_value, "Value");

    registry["saveState"] = bdb(
        "saveState", save_state_block, "Save State", "Context",
        "Persist current scope values back into this XBlox document's context.",
        {{"kind", "saveState"}, {"scope", "context"}, {"mode", "existing"}, {"keys", nlohmann::json::array()}})
        .params({
            pd_enum("scope", {"context"}).in().dflt("context").desc("State area to persist. Currently only document context."),
            pd_enum("mode", {"existing", "all"}).in().dflt("existing")
                .desc("existing = update keys already present in the file context. all = write the full current scope."),
            pd_args("keys").in().dflt(nlohmann::json::array())
                .desc("Optional context keys to persist. Empty uses mode."),
        })
        .primary_output(ParamKind::json_value, "Persisted state")
        .exec({false, true, true});

    registry["if"] = bdb(
        "if", if_block, "If", "Flow",
        "Run child blocks when the condition is true. Chain `elseIf`/`else` siblings after it.",
        {{"kind", "if"}, {"condition", "true"}, {"consequent", nlohmann::json::array()}})
        .params({
            pd_expr("condition").in().req().dflt("true").desc("Condition expression."),
        })
        .preserves_previous()
        .children({
            {"consequent", "Then", "consequent", {"*"}, 0, -1},
        })
        .exec({false, false, true, true});

    registry["elseIf"] = bdb(
        "elseIf", else_if_block, "Else If", "Flow",
        "Runs when the preceding if/elseIf chain has not matched and its own condition is true.",
        {{"kind", "elseIf"}, {"condition", "true"}, {"items", nlohmann::json::array()}})
        .params({
            pd_expr("condition").in().req().dflt("true").desc("Condition expression."),
        })
        .preserves_previous()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    registry["else"] = bdb(
        "else", else_block, "Else", "Flow",
        "Runs when the preceding if/elseIf chain has not matched.",
        {{"kind", "else"}, {"items", nlohmann::json::array()}})
        .preserves_previous()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    registry["case"] = bdb(
        "case", case_orphan_block, "Case", "Flow",
        "One switch branch: runs when the switch value matches its expression.",
        {{"kind", "case"}, {"comparator", "==="}, {"expression", "\"value\""},
         {"consequent", nlohmann::json::array()}})
        .params({
            pd_str("comparator").in().dflt("===").desc("Comparison operator (===, !=, <, >, ...)."),
            pd_expr("expression").in().desc("Value the switch variable is compared against."),
        })
        .preserves_previous()
        .children({{"consequent", "Body", "consequent", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    registry["switchDefault"] = bdb(
        "switchDefault", case_orphan_block, "Default", "Flow",
        "Fallback switch branch: runs when no case matches.",
        {{"kind", "switchDefault"}, {"consequent", nlohmann::json::array()}})
        .preserves_previous()
        .children({{"consequent", "Body", "consequent", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    registry["for"] = bdb(
        "for", for_block, "For", "Flow",
        "Run child blocks over a numeric range.",
        {{"kind", "for"}, {"initial", "0"}, {"final", "3"}, {"comparator", "<"}, {"modifier", "+1"}, {"items", nlohmann::json::array()}})
        .params({
            pd_expr("initial").in().req().dflt("0").desc("Loop counter initial value."),
            pd_enum("comparator", {"<", "<=", ">", ">=", "!=", "=="}).in().req().dflt("<"),
            pd_expr("final").in().req().dflt("3").desc("Loop bound expression."),
            pd_expr("modifier").in().req().dflt("+1").desc("Counter increment expression, e.g. +1 or *2."),
        })
        .preserves_previous()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});

    registry["while"] = bdb(
        "while", while_block, "While", "Flow",
        "Run child blocks while a condition is true.",
        {{"kind", "while"}, {"condition", "false"}, {"loopLimit", media::xblox::constants::kLoopLimitDefault}, {"items", nlohmann::json::array()}})
        .params({
            pd_expr("condition").in().req().dflt("false").desc("Loop condition expression."),
            pd_int("loopLimit").adv().dflt(media::xblox::constants::kLoopLimitDefault)
                .range(1, media::xblox::constants::kLoopLimitMax)
                .desc("Maximum iterations (safety cap)."),
        })
        .preserves_previous()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});

    registry["switch"] = bdb(
        "switch", switch_block, "Switch", "Flow",
        "Run the first matching case.",
        {{"kind", "switch"}, {"variable", "mode"}, {"items", nlohmann::json::array()}})
        .params({
            pd_varref("variable").in().req().dflt("mode").desc("Variable to switch on."),
        })
        .preserves_previous()
        .children({{"items", "Cases", "case", {"case", "switchDefault"}, 0, -1}})
        .exec({false, false, true, true});

    registry["group"] = bdb(
        "group", group_block, "Group", "Flow",
        "Transparent container: runs its child blocks in the current scope.",
        {{"kind", "group"}, {"items", nlohmann::json::array()}})
        .preserves_previous()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    registry["runScript"] = bdb(
        "runScript", group_block, "Run Script", "Flow",
        "Legacy alias for group: runs child blocks in the current scope.",
        {{"kind", "runScript"}, {"items", nlohmann::json::array()}})
        .preserves_previous()
        .hidden()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    registry["onStart"] = bdb(
        "onStart", on_start_block, "On Start", "Flow",
        "Run child blocks once when the document starts or ends.",
        {{"kind", "onStart"}, {"event", "On Start"}, {"items", nlohmann::json::array()}})
        .params({
            pd_enum("event", {"On Start", "On End"}).in().dflt("On Start")
                .ui({{"block_label", true}})
                .desc("Document lifecycle moment that runs this block's children once."),
        })
        .preserves_previous()
        .children({{"items", "Body", "body", {"*"}, 0, -1}})
        .exec({false, false, true, true});

    validate_registered_block_descriptors(registry);
}

std::vector<BlockDescriptor> registered_block_definitions(bool include_hidden)
{
    BlockRegistry registry;
    register_builtin_blocks(registry);
    std::vector<BlockDescriptor> definitions;
    definitions.reserve(registry.size());
    for (const auto& [_, descriptor] : registry) {
        if (!include_hidden && !descriptor.palette_visible) continue;
        if (!is_block_group_allowed(descriptor.group))      continue;
        if (!network_block_allowed(descriptor.kind))        continue;
        definitions.push_back(descriptor);
    }
    std::sort(definitions.begin(), definitions.end(), [](const BlockDescriptor& a, const BlockDescriptor& b) {
        if (a.group != b.group)
            return a.group < b.group;
        return a.kind < b.kind;
    });
    return definitions;
}

} // namespace media::xblox::blocks
