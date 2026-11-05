// network_blocks.cpp  -  HTTP(S) request blocks backed by lib/net/http_client.
//
// Blocks: fetch · httpRequest · network
//
// All three share one handler (network_request_block).  The thin wrapper maps
// BlockIO params -> HttpRequest, calls http_fetch(), then maps the result back
// to typed block outputs.
//
// fetch is registered as an *iterable* producer: when the response is a JSON
// array (decode:json and the top-level value is []), the executor fans its
// child `items` blocks over each element (PREVIOUS = element, index = i).
// This makes `fetch -> Parse(.field) -> log` pipelines trivially composable.
//
// New params vs. the old network_blocks:
//   auth / authToken / authUser / authPass / authHeader
//   proxy / proxyUser / proxyPass
//   insecure  (skip SSL verification)
//   downloadAs  (save body to file; PREVIOUS = saved path)
//   cache / cacheDir / cacheTtlMs  (disk cache with ETag revalidation)
//
// Exponential back-off replaces the fixed retry delay:
//   delay_ms = retryDelayMs * 2^attempt + rand(0, retryDelayMs)

#include "blocks/network_blocks.hpp"
#include "json/json_utils.hpp"
#include "blocks/block_variables.hpp"
#include "blocks/json_templates.hpp"
#include "lib/net/http_client.hpp"
#include "string/string_utils.hpp"
#include "xblox/xblox_constants.hpp"
#include "xblox/xblox_session.hpp"
#include "policy/app_policy.hpp"

#if defined(FEATURE_SSH_XBLOX) && FEATURE_SSH_XBLOX
#include "lib/net/ssh_exec.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "fs/path_sanitizer.hpp"

namespace media::xblox::blocks {
namespace {

namespace xblox_constants = media::xblox::constants;

std::string upper_ascii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

using media::strutil::lower_ascii;

// ── Simple dot-path selector (unchanged from previous implementation) ─────────

std::vector<std::string> split_filter_path(std::string filter)
{
    std::vector<std::string> parts;
    if (!filter.empty() && filter.front() == '.')
        filter.erase(filter.begin());
    std::string current;
    for (char ch : filter) {
        if (ch == '.') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        parts.push_back(std::move(current));
    return parts;
}

const nlohmann::json* select_path(const nlohmann::json& value, const std::string& filter)
{
    if (filter.empty() || filter == ".")
        return &value;
    const auto parts = split_filter_path(filter);
    const nlohmann::json* current = &value;
    for (const auto& part : parts) {
        if (!current) return nullptr;
        if (current->is_object()) {
            if (!current->contains(part)) return nullptr;
            current = &(*current)[part];
            continue;
        }
        if (current->is_array()) {
            try {
                size_t consumed = 0;
                const size_t idx = static_cast<size_t>(std::stoull(part, &consumed));
                if (consumed != part.size() || idx >= current->size()) return nullptr;
                current = &(*current)[idx];
                continue;
            } catch (...) {
                return nullptr;
            }
        }
        return nullptr;
    }
    return current;
}

std::string preview_text(const std::string& text, std::size_t max_len = 4000)
{
    if (text.size() <= max_len)
        return text;
    return text.substr(0, max_len) + "...[truncated " + std::to_string(text.size() - max_len) + " bytes]";
}

std::string mask_secret(const std::string& value)
{
    if (value.empty())
        return {};
    if (media::strutil::has_variable_tokens(value))
        return value;
    if (value.size() <= 8)
        return std::string(value.size(), '*');
    return value.substr(0, 4) + "..." + value.substr(value.size() - 4) +
           " (len=" + std::to_string(value.size()) + ")";
}

std::string trim_copy(std::string s)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool sensitive_header_name(std::string name)
{
    name = lower_ascii(trim_copy(std::move(name)));
    return name == "authorization" ||
           name == "proxy-authorization" ||
           name == "cookie" ||
           name == "set-cookie" ||
           name.find("api-key") != std::string::npos ||
           name.find("apikey") != std::string::npos ||
           name.find("token") != std::string::npos;
}

bool sensitive_json_field_name(std::string name)
{
    name = lower_ascii(trim_copy(std::move(name)));
    return sensitive_header_name(name) ||
           name == "document_key" ||
           name == "auth_key" ||
           name == "api_key" ||
           name == "access_token" ||
           name == "refresh_token";
}

std::string mask_header_line(const std::string& header)
{
    const std::size_t colon = header.find(':');
    if (colon == std::string::npos)
        return header;
    const std::string name = trim_copy(header.substr(0, colon));
    const std::string value = trim_copy(header.substr(colon + 1));
    if (!sensitive_header_name(name))
        return header;
    return name + ": " + mask_secret(value);
}

nlohmann::json masked_headers_json(const std::vector<std::string>& headers)
{
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& header : headers)
        out.push_back(mask_header_line(header));
    return out;
}

nlohmann::json mask_sensitive_json(nlohmann::json value)
{
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (sensitive_json_field_name(it.key())) {
                if (it.value().is_string())
                    it.value() = mask_secret(it.value().get<std::string>());
                else if (!it.value().is_null())
                    it.value() = "<redacted>";
            } else {
                it.value() = mask_sensitive_json(std::move(it.value()));
            }
        }
    } else if (value.is_array()) {
        for (auto& item : value)
            item = mask_sensitive_json(std::move(item));
    }
    return value;
}

std::string preview_body_text(const std::string& text, std::size_t max_len = 4000)
{
    // Network bodies are often HTML/plain text — never abort the run on a debug
    // preview parse failure (xbloxDocumentRun executes on a worker thread).
    nlohmann::json parsed = media::json::try_parse(text);
    if (!parsed.is_discarded()) {
        try {
            return preview_text(mask_sensitive_json(std::move(parsed)).dump(), max_len);
        } catch (...) {
        }
    }
    return preview_text(text, max_len);
}

std::string masked_auth_preview(const media::net::Auth& auth)
{
    switch (auth.kind) {
    case media::net::AuthKind::Bearer:
        return "Authorization: Bearer " + mask_secret(auth.token);
    case media::net::AuthKind::Basic:
        return "Authorization: Basic <user=" + auth.user + ", pass=" + mask_secret(auth.pass) + ">";
    case media::net::AuthKind::ApiKey: {
        const std::string header = auth.header.empty() ? std::string("X-Api-Key") : auth.header;
        return header + ": " + mask_secret(auth.token);
    }
    default:
        return {};
    }
}

