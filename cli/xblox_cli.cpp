#include "xblox_cli.hpp"

#include "Scope.h"
#include "blocks/block_params_ui.hpp"
#include "blocks/builtin_blocks.hpp"
#include "blocks/parameter_operations.hpp"
#include "fs/path_sanitizer.hpp"
#include "logging/logger.h"
#include "utils/conv.hpp"
#include "xblox_commands.hpp"
#include "xblox_diagram.hpp"
#include "xblox_markdown.hpp"
#include "xblox_perf.hpp"
#include "xblox_sequence_diagram.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace media::xblox::cli {
namespace {

Hooks g_hooks;
ParentRefs g_parent;

std::atomic<bool> g_local_cancel{false};

void local_signal_handler(int) {
    g_local_cancel.store(true);
}

struct State {
    CLI::App* info_cmd = nullptr;
    CLI::App* info_schema_cmd = nullptr;
    CLI::App* info_options_cmd = nullptr;
    CLI::App* selftest_cmd = nullptr;
    CLI::App* run_cmd = nullptr;
    CLI::App* session_cmd = nullptr;
    CLI::App* session_list_cmd = nullptr;
    CLI::App* session_start_cmd = nullptr;
    CLI::App* session_stop_cmd = nullptr;
    CLI::App* session_ping_cmd = nullptr;
    CLI::App* session_pong_cmd = nullptr;
    CLI::App* session_info_cmd = nullptr;
    CLI::App* session_send_cmd = nullptr;
    CLI::App* session_broadcast_cmd = nullptr;
    CLI::App* session_recv_cmd = nullptr;
    CLI::App* session_get_cmd = nullptr;
    CLI::App* session_set_cmd = nullptr;
    CLI::App* session_vars_cmd = nullptr;

    std::string src;
    std::string commands_path;
    std::vector<std::string> extra_args;
    bool json = false;
    std::string event_mode = "errors";
    CLI::Option* event_mode_opt = nullptr;
    bool info_json = false;
    std::string info_schema_path;
    std::string info_schema_provider;
    std::string info_schema_model;
    std::string info_options_path;
    bool dry_run = false;
    bool simulate = false;
    bool md = false;
    bool md_numbered = false;
    std::vector<std::string> md_filter;
    bool mermaid = false;
    std::string mermaid_type = "flow";
    std::string mermaid_direction = "TD";
    std::string mermaid_color = "both";
    bool diagram = false;
    std::string diagram_styles;
    std::string diagram_theme = "light";
    std::string diagram_type = "flow";
    std::string diagram_png;
    std::string diagram_dst;
    bool diagram_show_arrows = true;
    bool diagram_show_links = true;
    bool diagram_show_disabled = false;
    bool diagram_show_variables = true;
    bool diagram_show_groups = true;
    bool diagram_humanize_node_names = false;
    bool diagram_show_variable_delimiters = false;
    std::string diagram_variable_link_style = "unicode";
    std::string diagram_variable_input_style = "at";
    std::string diagram_link_style = "straight";
    std::string diagram_show_parameters = "none";
    bool diagram_show_parameters_grouped = false;
    bool diagram_show_parameter_values = false;
    CLI::Option* diagram_show_arrows_opt = nullptr;
    CLI::Option* diagram_show_links_opt = nullptr;
    CLI::Option* diagram_show_disabled_opt = nullptr;
    CLI::Option* diagram_show_variables_opt = nullptr;
    CLI::Option* diagram_show_groups_opt = nullptr;
    CLI::Option* diagram_humanize_node_names_opt = nullptr;
    CLI::Option* diagram_show_variable_delimiters_opt = nullptr;
    CLI::Option* diagram_variable_link_style_opt = nullptr;
    CLI::Option* diagram_variable_input_style_opt = nullptr;
    CLI::Option* diagram_link_style_opt = nullptr;
    CLI::Option* diagram_show_parameters_opt = nullptr;
    CLI::Option* diagram_show_parameters_grouped_opt = nullptr;
    CLI::Option* diagram_show_parameter_values_opt = nullptr;
    bool expand_parameters = false;
    bool no_wait = false;
    int max_loop_iterations = 10000;
    bool loop = false;
    bool no_loop = false;
    bool loop_reset = false;
    long long loop_interval_ms = 100;
    CLI::Option* loop_interval_opt = nullptr;
    int64_t loop_limit = 0;
    CLI::Option* loop_limit_opt = nullptr;
    bool quiet = false;
    bool profile = false;
    std::string consent_ui;
    std::string consent_owner;

