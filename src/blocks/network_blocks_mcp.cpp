#include "blocks/network_blocks.hpp"

#include "blocks/block_params.hpp"
#include "blocks/block_registry.hpp"
#include "blocks/json_templates.hpp"
#include "blocks/mcp_server_config_normalize.hpp"

#if FEATURE_MCP_CLIENT
#include "llm/mcp_config.hpp"
#endif

#include <string>

namespace media::xblox::blocks {
namespace {

std::string trim_policy_warning(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

} // namespace

#if FEATURE_MCP_CLIENT

BlockResult mcp_call_block(BlockIO& io)
{
    const std::string server = io.str("server");
    const std::string tool   = io.str("tool");
    if (server.empty()) {
        io.error("mcpCall", "server is required", 2);
        return BlockResult::Error;
    }
    if (tool.empty()) {
        io.error("mcpCall", "tool is required", 2);
        return BlockResult::Error;
    }

    const bool is_custom = (server == "__custom__");

    // ── Profile config ────────────────────────────────────────────────────────
    // Always attempt to load for policy settings.  For named servers it is also
    // the source of the server config object.  For custom servers a load failure
    // is non-fatal (the inline config is the source of truth).
    nlohmann::json profile_cfg;
    std::string    profile_err;
    const bool     profile_loaded = media::llm::mcp::load_mcp_config({}, profile_cfg, profile_err);

    // ── Resolve server config object ──────────────────────────────────────────
    nlohmann::json srv;
    std::string    server_key;

    if (is_custom) {
        // Read the raw (pre-resolve) config first — the slug / probe cache must
        // use the unresolved form so ${USER:…} secrets don't leak into filenames.
        // Then resolve templates for session creation.
        const nlohmann::json raw_cfg = io.json("serverConfig");
        srv = resolve_json_templates(io, raw_cfg);
        if (!srv.is_object() || srv.empty()) {
            io.error("mcpCall",
                     "serverConfig is required when server is __custom__ "
                     "(configure transport, command/url, and args in the block)",
                     2);
            return BlockResult::Error;
        }
        server_key = "__inline__";

        // Policy: inline servers are never security.registered, so when the
        // profile requires all servers to be registered we must fail with a
        // clear, actionable message rather than a generic policy string.
        if (profile_loaded && media::llm::mcp::require_registered_mcp(profile_cfg)) {
            io.error("mcpCall",
                     "inline MCP servers are blocked by policy: requireRegisteredMcp is enabled — "
                     "add the server to mcp.json and mark it as registered instead",
                     1);
            return BlockResult::Error;
        }

        // Normalize UI-serialised strings (comma-sep args, JSON-string headers/env)
        // into the native types expected by create_mcp_session / parse_mcp_server_endpoint.
        srv = normalize_mcp_server_config(std::move(srv));
    } else {
        if (!profile_loaded) {
            io.error("mcpCall", profile_err.empty() ? "failed to load mcp config" : profile_err, 1);
            return BlockResult::Error;
        }
        if (!profile_cfg.contains("mcpServers") || !profile_cfg["mcpServers"].is_object() ||
            !profile_cfg["mcpServers"].contains(server)) {
            io.error("mcpCall", "MCP server not found in mcp.json: " + server, 2);
            return BlockResult::Error;
        }
        srv = profile_cfg["mcpServers"][server];
        if (!srv.is_object()) {
            io.error("mcpCall", "MCP server config must be an object: " + server, 2);
            return BlockResult::Error;
        }
        if (srv.contains("enabled") && srv["enabled"].is_boolean() && !srv["enabled"].get<bool>()) {
            io.error("mcpCall", "MCP server is disabled: " + server, 2);
            return BlockResult::Error;
        }
        server_key = server;
    }

    // ── Policy / security check ───────────────────────────────────────────────
    media::llm::mcp::McpServerEndpoint endpoint;
    media::llm::mcp::parse_mcp_server_endpoint(srv, endpoint);

    // Inline servers can't be registered; pass require_registered=false so that
    // the policy function applies its non-registration checks only (sandbox,
    // hash, allowed-hosts) without double-failing on registration.
    const bool require_registered =
        !is_custom && profile_loaded && media::llm::mcp::require_registered_mcp(profile_cfg);

    std::string policy_warning;
    if (!media::llm::mcp::mcp_server_policy_allows(
            srv, server_key, endpoint.command, require_registered, policy_warning)) {
        const std::string msg = trim_policy_warning(std::move(policy_warning));
        io.error("mcpCall", msg.empty() ? "MCP server blocked by policy" : msg, 1);
        return BlockResult::Error;
    }

    // ── Session ───────────────────────────────────────────────────────────────
    media::llm::mcp::McpSessionCreateOptions session_opts;
    session_opts.timeout_ms = static_cast<int>(std::max<long long>(
        1, io.i64("timeoutMs", media::llm::mcp::mcp_default_timeout_ms())));
    session_opts.cancel_check = [&io]() { return io.cancelled(); };

    std::string session_err;
    std::unique_ptr<media::llm::mcp::IMcpSession> client =
        media::llm::mcp::create_mcp_session(srv, server_key, session_err, session_opts);
    if (!client) {
        io.error("mcpCall", session_err.empty() ? "failed to create MCP session" : session_err, 1);
        return BlockResult::Error;
    }

    std::string init_err;
    if (!client->initialize(init_err)) {
        client->try_delete_session();
        io.error("mcpCall", "handshake failed: " + init_err, 1);
        return BlockResult::Error;
    }

    nlohmann::json args = resolve_json_templates(io, io.json("arguments"));
    if (!args.is_object())
        args = nlohmann::json::object();

    std::string call_err;
    nlohmann::json result = client->tools_call(tool, args, call_err);
    client->try_delete_session();

    if (!call_err.empty()) {
        io.error("mcpCall", call_err, 1)
          .set("server", server_key)
          .set("tool",   tool);
        return BlockResult::Error;
    }

    if (result.is_object() && result.value("isError", false)) {
        io.error("mcpCall", "tool returned isError", 1)
          .set("server", server_key)
          .set("tool",   tool)
          .set("result", Value::from_json(result));
        return BlockResult::Error;
    }

    io.ok("mcpCall", "MCP tool call complete")
      .set("server", server_key)
      .set("tool",   tool)
      .set("result", Value::from_json(result))
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

#else

BlockResult mcp_call_block(BlockIO& io)
{
    io.error("mcpCall", "MCP client disabled in this build", 1);
    return BlockResult::Error;
}

#endif

void register_network_mcp_blocks(BlockRegistry& registry)
{
    const std::vector<ParamDef> mcp_params = {
        // ── Server selection ─────────────────────────────────────────────────
        pd_str("server").in().req().grp("server")
            .ui({{"control",      "select"},
                 {"options_path", "mcp.servers"}})
            .desc("MCP server name from mcp.json, a bundled system server, or __custom__ for an "
                  "inline config.  Supports ${var} interpolation."),

        pd_json("serverConfig").grp("connection").lbl("Server config")
            .dflt(nlohmann::json::object())
            .dyn({
                {"kind",          "mcp_server_config"},
                {"schema_path",   "mcp.server"},
                {"schema_method", "xbloxProviderOptionsSchemaGet"},
                {"value_field",   "serverConfig"},
            })
            .ui({{"visible_when", {{"server", "__custom__"}}}})
            .desc("Inline MCP server config (used only when server == __custom__). "
                  "Use ${USER:\u2026} for secrets in headers / env / args."),

        pd_str("tool").in().req().grp("request")
            .ui({{"control",               "select"},
                 {"options_path_template", "mcp.tools.{server}"},
                 {"empty_label",           "(select server or load tools)"}})
            .desc("Remote tool name on the selected server. Supports ${var} interpolation."),

        pd_json("arguments").grp("arguments").lbl("Arguments").dflt(nlohmann::json::object())
            .dyn({
                {"kind",          "provider_options"},
                {"schema_path",   "mcp.tools"},
                {"provider_field","server"},
                {"model_field",   "tool"},
                {"schema_method", "xbloxProviderOptionsSchemaGet"},
                {"value_field",   "arguments"},
            })
            .desc("Tool arguments object. Supports ${var} interpolation on all fields."),

        pd_ms("timeoutMs").grp("timeouts").dflt(60000)
            .desc("MCP request timeout in milliseconds."),
        pd_store_as().dflt("mcpResult"),
    };

    const nlohmann::json default_server_config = {
        {"type",         ""},
        {"command",      ""},
        {"args",         nlohmann::json::array()},
        {"url",          ""},
        {"auth",         "none"},
        {"authToken",    ""},
        {"authUser",     ""},
        {"authPass",     ""},
        {"authHeader",   "X-Api-Key"},
        {"headers",      nlohmann::json::object()},
        {"env",          nlohmann::json::object()},
        {"tool_timeout", 30},
    };

    registry["mcpCall"] = bdb(
        "mcpCall", mcp_call_block, "MCP Call", "Network",
        "Call a tool on an MCP server. Supports profile servers (mcp.json), bundled system "
        "servers, and inline custom configs (stdio + HTTP). Fresh session per call. When the "
        "tool result is a JSON array, child blocks iterate over each element (PREVIOUS = element, "
        "index = i); otherwise children run once with the full result as PREVIOUS.",
        {{"kind",         "mcpCall"},
         {"server",       ""},
         {"serverConfig", default_server_config},
         {"tool",         ""},
         {"arguments",    nlohmann::json::object()},
         {"timeoutMs",    60000},
         {"storeAs",      "mcpResult"},
         {"items",        nlohmann::json::array()}})
        .params(mcp_params)
        .primary_output(ParamKind::json_value, "Tool result")
        .exec({false, true, true})
        .iterable()
        .children({{"items", "For each", "body", {"*"}, 0, -1}});
}

} // namespace media::xblox::blocks