nlohmann::json multipart_part_debug(const std::vector<std::string>& parts)
{
    namespace fs = std::filesystem;
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& part : parts) {
        nlohmann::json item{{"raw", part}};
        const auto at = part.find('@');
        const auto eq = part.find('=');
        if (at != std::string::npos && (eq == std::string::npos || at < eq)) {
            std::string name = part.substr(0, at);
            const std::string filepath = part.substr(at + 1);
            if (name.empty())
                name = media::path::path_to_utf8(media::path::path_from_utf8(filepath).filename());
            item["kind"] = "file";
            item["name"] = name;
            item["path"] = filepath;
            std::error_code ec;
            const fs::path p = media::path::path_from_utf8(filepath);
            item["exists"] = fs::exists(p, ec);
            if (ec)
                item["statError"] = ec.message();
            else if (item["exists"].get<bool>() && fs::is_regular_file(p, ec))
                item["bytes"] = static_cast<long long>(fs::file_size(p, ec));
            if (ec)
                item["statError"] = ec.message();
        } else if (eq != std::string::npos) {
            const std::string name = part.substr(0, eq);
            const std::string value = part.substr(eq + 1);
            item["kind"] = "field";
            item["name"] = name;
            item["value"] = sensitive_header_name(name) ? mask_secret(value) : preview_text(value, 200);
        } else {
            item["kind"] = "file";
            item["name"] = media::path::path_to_utf8(media::path::path_from_utf8(part).filename());
            item["path"] = part;
            std::error_code ec;
            const fs::path p = media::path::path_from_utf8(part);
            item["exists"] = fs::exists(p, ec);
            if (ec)
                item["statError"] = ec.message();
            else if (item["exists"].get<bool>() && fs::is_regular_file(p, ec))
                item["bytes"] = static_cast<long long>(fs::file_size(p, ec));
            if (ec)
                item["statError"] = ec.message();
        }
        out.push_back(std::move(item));
    }
    return out;
}

// ── Block handler ─────────────────────────────────────────────────────────────

BlockResult network_request_block(BlockIO& io)
{
    const std::string url = io.str("url");
    if (url.empty() || url == "https://") {
        io.error("network", "network request is missing url", 2);
        return BlockResult::Error;
    }

    // ── Build HttpRequest ────────────────────────────────────────────────────
    media::net::HttpRequest req;

    req.url    = url;
    req.method = upper_ascii(io.str("method", "GET"));
    if (req.method.empty()) req.method = "GET";

    // ── Query-string parameters ───────────────────────────────────────────────
    {
        const nlohmann::json qp = resolve_json_templates(io, io.json("queryParams"));
        if (qp.is_object()) {
            for (auto it = qp.begin(); it != qp.end(); ++it) {
                const std::string val = it.value().is_string()
                    ? it.value().get<std::string>() : it.value().dump();
                req.query_params.push_back(it.key() + "=" + val);
            }
        }
    }

    // ── Request body  -  dispatched on bodyType ────────────────────────────────
    const std::string body_type = io.str("bodyType", "none");

    if (body_type == "raw") {
        req.body = io.str("body");
    } else if (body_type == "json") {
        const nlohmann::json bj = resolve_json_templates(io, io.json("bodyJson"));
        req.body         = bj.is_null() ? std::string{} : bj.dump();
        req.content_type = "application/json";
    } else if (body_type == "form") {
        const nlohmann::json fields = resolve_json_templates(io, io.json("bodyFields"));
        if (fields.is_object()) {
            auto pct_encode = [](const std::string& s) {
                std::string out;
                out.reserve(s.size() * 3);
                for (unsigned char c : s) {
                    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        out += static_cast<char>(c);
                    } else {
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned>(c));
                        out += buf;
                    }
                }
                return out;
            };
            std::string encoded;
            for (auto it = fields.begin(); it != fields.end(); ++it) {
                if (!encoded.empty()) encoded += '&';
                const std::string val = it.value().is_string()
                    ? it.value().get<std::string>() : it.value().dump();
                encoded += pct_encode(it.key()) + '=' + pct_encode(val);
            }
            req.body         = std::move(encoded);
            req.content_type = "application/x-www-form-urlencoded";
        }
    } else if (body_type == "multipart") {
        const nlohmann::json parts = resolve_json_templates(io, io.json("bodyParts"));
        if (parts.is_array()) {
            for (const auto& p : parts)
                if (p.is_string()) req.body_parts.push_back(p.get<std::string>());
        }
    }
    // "none" (default)  -  no body

    // Headers
    const nlohmann::json raw_headers = resolve_json_templates(io, io.json("headers"));
    media::net::append_json_headers(&raw_headers, req.headers);

    // Auth
    req.auth.kind = media::net::parse_auth_kind(io.str("auth"));
    if (req.auth.kind != media::net::AuthKind::None) {
        req.auth.token  = io.str("authToken");
        req.auth.user   = io.str("authUser");
        req.auth.pass   = io.str("authPass");
        req.auth.header = io.str("authHeader", "X-Api-Key");
    }

    // Proxy
    req.proxy      = io.str("proxy");
    req.proxy_user = io.str("proxyUser");
    req.proxy_pass = io.str("proxyPass");

    // Session (persistent cookie jar)
    req.session_id  = io.str("sessionId");
    req.session_dir = io.str("sessionDir");

    // SSL
    const bool insecure = io.flag("insecure", false);
    req.verify_peer = !insecure && io.flag("verifyPeer", true);
    req.verify_host = !insecure && io.flag("verifyHost", true);

    // Redirects / timeouts / retry
    req.timeout_ms         = std::max<long>(1, static_cast<long>(
        io.i64("timeoutMs", io.runtime().options.default_timeout_ms)));
    req.connect_timeout_ms = std::max<long>(1, static_cast<long>(
        io.i64("connectTimeoutMs", std::min<long>(req.timeout_ms, xblox_constants::kNetworkDefaultConnectTimeoutMs))));
    req.follow_redirects   = io.flag("followRedirects", true);
    req.max_redirects      = std::max<long>(0, static_cast<long>(
        io.i64("maxRedirects", xblox_constants::kNetworkDefaultMaxRedirects)));
    req.retries            = static_cast<int>(std::max<long long>(
        0, io.i64("retries", xblox_constants::kNetworkDefaultRetries)));
    req.retry_delay_ms     = static_cast<int>(std::max<long long>(0, io.i64("retryDelayMs", xblox_constants::kNetworkDefaultRetryDelayMs)));

    // Download-as
    req.download_as = io.str("downloadAs");

    // Cache
    req.cache.enabled         = io.flag("cache", false);
    req.cache.cache_dir       = io.str("cacheDir");
    req.cache.ttl_ms          = static_cast<int64_t>(io.i64("cacheTtlMs", xblox_constants::kNetworkDefaultCacheTtlMs));
    req.cache.force_revalidate = false;

    // Cancel hook
    auto& rt = io.runtime();
    req.cancel_check = [&rt]() -> bool {
        return rt.cancel_requested && rt.cancel_requested(rt.user_data);
    };

    // Decode mode
    std::string decode = lower_ascii(io.str("decode", "raw"));
    if (decode.empty()) decode = "raw";
    if (decode != "raw" && decode != "json") {
        io.error("network", "unsupported network decode: " + decode, 2);
        return BlockResult::Error;
    }

    {
        nlohmann::json debug{
            {"url", req.url},
            {"method", req.method},
            {"headers", masked_headers_json(req.headers)},
            {"authHeader", masked_auth_preview(req.auth)},
            {"bodyType", body_type},
            {"bodyBytes", req.body.size()},
            {"body", preview_body_text(req.body)},
            {"bodyParts", req.body_parts},
            {"multipart", multipart_part_debug(req.body_parts)},
            {"downloadAs", req.download_as},
            {"decode", decode},
            {"parse", io.str_any({"parse", "filter", "query"}, ".")},
        };
        io.debug("network request: " + debug.dump());
    }

    // ── Execute ──────────────────────────────────────────────────────────────
    const media::net::HttpResponse resp = media::net::http_fetch(req);

    io.debug(std::string("network response: ") + nlohmann::json{
        {"ok", resp.ok},
        {"statusCode", resp.status_code},
        {"attempts", resp.attempts},
        {"fromCache", resp.from_cache},
        {"savedPath", resp.saved_path},
        {"effectiveUrl", resp.effective_url},
        {"error", resp.error},
        {"bodyBytes", resp.body.size()},
        {"body", preview_body_text(resp.body)}
    }.dump());

    // ── Failure ───────────────────────────────────────────────────────────────
    if (!resp.ok) {
        const int exit_code = resp.error.find("timeout") != std::string::npos ? 124
                            : resp.error.find("cancelled") != std::string::npos ? 130 : 1;
        io.error("network", resp.error.empty() ? "network request failed" : resp.error, exit_code)
          .set("url",        url)
          .set("method",     req.method)
          .set("attempts",   static_cast<long long>(resp.attempts))
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)});
        return BlockResult::Error;
    }

    // ── Success ───────────────────────────────────────────────────────────────

    if (!resp.saved_path.empty()) {
        io.ok("network", "download complete")
          .set("url",        resp.effective_url)
          .set("method",     req.method)
          .set("attempts",   static_cast<long long>(resp.attempts))
          .set("decode",     decode)
          .set("fromCache",  resp.from_cache)
          .set("headers",    Value::from_json(resp.response_headers))
          .set("savedPath",  resp.saved_path)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)})
          .result(Value{resp.saved_path}, ParamKind::string_);
        return BlockResult::Ok;
    }

    // In-memory body path
    Value primary{resp.body};
    ParamKind primary_kind = ParamKind::string_;

    if (decode == "json") {
        nlohmann::json decoded;
        try {
            decoded = nlohmann::json::parse(resp.body);
        } catch (const std::exception& e) {
            io.error("network", std::string("json decode failed: ") + e.what(), 2)
              .set("url",        resp.effective_url)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)});
            return BlockResult::Error;
        }
        const std::string filter = io.str_any({"parse", "filter", "query"}, ".");
        const nlohmann::json* selected = select_path(decoded, filter);
        if (!selected) {
            io.error("network", "json parse filter returned no value: " + filter, 2)
              .set("url",        resp.effective_url)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)});
            return BlockResult::Error;
        }
        primary      = Value::from_json(*selected);
        primary_kind = ParamKind::json_value;
    }

    auto ev = io.ok("network", "network request complete");
    ev.set("url",        resp.effective_url)
      .set("method",     req.method)
      .set("attempts",   static_cast<long long>(resp.attempts))
      .set("decode",     decode)
      .set("fromCache",  resp.from_cache)
      .set("headers",    Value::from_json(resp.response_headers))
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)});
    if (!resp.saved_path.empty())
        ev.set("savedPath", resp.saved_path);
    ev.result(primary, primary_kind);
    return BlockResult::Ok;
}

