#pragma once

#include "json/json_utils.hpp"
#include "lib/net/http_client.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace media::xblox::blocks {

inline std::string json_string_field(const nlohmann::json& obj, const char* key)
{
    if (!obj.contains(key) || !obj[key].is_string())
        return {};
    return obj[key].get<std::string>();
}

/// Normalize argv tokens written as a comma-separated string or string array.
inline nlohmann::json normalize_string_array_field(const nlohmann::json& value)
{
    if (value.is_array()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : value) {
            if (!item.is_string())
                continue;
            const std::string tok = item.get<std::string>();
            if (!tok.empty())
                arr.push_back(tok);
        }
        return arr;
    }

    if (value.is_string()) {
        nlohmann::json arr = nlohmann::json::array();
        std::istringstream ss(value.get<std::string>());
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            const auto l = tok.find_first_not_of(" \t\r\n");
            const auto r = tok.find_last_not_of(" \t\r\n");
            if (l != std::string::npos)
                arr.push_back(tok.substr(l, r - l + 1));
        }
        return arr;
    }

    return nlohmann::json::array();
}

/// Normalize inline serverConfig from the property panel into the native types
/// expected by create_mcp_session / parse_mcp_server_endpoint.
///
///   args    — comma-separated string or string[] → string[]
///   headers — fetch-style JSON object / "Name: value" array / string → object
///   env     — JSON string or object → object
inline nlohmann::json normalize_mcp_server_config(nlohmann::json cfg)
{
    if (cfg.contains("args"))
        cfg["args"] = normalize_string_array_field(cfg["args"]);

    nlohmann::json explicit_headers = nlohmann::json::object();
    if (cfg.contains("headers")) {
        explicit_headers = media::net::parse_json_headers_object(cfg["headers"]);
        if (explicit_headers.empty() && (cfg["headers"].is_string() || cfg["headers"].is_array()))
            cfg.erase("headers");
    }

    if (cfg.contains("env")) {
        if (cfg["env"].is_string()) {
            nlohmann::json parsed = media::json::parse_object_or(cfg["env"].get<std::string>());
            if (parsed.empty())
                cfg.erase("env");
            else
                cfg["env"] = std::move(parsed);
        } else if (!cfg["env"].is_object()) {
            cfg.erase("env");
        }
    }

    // Fetch-style auth → merge into headers (same rules as network/fetch).
    media::net::Auth auth;
    auth.kind   = media::net::parse_auth_kind(json_string_field(cfg, "auth"));
    auth.token  = json_string_field(cfg, "authToken");
    auth.user   = json_string_field(cfg, "authUser");
    auth.pass   = json_string_field(cfg, "authPass");
    auth.header = json_string_field(cfg, "authHeader");
    if (auth.header.empty())
        auth.header = "X-Api-Key";
    if (auth.kind != media::net::AuthKind::None) {
        nlohmann::json headers = nlohmann::json::object();
        media::net::apply_auth_to_headers(auth, headers);
        if (explicit_headers.is_object()) {
            for (auto it = explicit_headers.begin(); it != explicit_headers.end(); ++it)
                headers[it.key()] = it.value();
        }
        cfg["headers"] = std::move(headers);
    } else if (explicit_headers.is_object() && !explicit_headers.empty()) {
        cfg["headers"] = std::move(explicit_headers);
    } else if (cfg.contains("headers") && explicit_headers.empty()) {
        cfg.erase("headers");
    }

    cfg.erase("auth");
    cfg.erase("authToken");
    cfg.erase("authUser");
    cfg.erase("authPass");
    cfg.erase("authHeader");

    return cfg;
}

} // namespace media::xblox::blocks