    std::string session_key;
    std::string session_label;
    std::string session_name;
    std::string session_value;
    std::string session_message;
    std::string session_from;
    bool session_json = false;
    bool session_all = false;
    bool session_peek = false;
};

State g;

nlohmann::json coerce_cli_arg_value(const std::string& s) {
    if (s == "true") return true;
    if (s == "false") return false;
    try {
        size_t pos = 0;
        const long long v = std::stoll(s, &pos);
        if (pos == s.size()) return v;
    } catch (...) {}
    try {
        size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (pos == s.size()) return v;
    } catch (...) {}
    return s;
}

nlohmann::json parse_cli_arg_overrides(const std::vector<std::string>& extras) {
    nlohmann::json args = nlohmann::json::object();
    for (size_t i = 0; i < extras.size(); ++i) {
        const std::string& tok = extras[i];
        if (tok.rfind("--", 0) != 0)
            continue;
        std::string key = tok.substr(2);
        nlohmann::json value;
        if (const auto eq = key.find('='); eq != std::string::npos) {
            value = coerce_cli_arg_value(key.substr(eq + 1));
            key = key.substr(0, eq);
        } else if (i + 1 < extras.size() && extras[i + 1].rfind("--", 0) != 0) {
            value = coerce_cli_arg_value(extras[i + 1]);
            ++i;
        } else {
            value = true;
        }
        if (!key.empty())
            args[key] = std::move(value);
    }
    return args;
}

bool cancel_requested() {
    if (g_hooks.cancel_requested && g_hooks.cancel_requested())
        return true;
    return g_local_cancel.load();
}

void install_interrupts() {
    if (g_hooks.install_interrupts) {
        g_hooks.install_interrupts();
        return;
    }
    std::signal(SIGINT, local_signal_handler);
#ifdef SIGTERM
    std::signal(SIGTERM, local_signal_handler);
#endif
}

std::string expand_path(const std::string& in) {
    if (g_hooks.expand_path)
        return g_hooks.expand_path(in);
    return media::path::normalize_local_path(in, true);
}

bool read_json_file(const std::filesystem::path& path, nlohmann::json& out, std::string& err) {
    try {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            err = "cannot open " + media::path::path_to_utf8(path);
            return false;
        }
        std::ostringstream ss;
        ss << ifs.rdbuf();
        out = nlohmann::json::parse(ss.str());
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool load_commands(nlohmann::json& out, std::string& err) {
    if (g_hooks.load_commands)
        return g_hooks.load_commands(g.commands_path, out, err);
    if (g.commands_path.empty()) {
        out = nlohmann::json::object();
        err.clear();
        return true;
    }
    return read_json_file(media::path::path_from_utf8(g.commands_path), out, err);
}

int noop_handler(const CommandInvocation& invocation, std::string* message) {
    if (message)
        *message = invocation.action + " command noop";
    return 0;
}

void print_plain_errors(const ExecutionResult& result) {
    for (const auto& event : result.events) {
        if (event.status != "error")
            continue;
        std::ostringstream line;
        line << "xblox run: error";
        if (!event.path.empty())
            line << " at " << event.path;
        if (!event.kind.empty())
            line << " [" << event.kind << "]";
        if (!event.message.empty())
            line << ": " << event.message;
        if (event.exit_code != 0)
            line << " (exit " << event.exit_code << ")";
        const std::string text = line.str();
        std::cerr << text << "\n";
        logger::error(text);
        if (!event.data.is_null() && !event.data.empty())
            std::cerr << event.data.dump(2) << "\n";
    }
}

void add_env_deny_hook(ExecutionOptions& options) {
    if (const char* needle = std::getenv("XBLOX_DENY_PATH_SUBSTR"); needle && *needle) {
        options.resolve_hooks.push_back([sub = std::string(needle)](const ResolveHookInput& in) {
            ResolveHookOutput out;
            if (in.value.find(sub) != std::string::npos) {
                out.deny = true;
                out.severity = 2;
                out.message = "denied by XBLOX_DENY_PATH_SUBSTR (" + sub + ")";
            }
            return out;
        });
    }
}

nlohmann::json info_payload() {
    nlohmann::json blocks = nlohmann::json::array();
    nlohmann::json groups = nlohmann::json::object();
    for (const auto& block : blocks::registered_block_definitions(true)) {
        const auto block_json = block.to_manifest();
        blocks.push_back(block_json);
        groups[block.group.empty() ? "Other" : block.group].push_back(block_json);
    }

    nlohmann::json registered = nlohmann::json::array();
    nlohmann::json custom = nlohmann::json::array();
    if (g_hooks.list_commands)
        g_hooks.list_commands(registered, custom);

    nlohmann::json document = nlohmann::json::object();
    std::string err;
    if (load_commands(document, err))
        err.clear();

    std::string commands_path = g_hooks.commands_path
        ? g_hooks.commands_path()
        : g.commands_path;
    return {
        {"version", 2},
        {"description", "XBlox block-tree command flow metadata for LLM composition."},
        {"documentShape", {
            {"version", 2},
            {"context", nlohmann::json::object()},
            {"roots", nlohmann::json::array()},
        }},
        {"blocks", std::move(blocks)},
        {"groups", std::move(groups)},
        {"bindingDefaults", {
            {"accepts", {"blockOutput", "scopeVariable", "previous", "parameterOperation"}},
            {"syntax", "value"},
        }},
        {"parameterOperations", blocks::parameter_operation_registry().to_manifest()},
        {"commands", {
            {"commandsPath", std::move(commands_path)},
            {"document", std::move(document)},
            {"documentError", err},
            {"registeredCommands", std::move(registered)},
            {"customCommands", std::move(custom)},
        }},
    };
}

int cmd_info() {
    if (g.info_schema_cmd && g.info_schema_cmd->parsed()) {
        const std::string path = g.info_schema_path.empty()
            ? (g.info_schema_provider.empty()
                ? std::string{}
                : "providers." + g.info_schema_provider)
            : g.info_schema_path;
        const nlohmann::json req = {
            {"schema_path", path},
            {"provider", g.info_schema_provider},
            {"model", g.info_schema_model},
        };
        const auto result = blocks::ui::resolve_provider_options_schema(req);
        std::cout << result.dump(2) << "\n";
        const bool has_schema = result.contains("schema")
            && result["schema"].is_object()
            && result["schema"].contains("properties")
            && !result["schema"]["properties"].empty();
        return has_schema ? 0 : 1;
    }

    if (g.info_options_cmd && g.info_options_cmd->parsed()) {
        const auto options = blocks::ui::resolve_options_path(g.info_options_path);
        std::cout << options.dump(2) << "\n";
        return (options.is_array() && !options.empty()) ? 0 : 1;
    }

    const auto payload = info_payload();
    if (g.info_json) {
        std::cout << payload.dump(2) << "\n";
        return 0;
    }

    const auto& blocks_json = payload["blocks"];
    const auto& commands = payload["commands"];
    std::cout << "XBlox blocks: " << (blocks_json.is_array() ? blocks_json.size() : 0) << "\n";
    for (const auto& [group, items] : payload["groups"].items())
        std::cout << "  " << group << ": " << (items.is_array() ? items.size() : 0) << "\n";
    std::cout << "Registered CLI commands: "
              << commands.value("registeredCommands", nlohmann::json::array()).size() << "\n";
    std::cout << "Custom commands: "
              << commands.value("customCommands", nlohmann::json::array()).size() << "\n";
    std::cout << "Use `xblox info --json` for block params/defaults and command payloads.\n";
    std::cout << "Use `xblox info schema --provider <name>` to resolve a provider options schema.\n";
    return 0;
}

int cmd_session() {
    namespace session = media::xblox::session;
    using nlohmann::json;

    if (g.session_list_cmd && g.session_list_cmd->parsed()) {
        const auto sessions = session::list_sessions();
        if (g.session_json) {
            json arr = json::array();
            for (const auto& d : sessions)
                arr.push_back(d.to_json());
            std::cout << arr.dump(2) << "\n";
            return 0;
        }
        std::cout << sessions.size() << " session(s)\n";
        for (const auto& d : sessions) {
            std::cout << "  " << d.key << "  " << d.transport << ":" << d.endpoint
                      << "  pid=" << d.pid << (d.loop ? "  [loop]" : "")
                      << (d.source.empty() ? "" : ("  " + d.source)) << "\n";
        }
        return 0;
    }

    if (g.session_start_cmd && g.session_start_cmd->parsed()) {
        install_interrupts();
        session::SessionInfo info;
        if (!g.session_key.empty()) {
            info.key = g.session_key.rfind("name:", 0) == 0
                ? g.session_key
                : "name:" + g.session_key;
        } else {
            info.key = "name:host-" + std::to_string(static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()));
        }
        info.label = g.session_label;
        try {
            info.cwd = media::path::path_to_utf8(std::filesystem::current_path());
        } catch (...) {
            info.cwd = ".";
        }
        session::SessionServer server;
        std::string err;
        if (!server.start(info, err)) {
            std::cerr << "xblox session start: " << err << "\n";
            return 1;
        }
        ExecutionOptions attach_options;
        RunAttach attach{
            attach_options,
            &server,
            true,
            false,
            info.cwd,
            info.label,
            {},
            g_parent.process_command_line ? *g_parent.process_command_line : std::string{},
            nullptr,
            {},
            {},
            {},
        };
        std::function<void()> cleanup;
        if (g_hooks.attach)
            cleanup = g_hooks.attach(attach);
        const auto& d = server.descriptor();
        if (g.session_json)
            std::cout << d.to_json().dump() << "\n";
        else
            std::cout << "session " << d.key << " hosting on " << d.transport << ":"
                      << d.endpoint << " (pid " << d.pid << ")\n";
        std::cout.flush();
        while (!server.stop_requested() && !cancel_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (cleanup)
            cleanup();
        server.shutdown();
        return 0;
    }

    auto resolve_target = [&](session::SessionDescriptor& d) -> bool {
        if (g.session_key.empty()) {
            std::cerr << "xblox session: --key <key> required\n";
            return false;
        }
        if (!session::find_session(g.session_key, d)) {
            std::cerr << "xblox session: no session matching '" << g.session_key << "'\n";
            return false;
        }
        return true;
    };
    auto parse_value = [](const std::string& s) -> json {
        try {
            return json::parse(s);
        } catch (...) {
            return json(s);
        }
    };

    if (g.session_info_cmd && g.session_info_cmd->parsed()) {
        session::SessionDescriptor d;
        if (!resolve_target(d))
            return 1;
        if (g.session_json) {
            std::cout << d.to_json().dump(2) << "\n";
            return 0;
        }
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const double uptime_s = d.started_unix_ms > 0 ? (now - d.started_unix_ms) / 1000.0 : 0.0;
        std::cout << "key:       " << d.key << "\n"
                  << "label:     " << d.label << "\n"
                  << "source:    " << (d.source.empty() ? "(inline)" : d.source) << "\n"
                  << "cwd:       " << (d.cwd.empty() ? "(unknown)" : d.cwd) << "\n"
                  << "pid:       " << d.pid << "\n"
                  << "transport: " << d.transport << ":" << d.endpoint << "\n"
                  << "loop:      " << (d.loop ? "yes" : "no") << "\n"
                  << "uptime:    " << uptime_s << "s\n";
        json reply;
        std::string err;
        if (session::send_action(d, "var.list", json::object(), reply, err) && reply.contains("vars")) {
            const auto& vars = reply["vars"];
            std::cout << "vars:      " << (vars.empty() ? std::string("(none)") : vars.dump()) << "\n";
        }
        return 0;
    }

    if (g.session_get_cmd && g.session_get_cmd->parsed()) {
        if (g.session_name.empty()) {
            std::cerr << "xblox session get: --name required\n";
            return 1;
        }
        session::SessionDescriptor d;
        if (!resolve_target(d))
            return 1;
        json reply, extra;
        extra["name"] = g.session_name;
        std::string err;
        if (!session::send_action(d, "var.get", extra, reply, err)) {
            std::cerr << "get " << d.key << ": " << err << "\n";
            return 1;
        }
        if (g.session_json)
            std::cout << reply.dump() << "\n";
        else if (reply.value("found", false))
            std::cout << reply["value"].dump() << "\n";
        else
            std::cout << "(unset)\n";
        return 0;
    }
    if (g.session_set_cmd && g.session_set_cmd->parsed()) {
        if (g.session_name.empty()) {
            std::cerr << "xblox session set: --name required\n";
            return 1;
        }
        session::SessionDescriptor d;
        if (!resolve_target(d))
            return 1;
        json reply, extra;
        extra["name"] = g.session_name;
        extra["value"] = parse_value(g.session_value);
        std::string err;
        if (!session::send_action(d, "var.set", extra, reply, err)) {
            std::cerr << "set " << d.key << ": " << err << "\n";
            return 1;
        }
        std::cout << (g.session_json ? reply.dump()
                                     : g.session_name + " = " + extra["value"].dump())
                  << "\n";
        return 0;
    }
    if (g.session_vars_cmd && g.session_vars_cmd->parsed()) {
        session::SessionDescriptor d;
        if (!resolve_target(d))
            return 1;
        json reply;
        std::string err;
        if (!session::send_action(d, "var.list", json::object(), reply, err)) {
            std::cerr << "vars " << d.key << ": " << err << "\n";
            return 1;
        }
        std::cout << reply.value("vars", json::object()).dump(g.session_json ? -1 : 2) << "\n";
        return 0;
    }
    if (g.session_recv_cmd && g.session_recv_cmd->parsed()) {
        session::SessionDescriptor d;
        if (!resolve_target(d))
            return 1;
        json reply, extra;
        extra["peek"] = g.session_peek;
        std::string err;
        if (!session::send_action(d, "recv", extra, reply, err)) {
            std::cerr << "recv " << d.key << ": " << err << "\n";
            return 1;
        }
        const auto& messages = reply.value("messages", json::array());
        if (g.session_json) {
            std::cout << messages.dump(2) << "\n";
        } else {
            std::cout << messages.size() << " message(s)\n";
            for (const auto& m : messages)
                std::cout << "  [" << m.value("from", std::string{"?"}) << "] "
                          << m.value("message", std::string{})
                          << (m.contains("data") ? ("  " + m["data"].dump()) : std::string{}) << "\n";
        }
        return 0;
    }
    if (g.session_send_cmd && g.session_send_cmd->parsed()) {
        session::SessionDescriptor d;
        if (!resolve_target(d))
            return 1;
        json reply, extra;
        extra["from"] = g.session_from.empty() ? std::string("cli") : g.session_from;
        extra["message"] = g.session_message;
        std::string err;
        if (!session::send_action(d, "send", extra, reply, err)) {
            std::cerr << "send " << d.key << ": " << err << "\n";
            return 1;
        }
        std::cout << (g.session_json ? reply.dump()
                                     : (d.key + " <- message (inbox " +
                                        std::to_string(reply.value("inbox", 0)) + ")"))
                  << "\n";
        return 0;
    }
    if (g.session_broadcast_cmd && g.session_broadcast_cmd->parsed()) {
        const auto sessions = session::list_sessions();
        json extra;
        extra["from"] = g.session_from.empty() ? std::string("cli") : g.session_from;
        extra["message"] = g.session_message;
        int delivered = 0;
        for (const auto& d : sessions) {
            json reply;
            std::string err;
            if (session::send_action(d, "send", extra, reply, err))
                ++delivered;
            else
                std::cerr << "broadcast " << d.key << ": " << err << "\n";
        }
        std::cout << "broadcast to " << delivered << "/" << sessions.size() << " session(s)\n";
        return delivered == static_cast<int>(sessions.size()) ? 0 : 1;
    }

    std::string action;
    if (g.session_stop_cmd && g.session_stop_cmd->parsed())
        action = "stop";
    else if (g.session_ping_cmd && g.session_ping_cmd->parsed())
        action = "ping";
    else if (g.session_pong_cmd && g.session_pong_cmd->parsed())
        action = "pong";
    else
        return 0;

    std::vector<session::SessionDescriptor> targets;
    if (g.session_all) {
        targets = session::list_sessions();
    } else if (g.session_key.empty()) {
        std::cerr << "xblox session " << action << ": --key <key> or --all required\n";
        return 1;
    } else {
        session::SessionDescriptor d;
        if (!session::find_session(g.session_key, d)) {
            std::cerr << "xblox session " << action << ": no session matching '"
                      << g.session_key << "'\n";
            return 1;
        }
        targets.push_back(d);
    }
    if (targets.empty()) {
        std::cout << "no sessions\n";
        return 0;
    }
    int rc = 0;
    for (const auto& d : targets) {
        json reply;
        std::string err;
        if (!session::send_action(d, action, json::object(), reply, err)) {
            std::cerr << action << " " << d.key << ": " << err << "\n";
            rc = 1;
            continue;
        }
        if (g.session_json)
            std::cout << reply.dump() << "\n";
        else
            std::cout << d.key << " -> " << reply.value("type", std::string{"?"}) << "\n";
    }
    return rc;
}

bool option_set(CLI::Option* opt) {
    return opt && opt->count() > 0;
}

bool populate_diagram_markdown_options(const std::filesystem::path& src,
                                       MarkdownOptions& md,
                                       std::string& err) {
    namespace fs = std::filesystem;
    md.title = media::path::path_to_utf8(src.filename());
    md.mermaid_direction = g.mermaid_direction;
    md.diagram_type = g.diagram_type;
    md.expand_parameters = g.expand_parameters;
    md.diagram_styles_path = g.diagram_styles;

    const std::string cwd = media::path::path_to_utf8(fs::current_path());
    if (!load_diagram_styles(g.diagram_styles, cwd, md.diagram_styles, err, g.diagram_theme))
        return false;

    md.diagram_show_arrows = md.diagram_styles.render.show_arrows;
    md.diagram_show_links = md.diagram_styles.render.show_links;
    md.diagram_show_disabled = md.diagram_styles.render.show_disabled;
    md.diagram_show_variables = md.diagram_styles.render.show_variables;
    md.diagram_show_groups = md.diagram_styles.render.show_groups;
    md.diagram_humanize_node_names = md.diagram_styles.render.humanize_node_names;
    md.diagram_show_variable_delimiters = md.diagram_styles.render.show_variable_delimiters;
    md.diagram_variable_link_style = md.diagram_styles.render.variable_link_style;
    md.diagram_variable_input_style = md.diagram_styles.render.variable_input_style;
    md.diagram_link_style = md.diagram_styles.render.link_style;
    md.diagram_show_parameters = md.diagram_styles.render.show_parameters;
    md.diagram_show_parameters_grouped = md.diagram_styles.render.show_parameters_grouped;
    md.diagram_show_parameter_values = md.diagram_styles.render.show_parameter_values;
    if (g.expand_parameters)
        md.diagram_show_parameters = "all";
    if (option_set(g.diagram_show_arrows_opt))
        md.diagram_show_arrows = g.diagram_show_arrows;
    if (option_set(g.diagram_show_links_opt))
        md.diagram_show_links = g.diagram_show_links;
    if (option_set(g.diagram_show_disabled_opt))
        md.diagram_show_disabled = g.diagram_show_disabled;
    if (option_set(g.diagram_show_variables_opt))
        md.diagram_show_variables = g.diagram_show_variables;
    if (option_set(g.diagram_show_groups_opt))
        md.diagram_show_groups = g.diagram_show_groups;
    if (option_set(g.diagram_humanize_node_names_opt))
        md.diagram_humanize_node_names = g.diagram_humanize_node_names;
    if (option_set(g.diagram_show_variable_delimiters_opt))
        md.diagram_show_variable_delimiters = g.diagram_show_variable_delimiters;
    if (option_set(g.diagram_variable_link_style_opt))
        md.diagram_variable_link_style = g.diagram_variable_link_style;
    if (option_set(g.diagram_variable_input_style_opt))
        md.diagram_variable_input_style = g.diagram_variable_input_style;
    if (option_set(g.diagram_link_style_opt))
        md.diagram_link_style = g.diagram_link_style;
    if (option_set(g.diagram_show_parameters_opt))
        md.diagram_show_parameters = g.diagram_show_parameters;
    if (option_set(g.diagram_show_parameters_grouped_opt))
        md.diagram_show_parameters_grouped = g.diagram_show_parameters_grouped;
    if (option_set(g.diagram_show_parameter_values_opt))
        md.diagram_show_parameter_values = g.diagram_show_parameter_values;

    md.expand_parameters = md.diagram_show_parameters == "all";
    if (!g.diagram_png.empty())
        md.diagram_png = expand_path(g.diagram_png);
    if (!g.diagram_dst.empty())
        md.diagram_dst = expand_path(g.diagram_dst);
    return true;
}

void apply_md_filter(MarkdownOptions& md) {
    if (g.md_filter.empty())
        return;
    md.exclude.clear();
    for (const std::string& kind : g.md_filter)
        if (kind != "none" && !kind.empty())
            md.exclude.push_back(kind);
}

int export_diagram_only(const std::filesystem::path& src, const nlohmann::json& blocks_file) {
    MarkdownOptions md;
    std::string err;
    if (!populate_diagram_markdown_options(src, md, err)) {
        std::cerr << "xblox run: " << err << "\n";
        return 1;
    }
    apply_md_filter(md);
    const nlohmann::json roots = blocks_file.value("roots", nlohmann::json::array());
    const std::string svg = md.diagram_type == "sequence"
        ? render_sequence_diagram_svg(blocks_file, md, md.diagram_styles, md.title, true)
        : render_flow_diagram_svg(roots, md, md.diagram_styles, md.mermaid_direction, md.title, true);

    if (!md.diagram_dst.empty()) {
        if (!save_diagram_svg_to_file(svg, md.diagram_dst, err)) {
            std::cerr << "xblox run: " << err << "\n";
            return 1;
        }
        std::cout << md.diagram_dst << "\n";
    }
    if (!md.diagram_png.empty()) {
        if (!save_diagram_svg_as_png(svg, md.diagram_png, 144.0, err)) {
            std::cerr << "xblox run: " << err << "\n";
            return 1;
        }
        if (md.diagram_dst.empty())
            std::cout << md.diagram_png << "\n";
    }
    if (md.diagram_dst.empty() && md.diagram_png.empty())
        std::cout << svg;
    return 0;
}

void copy_context_field(const nlohmann::json& ctxj, const char* key, std::string& slot) {
    if (!slot.empty() || !ctxj.contains(key))
        return;
    const auto& v = ctxj[key];
    if (v.is_string())
        slot = v.get<std::string>();
    else if (v.is_number_unsigned())
        slot = std::to_string(v.get<unsigned long long>());
    else if (v.is_number_integer())
        slot = std::to_string(v.get<long long>());
}

int cmd_run() {
    namespace fs = std::filesystem;
    install_interrupts();

    std::error_code ec;
    const fs::path src = fs::absolute(media::path::path_from_utf8(g.src), ec);
    if (ec || src.empty()) {
        std::cerr << "xblox run: invalid --src: " << g.src << "\n";
        return 1;
    }

    nlohmann::json blocks_file;
    std::string err;
    if (!read_json_file(src, blocks_file, err)) {
        std::cerr << "xblox run: " << err << "\n";
        return 1;
    }

    if (g.diagram && !g.md)
        return export_diagram_only(src, blocks_file);

    if (g.md) {
        MarkdownOptions md;
        md.title = media::path::path_to_utf8(src.filename());
        md.numbered = g.md_numbered;
        md.mermaid = g.mermaid && !g.diagram;
        md.diagram = g.diagram;
        md.mermaid_type = g.mermaid_type;
        md.diagram_type = g.diagram_type;
        md.mermaid_direction = g.mermaid_direction;
        md.mermaid_color = g.mermaid_color;
        md.expand_parameters = g.expand_parameters;
        md.diagram_styles_path = g.diagram_styles;
        if (g.diagram) {
            if (!populate_diagram_markdown_options(src, md, err)) {
                std::cerr << "xblox run: " << err << "\n";
                return 1;
            }
        } else {
            md.diagram_show_arrows = true;
            md.diagram_show_links = true;
            md.diagram_show_disabled = false;
            md.diagram_show_variables = true;
            md.diagram_show_groups = true;
            md.diagram_humanize_node_names = false;
            md.diagram_show_variable_delimiters = false;
            md.diagram_variable_link_style = "unicode";
            md.diagram_variable_input_style = "at";
            md.diagram_link_style = "straight";
        }
        apply_md_filter(md);
        std::cout << document_to_markdown(blocks_file, md);
        return 0;
    }

    if (g.run_cmd) {
        const nlohmann::json cli_args = parse_cli_arg_overrides(g.run_cmd->remaining());
        if (!blocks_file.is_object())
            blocks_file = nlohmann::json::object();
        nlohmann::json& ctx = blocks_file["context"];
        if (!ctx.is_object())
            ctx = nlohmann::json::object();
        for (auto it = cli_args.begin(); it != cli_args.end(); ++it)
            ctx[it.key()] = it.value();
        ctx["args"] = cli_args;
    }

    nlohmann::json custom_commands;
    if (!load_commands(custom_commands, err)) {
        std::cerr << "xblox run: " << err << "\n";
        return 1;
    }

    std::error_code cwd_ec;
    const std::string parent_cwd = g_parent.cwd ? *g_parent.cwd : std::string{};
    const fs::path default_cwd = parent_cwd.empty()
        ? fs::current_path(cwd_ec)
        : media::path::path_from_utf8(parent_cwd);

    ExecutionOptions options;
    options.custom_commands = std::move(custom_commands);
    options.source_path = src;
    if (!cwd_ec || !parent_cwd.empty()) {
        options.default_cwd = default_cwd;
        options.command_context.cwd = media::path::path_to_utf8(default_cwd);
    }
    if (blocks_file.contains("context") && blocks_file["context"].is_object()) {
        const auto& ctxj = blocks_file["context"];
        copy_context_field(ctxj, "CURRENT_HWND", options.command_context.current_hwnd);
        copy_context_field(ctxj, "CURRENT_PID", options.command_context.current_pid);
        copy_context_field(ctxj, "CURRENT_WINTITLE", options.command_context.current_wintitle);
        copy_context_field(ctxj, "CURRENT_PROCESS", options.command_context.current_process);
    }
    options.execute_external_commands = !g.dry_run;
    options.simulate = g.simulate;
    options.wait_blocks = !g.no_wait;
    const bool event_mode_explicit = option_set(g.event_mode_opt);
    const std::string event_mode = g.json && !event_mode_explicit ? "full" : g.event_mode;
    options.collect_events = event_mode == "full";
    options.collect_error_events = event_mode == "errors";
    options.max_loop_iterations = g.max_loop_iterations;
    if (g_parent.process_args)
        options.process_args = *g_parent.process_args;
    if (g_parent.process_command_line)
        options.process_command_line = *g_parent.process_command_line;
    options.extra_args = g.extra_args;
    if (g.loop)
        options.loop_override = true;
    else if (g.no_loop)
        options.loop_override = false;
    if (option_set(g.loop_interval_opt))
        options.loop_interval_ms_override = g.loop_interval_ms;
    if (g.loop_reset)
        options.loop_reset_override = true;
    if (option_set(g.loop_limit_opt) && g.loop_limit > 0)
        options.loop_limit_override = g.loop_limit;
    if (g.quiet)
        options.quiet = true;
    options.ribbon_command_handler = noop_handler;
    options.open_url_handler = noop_handler;
    if (g_hooks.prepare_options)
        g_hooks.prepare_options(options);
    add_env_deny_hook(options);

    bool will_loop = false;
    if (g.loop)
        will_loop = true;
    else if (g.no_loop)
        will_loop = false;
    else if (blocks_file.contains("loop") && blocks_file["loop"].is_object())
        will_loop = blocks_file["loop"].value("enabled", false);

    namespace session = media::xblox::session;
    session::SessionServer session_server;
    artifacts::ArtifactLedger artifact_ledger;
    const std::string run_label = media::path::path_to_utf8(src.filename());
    const std::string run_cwd = media::path::path_to_utf8(options.default_cwd.empty()
        ? src.parent_path()
        : options.default_cwd);
    artifact_ledger.set_run_context({media::path::path_to_utf8(src), run_cwd, run_label});
    artifact_ledger.set_ui_notify(true);

    bool session_hosted = false;
    const auto artifacts_snapshot = [&]() -> nlohmann::json { return artifact_ledger.snapshot(); };

    if (will_loop) {
        session::SessionInfo info;
        info.key = session::make_file_key(src);
        info.source = media::path::path_to_utf8(src);
        info.label = run_label;
        info.cwd = run_cwd;
        info.loop = true;
        std::string serr;
        if (session_server.start(info, serr)) {
            session_hosted = true;
            artifact_ledger.set_session_sink(&session_server);
        } else {
            std::cerr << "xblox run: session host unavailable: " << serr << "\n";
        }
    }

    RunAttach attach{
        options,
        session_hosted ? &session_server : nullptr,
        session_hosted,
        will_loop,
        run_cwd,
        run_label,
        media::path::path_to_utf8(src),
        options.process_command_line,
        &artifact_ledger,
        artifacts_snapshot,
        g.consent_ui,
        g.consent_owner,
    };
    std::function<void()> cleanup;
    if (g_hooks.attach)
        cleanup = g_hooks.attach(attach);

    options.cancel_requested = [&session_server, session_hosted] {
        return cancel_requested() || (session_hosted && session_server.stop_requested());
    };
    if (session_hosted) {
        options.pull_external_vars = [&session_server] { return session_server.take_pending_sets(); };
        options.publish_vars = [&session_server](const nlohmann::json& vars) {
            session_server.publish_vars(vars);
        };
    }
    options.event_sink = [&artifact_ledger](const ExecutionEvent& event) {
        artifact_ledger.ingest(event);
    };

    auto result = run_blocks_file(blocks_file, options);
    if (cleanup)
        cleanup();
    session_server.shutdown();

    if (g.profile) {
        const auto& p = g_xblox_perf;
        auto us = [](int64_t ns) { return ns / 1000; };
        std::cerr << "\n── xblox profile ──────────────────────────────────────────\n";
        std::cerr << "  jq calls      : " << p.jq_calls
                  << "  (cache hits " << p.jq_cache_hits << ")\n";
        std::cerr << "  to_jv         : " << us(p.to_jv_ns) << " µs\n";
        std::cerr << "  jq VM         : " << us(p.jq_vm_ns) << " µs\n";
        std::cerr << "  from_jv       : " << us(p.from_jv_ns) << " µs\n";
        std::cerr << "  jq total      : " << us(p.jq_total_ns()) << " µs\n";
        std::cerr << "  emit calls    : " << p.emit_calls
                  << "  time " << us(p.emit_ns) << " µs\n";
        std::cerr << "  set_ctx calls : " << p.set_ctx_calls
                  << "  time " << us(p.set_ctx_ns) << " µs\n";
        std::cerr << "  expr calls    : " << p.expr_calls
                  << "  (bytecode reuse " << p.expr_prep_hits << ")\n";
        std::cerr << "  resolve_vars  : " << us(p.resolve_ns) << " µs\n";
        std::cerr << "  prepare_expr  : " << us(p.prepare_ns) << " µs\n";
        std::cerr << "  mu bind+Eval  : " << us(p.mu_eval_ns) << " µs\n";
        std::cerr << "────────────────────────────────────────────────────────────\n";
    }

    nlohmann::json events = nlohmann::json::array();
    for (const auto& event : result.events)
        events.push_back(conv::event_to_json(event));

    const nlohmann::json files = artifacts_snapshot();
    nlohmann::json report = {
        {"ok", result.ok},
        {"exitCode", result.exit_code},
        {"src", media::path::path_to_utf8(src)},
        {"eventCount", result.event_count},
        {"events", std::move(events)},
        {"artifacts", {
            {"enabled", true},
            {"count", files.is_array() ? files.size() : 0},
            {"files", files},
            {"suppressedRunCap", artifact_ledger.suppressed_run_cap()},
            {"suppressedFlood", artifact_ledger.suppressed_flood()},
        }},
    };

    if (g.json)
        std::cout << report.dump(2) << "\n";
    else if (!result.ok)
        print_plain_errors(result);

    FinishInfo fin{result, report, files, &artifact_ledger, src};
    if (g_hooks.finish)
        return g_hooks.finish(fin);
    if (result.ok)
        return 0;
    return result.exit_code == 0 ? 1 : result.exit_code;
}

void register_session(CLI::App& session) {
    g.session_cmd = &session;
    session.require_subcommand(1);

    g.session_list_cmd = session.add_subcommand(
        "list", "List live sessions (stale entries are reaped on scan).");
    g.session_list_cmd->add_flag("--json", g.session_json, "Emit JSON.");

    g.session_start_cmd = session.add_subcommand(
        "start", "Host a bare session and block until stopped (Ctrl+C or `session stop`).");
    g.session_start_cmd->add_option("--key", g.session_key, "Session name/key (default: auto host-<ms>).");
    g.session_start_cmd->add_option("--label", g.session_label, "Human-friendly label.");
    g.session_start_cmd->add_flag("--json", g.session_json, "Emit JSON.");

    g.session_stop_cmd = session.add_subcommand("stop", "Signal a session (or --all) to stop.");
    g.session_stop_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_stop_cmd->add_flag("--all", g.session_all, "Target every live session.");
    g.session_stop_cmd->add_flag("--json", g.session_json, "Emit JSON replies.");

    g.session_ping_cmd = session.add_subcommand(
        "ping", "Send a ping to a session (or --all); prints the pong reply.");
    g.session_ping_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_ping_cmd->add_flag("--all", g.session_all, "Target every live session.");
    g.session_ping_cmd->add_flag("--json", g.session_json, "Emit JSON replies.");

    g.session_pong_cmd = session.add_subcommand(
        "pong", "Send a pong to a session (or --all); prints the ping reply.");
    g.session_pong_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_pong_cmd->add_flag("--all", g.session_all, "Target every live session.");
    g.session_pong_cmd->add_flag("--json", g.session_json, "Emit JSON replies.");

    g.session_info_cmd = session.add_subcommand(
        "info", "Show a session's details (xblox file, cwd, pid, uptime) and live vars.");
    g.session_info_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_info_cmd->add_flag("--json", g.session_json, "Emit JSON.");

    g.session_get_cmd = session.add_subcommand("get", "Read a variable from a session's published scope.");
    g.session_get_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_get_cmd->add_option("--name", g.session_name, "Variable name.")->required();
    g.session_get_cmd->add_flag("--json", g.session_json, "Emit JSON reply.");

    g.session_set_cmd = session.add_subcommand(
        "set", "Set a variable on a session (applied at the next loop pass).");
    g.session_set_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_set_cmd->add_option("--name", g.session_name, "Variable name.")->required();
    g.session_set_cmd->add_option("--value", g.session_value, "Value (parsed as JSON when possible, else a string).");
    g.session_set_cmd->add_flag("--json", g.session_json, "Emit JSON reply.");

    g.session_vars_cmd = session.add_subcommand("vars", "List a session's published variables.");
    g.session_vars_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_vars_cmd->add_flag("--json", g.session_json, "Emit compact JSON.");

    g.session_send_cmd = session.add_subcommand("send", "Send a directed message to a session's inbox.");
    g.session_send_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_send_cmd->add_option("--message", g.session_message, "Message text.");
    g.session_send_cmd->add_option("--from", g.session_from, "Sender identity (default: cli).");
    g.session_send_cmd->add_flag("--json", g.session_json, "Emit JSON reply.");

    g.session_broadcast_cmd = session.add_subcommand(
        "broadcast", "Send a message to every live session's inbox.");
    g.session_broadcast_cmd->add_option("--message", g.session_message, "Message text.");
    g.session_broadcast_cmd->add_option("--from", g.session_from, "Sender identity (default: cli).");

    g.session_recv_cmd = session.add_subcommand("recv", "Drain (or --peek) a session's message inbox.");
    g.session_recv_cmd->add_option("--key", g.session_key, "Target session key, stem, or unambiguous prefix.");
    g.session_recv_cmd->add_flag("--peek", g.session_peek, "Read without consuming.");
    g.session_recv_cmd->add_flag("--json", g.session_json, "Emit JSON.");
}

} // namespace

void set_hooks(Hooks hooks) {
    g_hooks = std::move(hooks);
}

void set_parent_refs(ParentRefs refs) {
    g_parent = refs;
}

void register_subcommands(CLI::App& cmd) {
    g.info_cmd = cmd.add_subcommand(
        "info",
        "Print XBlox block/command metadata for builders and LLM composition.");
    g.info_cmd->add_flag("--json", g.info_json,
        "Print full JSON metadata: block groups, descriptions, params, defaults, and supported commands.");
    g.info_cmd->add_option("--commands", g.commands_path,
        "Optional commands.json override for custom command metadata.");

    g.info_schema_cmd = g.info_cmd->add_subcommand(
        "schema",
        "Resolve a provider options schema via the block_params_ui resolver. "
        "Exits 0 when the schema has properties, 1 when null/unregistered.");
    g.info_schema_cmd->add_option("--schema-path", g.info_schema_path,
        "Resolver routing key, e.g. providers.whisper or providers.replicate.image.");
    g.info_schema_cmd->add_option("--provider,-p", g.info_schema_provider,
        "Provider name shorthand; used as providers.<name> when --schema-path is omitted.");
    g.info_schema_cmd->add_option("--model,-m", g.info_schema_model,
        "Model slug forwarded to the resolver (e.g. base.en or stability-ai/sdxl).");

    g.info_options_cmd = g.info_cmd->add_subcommand(
        "options",
        "Resolve a block-param options list (array of {value,label}) via block_params_ui resolver. "
        "Exits 0 when options are returned, 1 when the path is unknown.");
    g.info_options_cmd->add_option("--path,-p", g.info_options_path,
        "Options path, e.g. providers.replicate.collections or providers.replicate.models.official.")
        ->required(true);

    g.selftest_cmd = cmd.add_subcommand(
        "selftest",
        "Run native runtime self-tests (scope frames, exit policy, muParser var factory) and print a JSON report.");

    g.run_cmd = cmd.add_subcommand(
        "run",
        "Run a blocks-file JSON document emitted by the XBlox web app.");
    g.run_cmd->add_option("--src", g.src,
        "Path to a blocks-file JSON document: { version: 1, context?: {}, roots: [...] }.")
        ->required(true);
    g.run_cmd->add_option("--commands", g.commands_path,
        "Optional commands.json override for resolving host.runCustomCommand({ id }).");
    g.run_cmd->add_flag("--json", g.json, "Print a JSON execution report.");
    g.run_cmd->add_option("--consent-ui", g.consent_ui,
        "Consent surface for security-gated tools in this run: "
        "win32 (default, native dialog) | auto | cli | auto-deny | auto-allow | preset. "
        "An llmAgent block's consentUi param overrides this per turn.")
        ->group("Security");
    g.run_cmd->add_option("--consent-owner", g.consent_owner,
        "Target descriptor (ipc id) for --consent-ui owner routing. Reserved.")
        ->group("Security");
    g.event_mode_opt = g.run_cmd->add_option("--event-mode", g.event_mode,
        "Event retention: full stores every event, errors stores only failures, "
        "count stores no events. Default: errors (full with --json).")
        ->check(CLI::IsMember({"full", "errors", "count"}));
    g.run_cmd->add_flag("--dry-run", g.dry_run,
        "Stage CLI/external commands but do not spawn child processes.");
    g.run_cmd->add_flag("--simulate", g.simulate,
        "Simulate execution: resolve inputs and run pure dataflow/control blocks, "
        "but suppress side-effecting blocks (fs writes, shell, network, capture, "
        "external commands), emitting predicted 'simulated' events instead.");
    CLI::Option* md_opt = g.run_cmd->add_flag("--md", g.md,
        "Print the document as brief pseudo-code markdown (context bullets + script tree) "
        "and exit without running it.");
    g.run_cmd->add_flag("--md-numbered", g.md_numbered,
        "Use numeric bullets (1. / 2.) instead of dashes in --md output.")
        ->needs(md_opt);
    g.run_cmd->add_option("--md-filter", g.md_filter,
        "Block kinds excluded from --md output, with their subtrees "
        "(repeatable or comma-separated). Default: stdout. Pass 'none' to include everything.")
        ->delimiter(',')
        ->needs(md_opt);
    CLI::Option* mermaid_opt = g.run_cmd->add_flag("--mermaid", g.mermaid,
        "Render the script section as a fenced mermaid flowchart instead of the bullet tree. "
        "Context variables stay as markdown bullets above the diagram. Requires --md.")
        ->needs(md_opt);
    g.run_cmd->add_option("--mermaid-type", g.mermaid_type,
        "Diagram type: flow (default; flowchart with decision diamonds) or sequence "
        "(registry groups as participants, control flow as alt/loop fragments).")
        ->check(CLI::IsMember({"flow", "sequence"}))
        ->needs(mermaid_opt);
    g.run_cmd->add_option("--mermaid-direction", g.mermaid_direction,
        "Flowchart direction: TD (default, vertical), LR, BT, or RL. Applies to --mermaid and --diagram.")
        ->check(CLI::IsMember({"TD", "LR", "BT", "RL"}));
    g.run_cmd->add_option("--mermaid-color", g.mermaid_color,
        "Colouring: edges (yes/no/loop/case link tints), groups (node strokes "
        "per registry group + flow diamonds), both (default), or none.")
        ->check(CLI::IsMember({"edges", "groups", "both", "none"}))
        ->needs(mermaid_opt);
    g.run_cmd->add_flag("--expand-parameters", g.expand_parameters,
        "Include set block parameters in diagram node labels "
        "(default: compact labels — group/kind, set <name>, conditions on diamonds).");
    CLI::Option* diagram_opt = g.run_cmd->add_flag("--diagram", g.diagram,
        "Render the script as a Tanit-style SVG flowchart and exit without running it. "
        "Default: raw SVG on stdout. Use --diagram-dst / --diagram-png for files. "
        "Combine with --md to embed the diagram in markdown output instead.");
    g.run_cmd->add_option("--styles", g.diagram_styles,
        "Diagram styling constants JSON. Default: ${TANIT_SHARED}/xblox-diagram-constants.json. "
        "Supports command_variables in the path. Palettes live under JSON \"themes\".")
        ->needs(diagram_opt);
    g.run_cmd->add_option("--theme", g.diagram_theme,
        "Diagram color palette: light (default) or dark. Selects themes.<name> in the styles JSON "
        "(defaultTheme when omitted).")
        ->check(CLI::IsMember({"light", "dark"}))
        ->needs(diagram_opt);
    g.run_cmd->add_option("--diagram-type", g.diagram_type,
        "Diagram layout: flow (default; flowchart with decision diamonds and param cards) "
        "or sequence (participants, lifelines, Script messages).")
        ->check(CLI::IsMember({"flow", "sequence"}))
        ->needs(diagram_opt);
    g.run_cmd->add_option("--diagram-png", g.diagram_png,
        "Rasterize the diagram to this PNG path (libvips svgload). "
        "Supports command_variables in the path.")
        ->needs(diagram_opt);
    g.run_cmd->add_option("--diagram-dst", g.diagram_dst,
        "Write a standalone .svg file (raw SVG, not markdown). "
        "Supports command_variables in the path.")
        ->needs(diagram_opt);
    g.diagram_show_arrows_opt = g.run_cmd->add_option("--show-arrows", g.diagram_show_arrows,
        "Draw arrowheads on flow edges when --show-links is true "
        "(default: true; styles JSON render.showArrows).")
        ->needs(diagram_opt);
    g.diagram_show_links_opt = g.run_cmd->add_option("--show-links", g.diagram_show_links,
        "Draw connector lines between blocks. When false, nodes are laid out "
        "compactly in script order without edges (default: true; styles JSON render.showLinks).")
        ->needs(diagram_opt);
    g.diagram_show_disabled_opt = g.run_cmd->add_option("--show-disabled", g.diagram_show_disabled,
        "Include disabled blocks in the diagram (default: false; styles JSON render.showDisabled).")
        ->needs(diagram_opt);
    g.diagram_show_variables_opt = g.run_cmd->add_option("--show-variables", g.diagram_show_variables,
        "Show storeAs targets and referenced variables on node labels "
        "(default: true; styles JSON render.showVariables).")
        ->needs(diagram_opt);
    g.diagram_show_groups_opt = g.run_cmd->add_option("--show-groups", g.diagram_show_groups,
        "Prefix registry block labels with their group, e.g. audio/audioRecord "
        "(default: true; styles JSON render.showGroups).")
        ->needs(diagram_opt);
    g.diagram_humanize_node_names_opt = g.run_cmd->add_option("--humanize-node-names", g.diagram_humanize_node_names,
        "Humanize registry block labels, e.g. shell/openPath -> Shell / Open Path "
        "(default: false; styles JSON render.humanizeNodeNames).")
        ->needs(diagram_opt);
    g.diagram_show_variable_delimiters_opt = g.run_cmd->add_option(
        "--show-variable-delimiters", g.diagram_show_variable_delimiters,
        "Wrap referenced variables as ${name}; when false show plain names "
        "(default: false; styles JSON render.showVariableDelimiters).")
        ->needs(diagram_opt);
    g.diagram_variable_link_style_opt = g.run_cmd->add_option(
        "--variable-link-style", g.diagram_variable_link_style,
        "Separator before storeAs targets: arrow, unicode (→), colon, dot, equals, none "
        "(default: unicode; styles JSON render.variableLinkStyle).")
        ->needs(diagram_opt);
    g.diagram_variable_input_style_opt = g.run_cmd->add_option(
        "--variable-input-style", g.diagram_variable_input_style,
        "Format for referenced inputs: at (@name), parens, bracket, none "
        "(default: at; styles JSON render.variableInputStyle).")
        ->needs(diagram_opt);
    g.diagram_link_style_opt = g.run_cmd->add_option("--link-style", g.diagram_link_style,
        "Edge routing style: straight (orthogonal), bezier (smooth curves), waypoints "
        "(obstacle-aware routing ported from nodehub pathfinding; default: straight; "
        "styles JSON render.linkStyle).")
        ->check(CLI::IsMember({"straight", "bezier", "waypoints"}))
        ->needs(diagram_opt);
    g.diagram_show_parameters_opt = g.run_cmd->add_option("--show-parameters", g.diagram_show_parameters,
        "Block parameter visibility on node labels: none (compact), set (wired/set params only, "
        "like wiring 'Wired params'), all (full param list, like wiring 'All params'). "
        "Styles JSON render.showParameters. --expand-parameters is equivalent to --show-parameters all.")
        ->check(CLI::IsMember({"none", "set", "all"}))
        ->needs(diagram_opt);
    g.diagram_show_parameters_grouped_opt = g.run_cmd->add_option(
        "--show-parameters-grouped", g.diagram_show_parameters_grouped,
        "Prefix each parameter with its ParamDef group (input/, options/, …). "
        "Matches wiring toolbar 'Show group'.")
        ->needs(diagram_opt);
    g.diagram_show_parameter_values_opt = g.run_cmd->add_option(
        "--show-parameter-values", g.diagram_show_parameter_values,
        "Include parameter values in node labels when --show-parameters is set or all "
        "(matches wiring toolbar 'Show set values').")
        ->needs(diagram_opt);
    g.run_cmd->add_flag("--no-wait", g.no_wait, "Skip sleeping for wait blocks.");
    g.run_cmd->add_option("--max-loop-iterations", g.max_loop_iterations,
        "Maximum iterations per for/while loop block.")
        ->default_val(10000)
        ->check(CLI::Range(0, 1000000));
    g.run_cmd->add_flag("--loop", g.loop,
        "Re-run the whole document until cancelled (Ctrl+C). Overrides the document's stored loop setting.");
    g.run_cmd->add_flag("--no-loop", g.no_loop,
        "Force a single pass even if the document enables looping.");
    g.loop_interval_opt = g.run_cmd->add_option("--loop-interval-ms", g.loop_interval_ms,
        "Gap in milliseconds between loop passes (0 = as fast as possible, yielding each pass). Clamped to [0, 86400000] (1 day).")
        ->check(CLI::Range(0LL, 86400000LL));
    g.run_cmd->add_flag("--loop-reset", g.loop_reset,
        "Reset scope/state each loop pass (clean restart). Default persists state across passes (variables carry over).");
    g.loop_limit_opt = g.run_cmd->add_option("--loop-limit", g.loop_limit,
        "Stop after exactly N document-level loop passes (0 = unlimited). Counted at the runtime level before each inter-pass yield.")
        ->check(CLI::Range(int64_t{0}, int64_t{INT64_MAX}));
    g.run_cmd->add_flag("--quiet", g.quiet,
        "Suppress stdout block output (message is still resolved and the event is still emitted). Useful for benchmarking and CI runs.");
    g.run_cmd->add_flag("--profile", g.profile,
        "Print a per-category timing breakdown to stderr after the run (jq VM, converters, emit, set-ctx).");
    g.run_cmd->add_option("--arg", g.extra_args,
        "Extra argument appended to cliCommand/external argv command invocations; repeatable.");
    g.run_cmd->allow_extras();

    register_session(*cmd.add_subcommand(
        "session",
        "Inspect and control live xblox sessions (looping runs and bare hosts)."));
}

int dispatch() {
    if (g.info_cmd && g.info_cmd->parsed())
        return cmd_info();
    if (g.session_cmd && g.session_cmd->parsed())
        return cmd_session();
    if (g.selftest_cmd && g.selftest_cmd->parsed()) {
        std::string report;
        const int rc = scope_selftest(report);
        std::cout << report << "\n";
        return rc;
    }
    if (g.run_cmd && g.run_cmd->parsed())
        return cmd_run();
    return 0;
}

} // namespace media::xblox::cli