#if defined(FEATURE_SSH_XBLOX) && FEATURE_SSH_XBLOX
BlockResult ssh_exec_block(BlockIO& io)
{
    media::net::SshExecOptions o;
    o.host                = io.str("host");
    o.command             = io.str("command");
    o.username            = io.has("username") ? io.str_any({"username", "user"}) : std::string{};
    o.password            = io.str("password");
    o.passphrase          = io.str("passphrase");
    o.private_key         = io.str_any({"privateKey", "keyFile"});
    o.config_file         = io.str("configFile");
    o.known_hosts         = io.str("knownHosts");
    o.port                = static_cast<int>(std::clamp<long long>(io.i64("port", 22), 1, 65535));
    o.port_set            = io.has("port");
    o.verify_host         = io.flag("verifyHost", true);
    o.accept_unknown_host = io.flag("acceptUnknownHost", false);
    o.timeout_ms          = static_cast<int>(std::max<long long>(
        1, io.i64("timeoutMs", io.runtime().options.default_timeout_ms)));
    o.cancelled           = [&io] { return io.cancelled(); };

    const auto r = media::net::ssh_exec(o);
    if (!r.ran) {
        io.error("sshExec", r.error, r.error_code)
          .set("host", o.host)
          .set("port", static_cast<long long>(o.port));
        return BlockResult::Error;
    }

    nlohmann::json result{
        {"host", o.host},
        {"port", o.port},
        {"command", o.command},
        {"exitStatus", r.exit_status},
        {"stdout", r.stdout_text},
        {"stderr", r.stderr_text},
    };
    io.ok("sshExec", r.exit_status == 0 ? "SSH command complete" : "SSH command exited non-zero")
      .set("host", o.host)
      .set("port", static_cast<long long>(o.port))
      .set("exitStatus", static_cast<long long>(r.exit_status))
      .set("stdout", r.stdout_text)
      .set("stderr", r.stderr_text)
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return r.exit_status == 0 ? BlockResult::Ok : BlockResult::Error;
}
#endif

nlohmann::json session_descriptors_json()
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : media::xblox::session::list_sessions())
        arr.push_back(d.to_json());
    return arr;
}

std::string trim_ascii_local(std::string s)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string normalize_named_session_key(std::string name)
{
    name = trim_ascii_local(std::move(name));
    if (name.empty())
        return {};
    if (name.rfind("name:", 0) == 0 || name.rfind("file:", 0) == 0 || name.rfind("data:", 0) == 0)
        return name;
    return "name:" + name;
}

struct HostedIpcSession {
    media::xblox::session::SessionServer server;
    bool ok = false;
    std::string error;

    HostedIpcSession(std::string key, std::string label, std::string cwd)
    {
        media::xblox::session::SessionInfo info;
        info.key = std::move(key);
        info.label = std::move(label);
        info.cwd = std::move(cwd);
        info.loop = true;
        ok = server.start(info, error);
    }
};

