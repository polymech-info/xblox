#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace media::xblox::blocks::ui {

nlohmann::json bootstrap_payload();
nlohmann::json resolve_options_path(const std::string& path);
nlohmann::json resolve_request(const nlohmann::json& request);

/// Load an `.xblox` document's context variables and attach the ParamDef of
/// the first block field bound as `${name}` (or an explicit variable source).
/// Shape: `{ path, requestedPath, variables:[{name,type,value,text,public,param,source}], error? }`.
nlohmann::json script_context_info(const std::string& path);
nlohmann::json resolve_provider_options_schema(const nlohmann::json& request);

/// Handle the xbloxMcpProbeInline RPC:
///   request = { "serverConfig": {…}, "force": bool }
///
/// Probes a single inline MCP server (uses a slug-keyed disk cache — see
/// mcp_config.hpp mcp_probe_inline_server).  Only call this from an explicit
/// user action; never from a panel render path.
///
/// Returns:
///   { "ok": true,  "tools": [{value, label, description?}], "slug": "…" }
///   { "ok": false, "error": "…", "tools": [] }
nlohmann::json resolve_mcp_probe_inline(const nlohmann::json& request);

/// Handle the xbloxMcpRefreshCatalog RPC:
///   request = {} (no parameters)
///
/// Triggers a live tools/list handshake for every enabled server in mcp.json
/// (allow_probe=true) and writes the result to the disk cache.  Only call from
/// an explicit "Refresh catalog" user action; never from a render path.
///
/// Returns:
///   { "ok": true,  "serverCount": N, "note": "…" }
///   { "ok": false, "error": "…" }
nlohmann::json resolve_mcp_refresh_catalog(const nlohmann::json& request);

} // namespace media::xblox::blocks::ui