struct CachedIpcTarget {
    std::string target;
    media::xblox::session::SessionDescriptor descriptor;
    std::unique_ptr<media::xblox::session::SessionClient> client;
    std::chrono::steady_clock::time_point expires_at{};
};

nlohmann::json ipc_message_to_json(const media::xblox::session::IpcMessage& msg)
{
    nlohmann::json out{
        {"from", msg.from},
        {"message", msg.message},
        {"ts", msg.ts},
    };
    if (msg.has_data)
        out["data"] = msg.data;
    return out;
}

nlohmann::json ipc_messages_to_json(const std::vector<media::xblox::session::IpcMessage>& messages)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& msg : messages)
        out.push_back(ipc_message_to_json(msg));
    return out;
}

std::string ipc_target_cache_key(const std::string& target)
{
    return "ipcTarget:" + target;
}

void invalidate_session_target_cache(BlockIO& io, const std::string& target)
{
    if (target.empty())
        return;
    if (auto* scope = io.scope())
        scope->release(ipc_target_cache_key(target));
}

bool resolve_session_target(BlockIO& io, const char* kind, media::xblox::session::SessionDescriptor& out, std::string* resolved_target = nullptr)
{
    const std::string target = io.str("target");
    if (resolved_target)
        *resolved_target = target;
    if (target.empty()) {
        io.error(kind, std::string(kind) + ": target is empty", 2);
        return false;
    }

    if (auto* scope = io.scope()) {
        auto& cached = scope->acquire<CachedIpcTarget>(ipc_target_cache_key(target), [] {
            return std::make_shared<CachedIpcTarget>();
        });
        const auto now = std::chrono::steady_clock::now();
        if (cached.target == target && now < cached.expires_at && !cached.descriptor.key.empty()) {
            out = cached.descriptor;
            return true;
        }
        if (media::xblox::session::find_session(target, out)) {
            cached.target = target;
            cached.descriptor = out;
            cached.expires_at = now + xblox_constants::kIpcTargetCacheTtl;
            return true;
        }
        scope->release(ipc_target_cache_key(target));
        io.error(kind, std::string(kind) + ": no session matching " + target, 2)
          .set("target", target);
        return false;
    }

    if (!media::xblox::session::find_session(target, out)) {
        io.error(kind, std::string(kind) + ": no session matching " + target, 2)
          .set("target", target);
        return false;
    }
    return true;
}

bool send_cached_messages(BlockIO& io,
                          const char* kind,
                          const std::vector<media::xblox::session::IpcMessage>& messages,
                          media::xblox::session::SendMessagesReply& reply,
                          std::string& err,
                          int timeout_ms,
                          media::xblox::session::SessionDescriptor& target)
{
    std::string target_query;
    if (!resolve_session_target(io, kind, target, &target_query))
        return false;

    if (auto* scope = io.scope()) {
        auto* cached = scope->find<CachedIpcTarget>(ipc_target_cache_key(target_query));
        if (cached && cached->descriptor.key == target.key) {
            if (!cached->client)
                cached->client = std::make_unique<media::xblox::session::SessionClient>();
            if (!cached->client->connected() && !cached->client->connect(target, err, timeout_ms)) {
                invalidate_session_target_cache(io, target_query);
                return false;
            }
            if (cached->client->send_messages(messages, reply, err, timeout_ms))
                return true;
            invalidate_session_target_cache(io, target_query);
            return false;
        }
    }

    if (!media::xblox::session::send_messages(target, messages, reply, err, timeout_ms)) {
        invalidate_session_target_cache(io, target_query);
        return false;
    }
    return true;
}

bool receive_cached_messages(BlockIO& io,
                             const char* kind,
                             bool peek,
                             std::vector<media::xblox::session::IpcMessage>& messages,
                             std::string& err,
                             int timeout_ms,
                             media::xblox::session::SessionDescriptor& target)
{
    std::string target_query;
    if (!resolve_session_target(io, kind, target, &target_query))
        return false;

    if (auto* scope = io.scope()) {
        auto* cached = scope->find<CachedIpcTarget>(ipc_target_cache_key(target_query));
        if (cached && cached->descriptor.key == target.key) {
            if (!cached->client)
                cached->client = std::make_unique<media::xblox::session::SessionClient>();
            if (!cached->client->connected() && !cached->client->connect(target, err, timeout_ms)) {
                invalidate_session_target_cache(io, target_query);
                return false;
            }
            if (cached->client->receive_messages(peek, messages, err, timeout_ms))
                return true;
            invalidate_session_target_cache(io, target_query);
            return false;
        }
    }

    if (!media::xblox::session::receive_messages(target, peek, messages, err, timeout_ms)) {
        invalidate_session_target_cache(io, target_query);
        return false;
    }
    return true;
}

bool send_cached_session_action(BlockIO& io,
                                const char* kind,
                                const std::string& action,
                                const nlohmann::json& extra,
                                nlohmann::json& reply,
                                std::string& err,
                                int timeout_ms,
                                media::xblox::session::SessionDescriptor& target)
{
    std::string target_query;
    if (!resolve_session_target(io, kind, target, &target_query))
        return false;

    if (auto* scope = io.scope()) {
        auto* cached = scope->find<CachedIpcTarget>(ipc_target_cache_key(target_query));
        if (cached && cached->descriptor.key == target.key) {
            if (!cached->client)
                cached->client = std::make_unique<media::xblox::session::SessionClient>();
            if (!cached->client->connected() && !cached->client->connect(target, err, timeout_ms)) {
                invalidate_session_target_cache(io, target_query);
                return false;
            }
            if (cached->client->request(action, extra, reply, err, timeout_ms))
                return true;
            invalidate_session_target_cache(io, target_query);
            return false;
        }
    }

    if (!media::xblox::session::send_action(target, action, extra, reply, err, timeout_ms)) {
        invalidate_session_target_cache(io, target_query);
        return false;
    }
    return true;
}

BlockResult ipc_session_block(BlockIO& io)
{
    const std::string raw_key = io.has("name") ? io.str("name") : io.str("key");
    const std::string key = normalize_named_session_key(raw_key);
    if (key.empty()) {
        io.error("ipcSession", "ipcSession requires name", 2);
        return BlockResult::Error;
    }

    if (!io.scope()) {
        io.error("ipcSession", "ipcSession requires run scope", 2).set("key", key);
        return BlockResult::Error;
    }

    const std::string cache_key = "ipcSession:" + key;
    const std::string label = io.str("label", key);
    const std::string cwd = io.runtime().options.default_cwd.empty()
        ? std::string(".")
        : media::path::path_to_utf8(io.runtime().options.default_cwd);
    auto& hosted = io.scope()->acquire<HostedIpcSession>(cache_key, [&] {
        return std::make_shared<HostedIpcSession>(key, label, cwd);
    });
    if (!hosted.ok) {
        const std::string err = hosted.error.empty() ? "failed to host IPC session" : hosted.error;
        io.scope()->release(cache_key);
        io.error("ipcSession", err, 2).set("key", key);
        return BlockResult::Error;
    }

    for (const auto& [name, value] : hosted.server.take_pending_sets())
        io.runtime().set_context_value(io.runtime().user_data, name, value);
    io.runtime().set_context_value(io.runtime().user_data, "SELF_SESSION", key);
    io.runtime().set_context_value(io.runtime().user_data, "SELF_SESSION_KEY", key);
    hosted.server.publish_vars(io.runtime().context);

    io.ok("ipcSession", "hosting " + key)
      .set("key", key)
      .result(Value::from_json(hosted.server.descriptor().to_json()), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult ipc_sessions_block(BlockIO& io)
{
    nlohmann::json sessions = session_descriptors_json();
    const auto count = static_cast<long long>(sessions.size());
    io.ok("ipcSessions", "found " + std::to_string(count) + " session(s)")
      .set("count", count)
      .result(Value::from_json(std::move(sessions)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult ipc_send_block(BlockIO& io)
{
    media::xblox::session::IpcMessage msg;
    msg.from = io.str("from", "xblox");
    msg.message = io.str("message");
    const nlohmann::json data = io.value("data")
        ? resolve_json_templates(io, io.value("data")->to_json())
        : nlohmann::json(nullptr);
    if (!data.is_null()) {
        msg.has_data = true;
        msg.data = data;
    }

    media::xblox::session::SessionDescriptor target;
    media::xblox::session::SendMessagesReply reply;
    std::string err;
    const int timeout_ms = static_cast<int>(std::max<long long>(
        1, io.i64("timeoutMs", xblox_constants::kIpcDefaultTimeoutMs)));
    const std::vector<media::xblox::session::IpcMessage> messages{std::move(msg)};
    if (!send_cached_messages(io, "ipcSend", messages, reply, err, timeout_ms, target)) {
        if (!err.empty())
            io.error("ipcSend", err.empty() ? "ipcSend failed" : err, 1).set("target", target.key);
        return BlockResult::Error;
    }

    nlohmann::json result{{"type", "send_ack"}, {"accepted", reply.accepted}, {"inbox", reply.inbox}};
    io.ok("ipcSend", "message sent")
      .set("target", target.key)
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult ipc_send_batch_block(BlockIO& io)
{
    std::vector<media::xblox::session::IpcMessage> messages;
    const std::string from = io.str("from", "xblox");
    const std::string message = io.str("message");
    const std::string source = io.str("source");
    if (!source.empty()) {
        if (const nlohmann::json* src = io.context(source); src && src->is_array()) {
            messages.reserve(src->size());
            for (const auto& item : *src) {
                media::xblox::session::IpcMessage m;
                m.from = from;
                m.message = message;
                if (!item.is_null()) {
                    m.has_data = true;
                    m.data = item;
                }
                messages.push_back(std::move(m));
            }
        }
    }

    if (messages.empty()) {
        const long long count = std::clamp<long long>(
            io.i64("count", 1), 0, xblox_constants::kIpcMaxGeneratedBatchMessages);
        const nlohmann::json data = io.value("data")
            ? resolve_json_templates(io, io.value("data")->to_json())
            : nlohmann::json(nullptr);
        messages.reserve(static_cast<std::size_t>(count));
        for (long long i = 0; i < count; ++i) {
            media::xblox::session::IpcMessage m;
            m.from = from;
            m.message = message;
            if (!data.is_null()) {
                m.has_data = true;
                m.data = data;
            }
            messages.push_back(std::move(m));
        }
    }

    media::xblox::session::SessionDescriptor target;
    media::xblox::session::SendMessagesReply reply;
    std::string err;
    const int timeout_ms = static_cast<int>(std::max<long long>(
        1, io.i64("timeoutMs", xblox_constants::kIpcDefaultTimeoutMs)));
    if (!send_cached_messages(io, "ipcSendBatch", messages, reply, err, timeout_ms, target)) {
        if (!err.empty())
            io.error("ipcSendBatch", err.empty() ? "ipcSendBatch failed" : err, 1).set("target", target.key);
        return BlockResult::Error;
    }

    const long long accepted = reply.accepted;
    nlohmann::json result{{"type", "send_batch_ack"}, {"accepted", reply.accepted}, {"inbox", reply.inbox}};
    io.ok("ipcSendBatch", "sent " + std::to_string(accepted) + " message(s)")
      .set("target", target.key)
      .set("accepted", accepted)
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult ipc_broadcast_block(BlockIO& io)
{
    const nlohmann::json data = io.value("data")
        ? resolve_json_templates(io, io.value("data")->to_json())
        : nlohmann::json(nullptr);
    nlohmann::json replies = nlohmann::json::array();
    long long delivered = 0;
    long long failed = 0;
    const int timeout_ms = static_cast<int>(std::max<long long>(
        1, io.i64("timeoutMs", xblox_constants::kIpcDefaultTimeoutMs)));

    for (const auto& d : media::xblox::session::list_sessions()) {
        nlohmann::json extra{
            {"from", io.str("from", "xblox")},
            {"message", io.str("message")},
        };
        if (!data.is_null())
            extra["data"] = data;
        nlohmann::json reply;
        std::string err;
        if (media::xblox::session::send_action(d, "send", extra, reply, err, timeout_ms)) {
            ++delivered;
            replies.push_back({{"target", d.key}, {"reply", reply}});
        } else {
            ++failed;
            replies.push_back({{"target", d.key}, {"error", err}});
        }
    }

    nlohmann::json result{{"delivered", delivered}, {"failed", failed}, {"replies", replies}};
    io.ok("ipcBroadcast", "broadcast delivered to " + std::to_string(delivered) + " session(s)")
      .set("delivered", delivered)
      .set("failed", failed)
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult ipc_receive_block(BlockIO& io)
{
    const bool peek = io.flag("peek", false);
    const long long wait_ms = std::max<long long>(
        0, io.i64("waitMs", xblox_constants::kIpcDefaultWaitMs));
    const long long poll_ms = std::max<long long>(
        xblox_constants::kIpcPollMinMs, io.i64("pollMs", xblox_constants::kIpcDefaultPollMs));
    const int timeout_ms = static_cast<int>(std::max<long long>(
        1, io.i64("timeoutMs", xblox_constants::kIpcDefaultTimeoutMs)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    std::vector<media::xblox::session::IpcMessage> typed_messages;
    std::string last_err;
    media::xblox::session::SessionDescriptor target;
    for (;;) {
        std::string err;
        if (!receive_cached_messages(io, "ipcReceive", peek, typed_messages, err, timeout_ms, target)) {
            last_err = err;
            break;
        }
        if (!typed_messages.empty() || wait_ms == 0 || io.cancelled())
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        io.wait(std::min<long long>(poll_ms, std::max<long long>(xblox_constants::kIpcPollMinMs, wait_ms)));
    }

    if (!last_err.empty()) {
        io.error("ipcReceive", last_err, 1).set("target", target.key);
        return BlockResult::Error;
    }

    nlohmann::json messages = ipc_messages_to_json(typed_messages);
    const auto count = static_cast<long long>(messages.size());
    io.ok("ipcReceive", "received " + std::to_string(count) + " message(s)")
      .set("target", target.key)
      .set("count", count)
      .result(Value::from_json(std::move(messages)), ParamKind::json_value);
    return BlockResult::Ok;
}

} // namespace

bool network_block_allowed(const std::string& kind)
{
    if (kind == "sshExec")
        return pm::policy::ssh_access_allowed();
    return true;
}

BlockHandler network_block_handler(const std::string& kind)
{
    if (!network_block_allowed(kind))
        return nullptr;
    if (kind == "network" || kind == "httpRequest" || kind == "fetch")
        return network_request_block;
#if defined(FEATURE_SSH_XBLOX) && FEATURE_SSH_XBLOX
    if (kind == "sshExec")
        return ssh_exec_block;
#endif
#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    if (kind == "ipcSession")
        return ipc_session_block;
    if (kind == "ipcSessions")
        return ipc_sessions_block;
    if (kind == "ipcSend")
        return ipc_send_block;
    if (kind == "ipcSendBatch")
        return ipc_send_batch_block;
    if (kind == "ipcBroadcast")
        return ipc_broadcast_block;
    if (kind == "ipcReceive")
        return ipc_receive_block;
#endif
    return nullptr;
}

void register_network_blocks(BlockRegistry& registry)
{
    // ── Shared param list ─────────────────────────────────────────────────────
    const std::vector<ParamDef> net_params = {
        // ── request ──────────────────────────────────────────────────────────
        pd_str("url").in().req().dflt("https://")
            .desc("Request URL. Supports ${var} interpolation."),
        pd_str("method").grp("request").dflt("GET")
            .ui({{"control","editable_select"}})
            .enums({"GET","POST","PUT","PATCH","DELETE","HEAD","OPTIONS"})
            .desc("HTTP method  -  GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS, or any custom verb."),
        pd_json("queryParams").grp("request").dflt(nullptr)
            .desc("Query-string parameters as a JSON object {\"key\":\"value\"}. "
                  "Appended to the URL (percent-encoded). Merged with any params already in the URL."),
        pd_str("headers").grp("request").dflt("")
            .desc("Extra request headers  -  JSON object {\"Name\":\"value\"} or array of \"Name: value\" strings."),

        // ── body ─────────────────────────────────────────────────────────────
        pd_enum("bodyType", {"none","raw","json","form","multipart"}).grp("body").dflt("none")
            .desc("Request body format. "
                  "none = no body (GET / HEAD). "
                  "raw = plain text body. "
                  "json = serialize bodyJson + Content-Type: application/json. "
                  "form = URL-encode bodyFields + Content-Type: application/x-www-form-urlencoded. "
                  "multipart = curl_mime from bodyParts (\"name=value\" or \"name@/path\")."),
        pd_prompt("body").grp("body").dflt("")
            .desc("Raw request body (used when bodyType is raw). Supports ${var} interpolation."),
        pd_json("bodyJson").grp("body").dflt(nullptr)
            .desc("Body as a JSON value (object, array, string, ...). Serialized and sent with "
                  "Content-Type: application/json. Used when bodyType is json."),
        pd_json("bodyFields").grp("body").dflt(nullptr)
            .desc("Form fields as a JSON object {\"key\": \"value\"}. URL-encoded and sent with "
                  "Content-Type: application/x-www-form-urlencoded. Used when bodyType is form."),
        pd_args("bodyParts").grp("body").ui({{"picks_path", true}, {"item_kind", "file_path"}}).dflt(nullptr)
            .desc("Multipart form parts (bodyType: multipart). Each entry uses curl -F syntax: "
                  "\"name=value\" for a text field, \"name@/path/to/file\" for a file part, "
                  "or bare \"/path/to/file\" to use the filename as field name."),

        // ── response ─────────────────────────────────────────────────────────
        pd_enum("decode", {"raw","json"}).grp("response").dflt("raw")
            .desc("raw = body as a string. json = parse body as JSON and apply the parse selector."),
        pd_str("parse").grp("response").dflt(".")
            .desc("Dotted-path selector applied after decode:json (e.g. .items[0].name). Default . = whole document."),

        // ── auth ─────────────────────────────────────────────────────────────
        pd_enum("auth", {"none","bearer","basic","apikey"}).grp("auth").dflt("none")
            .desc("Authentication mode. none = no auth header added."),
        pd_str("authToken").grp("auth").dflt("")
            .desc("Bearer token or API key value."),
        pd_str("authUser").grp("auth").dflt("")
            .desc("HTTP Basic auth username."),
        pd_str("authPass").grp("auth").dflt("")
            .desc("HTTP Basic auth password."),
        pd_str("authHeader").grp("auth").dflt("X-Api-Key")
            .desc("Header name used for apikey auth. Default: X-Api-Key."),

        // ── download ─────────────────────────────────────────────────────────
        ParamDef{"downloadAs", {}, {}, ParamKind::output_path}.grp("download").dflt("")
            .desc("Save the response body to this file path; the saved path is available as the savedPath output. "
                  "decode/parse/storeAs still apply normally — result (PREVIOUS) is the decoded value, not the path. "
                  "\"auto\" derives the filename from Content-Disposition or the URL and saves to Downloads."),

        // ── cache ─────────────────────────────────────────────────────────────
        pd_bool("cache").grp("cache").dflt(false)
            .desc("Cache responses to disk. Sends ETag / Last-Modified revalidation on stale hits."),
        pd_str("cacheDir").grp("cache").dflt("")
            .desc("Cache directory. Default: system cache dir / pixlwiz / net."),
        pd_ms("cacheTtlMs").grp("cache")
            .dflt(xblox_constants::kNetworkDefaultCacheTtlMs)
            .range(xblox_constants::kNetworkCacheTtlMinMs, xblox_constants::kNetworkCacheTtlMaxMs)
            .desc("Cache TTL in ms. -1 = rely solely on server ETag / Last-Modified. Default: 1 h."),

        // ── proxy ─────────────────────────────────────────────────────────────
        pd_str("proxy").grp("proxy").dflt("")
            .desc("Proxy URL, e.g. http://proxy.corp:8080."),
        pd_str("proxyUser").grp("proxy").dflt("")
            .desc("Proxy username."),
        pd_str("proxyPass").grp("proxy").dflt("")
            .desc("Proxy password."),

        // ── session ───────────────────────────────────────────────────────────
        pd_str("sessionId").grp("session").dflt("")
            .desc("Named persistent cookie jar. All requests sharing the same sessionId reuse "
                  "cookies set by previous responses (login flows, OAuth redirects, CSRF tokens). "
                  "Concurrent requests to the same session are serialized. Empty = no session."),
        pd_dir("sessionDir").grp("session").dflt("")
            .desc("Directory for session jar files. Default: system cache dir / pixlwiz / sessions."),

        // ── ssl ───────────────────────────────────────────────────────────────
        pd_bool("insecure").grp("ssl").dflt(false)
            .desc("Skip SSL peer and host verification. Implies verifyPeer=false + verifyHost=false."),
        pd_bool("verifyPeer").adv().grp("ssl").dflt(true)
            .desc("Verify SSL certificate chain. Overridden to false by insecure:true."),
        pd_bool("verifyHost").adv().grp("ssl").dflt(true)
            .desc("Verify SSL hostname. Overridden to false by insecure:true."),

        // ── timeouts ─────────────────────────────────────────────────────────
        pd_ms("timeoutMs").grp("timeouts")
            .dflt(xblox_constants::kNetworkDefaultTimeoutMs)
            .range(0, xblox_constants::kNetworkTimeoutMaxMs)
            .desc("Total transfer timeout in ms (0 = no limit)."),
        pd_int("connectTimeoutMs").grp("timeouts")
            .dflt(xblox_constants::kNetworkDefaultConnectTimeoutMs)
            .range(0, xblox_constants::kNetworkTimeoutMaxMs)
            .desc("TCP connect timeout in ms."),

        // ── redirects ────────────────────────────────────────────────────────
        pd_bool("followRedirects").grp("redirects").dflt(true)
            .desc("Follow 3xx redirects automatically."),
        pd_int("maxRedirects").grp("redirects")
            .dflt(xblox_constants::kNetworkDefaultMaxRedirects)
            .range(0, xblox_constants::kNetworkMaxRedirects)
            .desc("Maximum number of redirects to follow."),

        // ── retry ─────────────────────────────────────────────────────────────
        pd_int("retries").grp("retry")
            .dflt(xblox_constants::kNetworkDefaultRetries)
            .range(0, xblox_constants::kNetworkMaxRetries)
            .desc("Maximum number of retries after a transient failure (curl error or 5xx / 408 / 429)."),
        pd_int("retryDelayMs").grp("retry")
            .dflt(xblox_constants::kNetworkDefaultRetryDelayMs)
            .range(0, xblox_constants::kNetworkRetryDelayMaxMs)
            .desc("Base retry delay in ms. Each retry doubles the delay plus random jitter (exponential back-off)."),

        // ── outputs ───────────────────────────────────────────────────────────
        pd_int("statusCode").out()
            .desc("HTTP status code of the final response. 0 for file:// and transport errors."),
        pd_store_as().dflt("response"),
    };

    // ── network (hidden alias, backward-compat) ───────────────────────────────
    registry["network"] = bdb(
        "network", network_request_block, "Network Request", "Network",
        "HTTP request via libcurl.",
        {{"kind","network"},{"url","https://"},{"method","GET"},{"decode","raw"},
         {"timeoutMs", xblox_constants::kNetworkDefaultTimeoutMs},{"followRedirects",true},{"storeAs","response"}})
        .params(net_params)
        .exec({false, true, true})
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::string_, ParamKind::json_value},
            "Response")
        .output("statusCode", ParamKind::integer_, "Status code")
        .hidden();

    // ── httpRequest (hidden alias) ────────────────────────────────────────────
    registry["httpRequest"] = bdb(
        "httpRequest", network_request_block, "HTTP Request", "Network",
        "HTTP request via libcurl.",
        {{"kind","httpRequest"},{"url","https://"},{"method","GET"},{"decode","raw"},
         {"timeoutMs", xblox_constants::kNetworkDefaultTimeoutMs},{"followRedirects",true},{"storeAs","response"}})
        .params(net_params)
        .exec({false, true, true})
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::string_, ParamKind::json_value},
            "Response")
        .output("statusCode", ParamKind::integer_, "Status code")
        .hidden();

    // ── fetch (visible; iterable producer) ───────────────────────────────────
    //
    // When decode:json and the response is a JSON array, the executor fans its
    // `items` children over each element (PREVIOUS = element, index = i).
    // No special code in the handler is needed  -  the iterable flag + ExecFlags
    // machinery in run_child_blocks() does the fanning automatically.
    registry["fetch"] = bdb(
        "fetch", network_request_block, "Fetch", "Network",
        "Fetch a URL. With decode:json + a JSON-array response, child blocks "
        "iterate over each element (PREVIOUS = element, index = i).",
        {{"kind","fetch"},{"url","https://"},{"method","GET"},{"decode","raw"},
         {"timeoutMs", xblox_constants::kNetworkDefaultTimeoutMs},{"followRedirects",true},{"storeAs","response"}})
        .params(net_params)
        .exec({false, true, true})
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::string_, ParamKind::json_value},
            "Response")
        .output("statusCode", ParamKind::integer_, "Status code")
        .iterable()
        .children({{"items", "Body", "body", {"*"}, 0, -1}});

#if defined(FEATURE_SSH_XBLOX) && FEATURE_SSH_XBLOX
    registry["sshExec"] = bdb(
        "sshExec", ssh_exec_block, "SSH Exec", "Network",
        "Run a command on a remote host over SSH using password or public-key authentication.",
        {{"kind","sshExec"},{"host",""},{"port",22},{"username",""},{"command",""},{"timeoutMs",30000},{"storeAs","ssh"}})
        .params({
            pd_str("host").in().req().desc("SSH host name or IP address."),
            pd_int("port").in().dflt(22).range(1, 65535).desc("SSH port."),
            pd_str("username").in().dflt("").desc("SSH username. Empty lets libssh use the current user."),
            pd_str("password").in().dflt("").control("password").desc("Password authentication value. Empty tries public-key auth."),
            pd_file("privateKey").in().dflt("").desc("Optional private key file for public-key auth."),
            pd_str("passphrase").in().dflt("").control("password").desc("Optional private key passphrase."),
            pd_prompt("command").in().req().desc("Remote command to execute. Supports ${var} interpolation."),
            pd_file("configFile").grp("ssh").dflt("").desc("Optional SSH config file. Empty parses the OS user default ~/.ssh/config."),
            pd_file("knownHosts").grp("ssh").dflt("").desc("Optional known_hosts file. Empty uses libssh defaults."),
            pd_bool("verifyHost").grp("ssh").dflt(true).desc("Verify the server host key against known_hosts."),
            pd_bool("acceptUnknownHost").grp("ssh").dflt(false).desc("Add an unknown host key to known_hosts when verifyHost is enabled."),
            pd_ms("timeoutMs").grp("timeouts").dflt(30000).range(1, xblox_constants::kNetworkTimeoutMaxMs).desc("Connect and command timeout in ms."),
            pd_int("exitStatus").out().desc("Remote command exit status."),
            pd_store_as().dflt("ssh"),
        })
        .primary_output(ParamKind::json_value, "SSH result")
        .output("exitStatus", ParamKind::integer_, "Exit status")
        .exec({false, false, true});
#endif

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    registry["ipcSessions"] = bdb(
        "ipcSessions", ipc_sessions_block, "IPC Sessions", "Network",
        "List live XBlox sessions. Child blocks iterate over each session descriptor.",
        {{"kind","ipcSessions"},{"storeAs","sessions"},{"items", nlohmann::json::array()}})
        .params({
            pd_store_as().dflt("sessions"),
        })
        .pure()
        .primary_output(ParamKind::json_value, "Sessions")
        .iterable()
        .children({{"items", "For each", "body", {"*"}, 0, -1}});

    registry["ipcSession"] = bdb(
        "ipcSession", ipc_session_block, "IPC Session", "Network",
        "Host a named XBlox IPC session for this run. Reuses the same session on later loop passes.",
        {{"kind","ipcSession"},{"name","ipc"},{"storeAs","session"}})
        .params({
            pd_str("name").in().req().desc("Session name. Plain names are normalized to name:<value>."),
            pd_str("label").in().dflt("").desc("Optional display label for the session."),
            pd_store_as().dflt("session"),
        })
        .primary_output(ParamKind::json_value, "Session descriptor");

    registry["ipcSend"] = bdb(
        "ipcSend", ipc_send_block, "IPC Send", "Network",
        "Send a message to a live XBlox session.",
        {{"kind","ipcSend"},{"target",""},{"from","xblox"},{"message",""},{"data", nullptr},{"storeAs","ipcReply"}})
        .params({
            pd_str("target").in().req().desc("Target session key, descriptor stem, or unambiguous key prefix."),
            pd_str("from").in().dflt("xblox").desc("Sender identity stored with the message."),
            pd_str("message").in().dflt("").control("variable_text").desc("Message text. Supports ${var} interpolation."),
            pd_json("data").in().dflt(nullptr).desc("Optional structured payload attached as message.data."),
            pd_ms("timeoutMs").grp("timeouts").dflt(xblox_constants::kIpcDefaultTimeoutMs).range(1, xblox_constants::kIpcTimeoutMaxMs).desc("IPC request timeout."),
            pd_store_as().dflt("ipcReply"),
        })
        .primary_output(ParamKind::json_value, "Send acknowledgement");

    registry["ipcSendBatch"] = bdb(
        "ipcSendBatch", ipc_send_batch_block, "IPC Send Batch", "Network",
        "Send many messages to a live XBlox session in one IPC frame.",
        {{"kind","ipcSendBatch"},{"target",""},{"from","xblox"},{"message",""},{"count",1},{"source",""},{"data", nullptr},{"storeAs","ipcBatchReply"}})
        .params({
            pd_str("target").in().req().desc("Target session key, descriptor stem, or unambiguous key prefix."),
            pd_str("from").in().dflt("xblox").desc("Sender identity stored with each message."),
            pd_str("message").in().dflt("").control("variable_text").desc("Message text. Supports ${var} interpolation."),
            pd_int("count").in().dflt(1).range(0, xblox_constants::kIpcMaxGeneratedBatchMessages).desc("Number of messages to create when source is empty."),
            pd_str("source").in().dflt("").desc("Optional context array to send one message per element."),
            pd_json("data").in().dflt(nullptr).desc("Optional shared structured payload when source is empty."),
            pd_ms("timeoutMs").grp("timeouts").dflt(xblox_constants::kIpcDefaultTimeoutMs).range(1, xblox_constants::kIpcTimeoutMaxMs).desc("IPC request timeout."),
            pd_store_as().dflt("ipcBatchReply"),
        })
        .primary_output(ParamKind::json_value, "Batch acknowledgement");

    registry["ipcBroadcast"] = bdb(
        "ipcBroadcast", ipc_broadcast_block, "IPC Broadcast", "Network",
        "Send a message to every live XBlox session.",
        {{"kind","ipcBroadcast"},{"from","xblox"},{"message",""},{"data", nullptr},{"storeAs","ipcBroadcast"}})
        .params({
            pd_str("from").in().dflt("xblox").desc("Sender identity stored with the message."),
            pd_str("message").in().dflt("").control("variable_text").desc("Message text. Supports ${var} interpolation."),
            pd_json("data").in().dflt(nullptr).desc("Optional structured payload attached as message.data."),
            pd_ms("timeoutMs").grp("timeouts").dflt(xblox_constants::kIpcDefaultTimeoutMs).range(1, xblox_constants::kIpcTimeoutMaxMs).desc("Per-session IPC request timeout."),
            pd_store_as().dflt("ipcBroadcast"),
        })
        .primary_output(ParamKind::json_value, "Broadcast summary");

    registry["ipcReceive"] = bdb(
        "ipcReceive", ipc_receive_block, "IPC Receive", "Network",
        "Drain or wait for messages from a live XBlox session inbox. Child blocks iterate over received messages.",
        {{"kind","ipcReceive"},{"target",""},{"peek",false},
         {"waitMs", xblox_constants::kIpcDefaultWaitMs},
         {"pollMs", xblox_constants::kIpcDefaultPollMs},
         {"storeAs","messages"},{"items", nlohmann::json::array()}})
        .params({
            pd_str("target").in().req().desc("Target session key, descriptor stem, or unambiguous key prefix."),
            pd_bool("peek").in().dflt(false).desc("Read without consuming messages."),
            pd_ms("waitMs").grp("wait")
                .dflt(xblox_constants::kIpcDefaultWaitMs)
                .range(0, xblox_constants::kIpcWaitMaxMs)
                .desc("Poll until messages arrive or this timeout elapses."),
            pd_ms("pollMs").grp("wait")
                .dflt(xblox_constants::kIpcDefaultPollMs)
                .range(xblox_constants::kIpcPollMinMs, xblox_constants::kIpcPollMaxMs)
                .desc("Polling interval while waiting."),
            pd_ms("timeoutMs").grp("timeouts").dflt(xblox_constants::kIpcDefaultTimeoutMs).range(1, xblox_constants::kIpcTimeoutMaxMs).desc("Per-IPC request timeout."),
            pd_store_as().dflt("messages"),
        })
        .exec({false, true, true})
        .primary_output(ParamKind::json_value, "Messages")
        .iterable()
        .children({{"items", "For each", "body", {"*"}, 0, -1}});
#endif
#if FEATURE_MCP_CLIENT
    register_network_mcp_blocks(registry);
#endif
}

} // namespace media::xblox::blocks
