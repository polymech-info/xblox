// vector_blocks.cpp  -  LiteLLM vector store blocks for XBlox.
//
// Blocks: vectorStoreList · vectorStoreCreate
//         vectorStoreSearch · vectorStoreAddText · vectorStoreAddFile
//
// Auth:
//   Identical to service blocks  -  Zitadel SSO bearer token via pm_zitadel_oauth.hpp.
//   The LiteLLM proxy (llm.polymech.info) accepts Zitadel JWTs via custom_auth.
//   auth:"auto" (default)  -  reads token from zitadel-oauth.json (same as `tanit-cli login`)
//   auth:"bearer"          -  explicit bearerToken param
//   auth:"none"            -  no auth header (open endpoints only)
//
// Server URL:
//   llmBaseUrl param -> pm::services::resolve_llm_router_base()
//
// Ingestion (vectorStoreAddText / vectorStoreAddFile)  -  two HTTP calls:
//   1. POST {llmBaseUrl}/v1/embeddings  {model, input:text}  -> embedding vector (1536 dims)
//   2. POST {llmBaseUrl}/pgvector/v1/vector_stores/{id}/embeddings  {content,embedding,metadata}
//      ↑ routed through the /pgvector pass-through to the litellm-pgvector connector
//
// Feature guard: FEATURE_VECTOR_XBLOX=1

#include "blocks/vector_blocks.hpp"
#include "blocks/block_registry.hpp"
#include "blocks/block_params.hpp"
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_VECTOR_XBLOX) && FEATURE_VECTOR_XBLOX

#include "service_defaults.hpp"
#include "lib/net/http_client.hpp"
#include "lib/pm_zitadel_oauth.hpp"    // pm_zitadel_oauth_read_access_token
#include "fs/fs_guard.hpp"        // llm_fs_guard_deny_reason
#include "fs/glob_filter.hpp"     // glob_file_extension_blocked_like_file_read

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

namespace fs = std::filesystem;
using J      = nlohmann::json;

// ── Auth / URL resolution ──────────────────────────────────────────────────

struct VectorContext {
    std::string base_url;   // trimmed, no trailing slash
    std::string bearer;     // may be empty for auth:none
    bool        ok  = false;
    std::string err;
};

VectorContext resolve_vector_context(BlockIO& io)
{
    VectorContext ctx;

    const std::string explicit_url = io.str("llmBaseUrl");
    ctx.base_url = pm::services::resolve_llm_router_base(explicit_url);
    if (ctx.base_url.empty()) {
        ctx.err = "llmBaseUrl is empty  -  set llmBaseUrl param or configure service defaults";
        return ctx;
    }

    const std::string auth_mode = io.str("auth", "auto");
    if (auth_mode == "none") {
        ctx.ok = true;
        return ctx;
    }
    if (auth_mode == "bearer") {
        ctx.bearer = io.str("bearerToken");
        if (ctx.bearer.empty()) {
            ctx.err = "auth:bearer requires bearerToken param";
            return ctx;
        }
        ctx.ok = true;
        return ctx;
    }

    // auth:auto  -  explicit bearerToken wins, else read from zitadel-oauth.json
    ctx.bearer = io.str("bearerToken");
    if (ctx.bearer.empty()) {
        std::string aerr;
        if (!pm_zitadel_oauth_read_access_token(ctx.bearer, aerr) || ctx.bearer.empty()) {
            ctx.err = std::string("no bearer token  -  run `tanit-cli login` (auth: ")
                + pm::services::resolve_zitadel_authority() + ") or set bearerToken param. detail: " + aerr;
            return ctx;
        }
    }
    ctx.ok = true;
    return ctx;
}

void add_bearer_header(media::net::HttpRequest& req, const std::string& bearer)
{
    if (bearer.empty()) return;
    req.auth.kind  = media::net::AuthKind::Bearer;
    req.auth.token = bearer;
}

long default_timeout_ms(BlockIO& io)
{
    return std::max<long>(1, static_cast<long>(io.i64("timeoutMs", 30000)));
}

// ── Auth param helpers ─────────────────────────────────────────────────────

std::vector<ParamDef> vector_auth_params()
{
    return {
        pd_str("llmBaseUrl").grp("auth").deep().dflt("").desc(
            "LiteLLM proxy URL. Defaults to https://llm.polymech.info. Supports ${var} interpolation."),
        pd_enum("auth", {"auto", "bearer", "none"}).grp("auth").dflt("auto").desc(
            "Auth mode: auto = Zitadel SSO (zitadel-oauth.json), bearer = explicit token, none = no auth."),
        pd_str("bearerToken").grp("auth").deep().dflt("").desc(
            "Explicit Zitadel bearer token (auth:bearer or auth:auto override). Supports ${var} interpolation."),
    };
}

ParamDef vs_timeout_param() { return pd_int("timeoutMs").grp("advanced").dflt(30000).range(1000, 300000).desc("Request timeout in ms."); }

// ── HTTP helper: POST JSON, parse response ─────────────────────────────────

struct HttpJsonResult {
    bool          ok = false;
    int           status_code = 0;
    std::string   url;
    nlohmann::json body;
    std::string   err;
};

HttpJsonResult post_json(BlockIO& io, const VectorContext& ctx,
                         const std::string& path, const nlohmann::json& payload)
{
    HttpJsonResult r;
    r.url = ctx.base_url + path;

    media::net::HttpRequest req;
    req.method      = "POST";
    req.url         = r.url;
    req.body        = payload.dump();
    req.content_type = "application/json";
    add_bearer_header(req, ctx.bearer);
    req.timeout_ms       = default_timeout_ms(io);
    req.follow_redirects = true;

    auto& rt = io.runtime();
    req.cancel_check = [&rt]() { return rt.cancel_requested && rt.cancel_requested(rt.user_data); };

    const auto resp = media::net::http_fetch(req);
    r.status_code   = resp.status_code;

    if (!resp.ok) {
        r.err = resp.error.empty() ? "request failed" : resp.error;
        return r;
    }

    try { r.body = nlohmann::json::parse(resp.body); }
    catch (...) { r.body = resp.body; }
    r.ok = true;
    return r;
}

HttpJsonResult get_json(BlockIO& io, const VectorContext& ctx, const std::string& path)
{
    HttpJsonResult r;
    r.url = ctx.base_url + path;

    media::net::HttpRequest req;
    req.method = "GET";
    req.url    = r.url;
    add_bearer_header(req, ctx.bearer);
    req.timeout_ms       = default_timeout_ms(io);
    req.follow_redirects = true;

    auto& rt = io.runtime();
    req.cancel_check = [&rt]() { return rt.cancel_requested && rt.cancel_requested(rt.user_data); };

    const auto resp = media::net::http_fetch(req);
    r.status_code   = resp.status_code;

    if (!resp.ok) {
        r.err = resp.error.empty() ? "request failed" : resp.error;
        return r;
    }

    try { r.body = nlohmann::json::parse(resp.body); }
    catch (...) { r.body = resp.body; }
    r.ok = true;
    return r;
}

// Emit a standard HTTP error event.
BlockResult emit_http_error(BlockIO& io, const char* K, const HttpJsonResult& r)
{
    io.error(K, r.err.empty() ? "request failed" : r.err, 1)
      .set("url",        r.url)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.status_code)});
    return BlockResult::Error;
}

// ── vectorStoreList ───────────────────────────────────────────────────────────
//
// GET /vector_store/list
// Returns the array (or object) of registered vector stores from the LiteLLM
// management API.

BlockResult vector_store_list_block(BlockIO& io)
{
    constexpr const char* K = "vectorStoreList";

    VectorContext ctx = resolve_vector_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const auto r = get_json(io, ctx, "/vector_store/list");
    if (!r.ok) return emit_http_error(io, K, r);

    // LiteLLM returns { object:"list", data:[...], total_count:N }
    const long long count = r.body.is_array() ? static_cast<long long>(r.body.size())
                          : r.body.is_object() && r.body.contains("data") && r.body["data"].is_array()
                            ? static_cast<long long>(r.body["data"].size())
                          : r.body.is_object() && r.body.contains("total_count")
                            ? r.body["total_count"].get<long long>() : 0LL;

    io.ok(K, std::to_string(count) + " store(s)")
      .set("url",        r.url)
      .set("count",      ParamKind::integer_, Value{count})
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.status_code)})
      .result(Value::from_json(r.body), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorStoreCreate ─────────────────────────────────────────────────────────
//
// POST /vector_store/new
// Registers a new pg_vector store in the LiteLLM management API.

BlockResult vector_store_create_block(BlockIO& io)
{
    constexpr const char* K = "vectorStoreCreate";

    VectorContext ctx = resolve_vector_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string name = io.str("name");
    if (name.empty()) { io.error(K, "name param is required", 2); return BlockResult::Error; }

    J body;
    body["vector_store_name"]        = name;
    body["custom_llm_provider"]      = "pg_vector";
    body["vector_store_description"] = io.str("description");
    body["vector_store_metadata"]    = J::object({{"provider","pg_vector"},{"connector","litellm-pgvector"}});
    body["litellm_params"]           = J::object({
        {"custom_llm_provider", "pg_vector"},
        {"api_base",            "os.environ/PG_VECTOR_API_BASE"},
        {"api_key",             "os.environ/PG_VECTOR_API_KEY"},
    });

    const std::string store_id = io.str("storeId");
    if (!store_id.empty()) body["vector_store_id"] = store_id;

    const auto r = post_json(io, ctx, "/vector_store/new", body);
    if (!r.ok) return emit_http_error(io, K, r);

    const std::string created_id = r.body.is_object()
        ? r.body.value("vector_store_id", r.body.value("id", std::string{}))
        : std::string{};

    io.ok(K, created_id.empty() ? "created" : "created " + created_id)
      .set("url",        r.url)
      .set("storeId",    created_id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.status_code)})
      .result(Value::from_json(r.body), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorStoreSearch ─────────────────────────────────────────────────────────
//
// POST /v1/vector_stores/{storeId}/search
// Semantic search via the LiteLLM proxy (OpenAI-compatible endpoint).
// Returns {data:[{id,score,content:[{type:"text",text:"..."}]},...], object:"list"}.

BlockResult vector_store_search_block(BlockIO& io)
{
    constexpr const char* K = "vectorStoreSearch";

    VectorContext ctx = resolve_vector_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string store_id = io.str("storeId");
    if (store_id.empty()) { io.error(K, "storeId param is required", 2); return BlockResult::Error; }

    const std::string query = io.str("query");
    if (query.empty()) { io.error(K, "query param is required", 2); return BlockResult::Error; }

    const long long max_results = io.i64("maxResults", 10);

    J body;
    body["query"]       = query;
    body["max_results"] = max_results;

    const std::string path = "/v1/vector_stores/" + store_id + "/search";
    const auto r = post_json(io, ctx, path, body);
    if (!r.ok) return emit_http_error(io, K, r);

    const nlohmann::json& data = r.body.is_object() && r.body.contains("data") ? r.body["data"] : r.body;
    const long long count = data.is_array() ? static_cast<long long>(data.size()) : 0LL;

    io.ok(K, std::to_string(count) + " hit(s) for: " + query)
      .set("url",        r.url)
      .set("count",      ParamKind::integer_, Value{count})
      .set("query",      query)
      .set("storeId",    store_id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.status_code)})
      .result(Value::from_json(r.body), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── embed helper ───────────────────────────────────────────────────────────────

// POST /v1/embeddings -> extract data[0].embedding as J array.
// Returns true on success; sets out_vec and out_dims on success, err on failure.
bool embed_text(BlockIO& io, const VectorContext& ctx,
                const std::string& text, const std::string& model,
                nlohmann::json& out_vec, std::string& err)
{
    J body;
    body["model"] = model;
    body["input"] = text;

    const auto r = post_json(io, ctx, "/v1/embeddings", body);
    if (!r.ok) { err = "embed failed: " + r.err; return false; }

    if (!r.body.is_object()
        || !r.body.contains("data")
        || !r.body["data"].is_array()
        || r.body["data"].empty()
        || !r.body["data"][0].contains("embedding")
        || !r.body["data"][0]["embedding"].is_array())
    {
        err = "embed: unexpected response shape from /v1/embeddings";
        return false;
    }

    out_vec = r.body["data"][0]["embedding"];
    return true;
}

// ── vectorStoreAddText ────────────────────────────────────────────────────────
//
// Two-step ingestion:
//   1. POST /v1/embeddings {model, input:text} -> embedding vector
//   2. POST /pgvector/v1/vector_stores/{storeId}/embeddings {content,embedding,metadata}
//      ↑ the /pgvector prefix routes through the pass-through to the connector
//
// Returns the connector insert response (typically {id:<uuid>,...}).

BlockResult vector_store_add_text_block(BlockIO& io)
{
    constexpr const char* K = "vectorStoreAddText";

    VectorContext ctx = resolve_vector_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string store_id = io.str("storeId");
    if (store_id.empty()) { io.error(K, "storeId param is required", 2); return BlockResult::Error; }

    const std::string text = io.str("text");
    if (text.empty()) { io.error(K, "text param is required", 2); return BlockResult::Error; }

    const std::string model = io.str("model", "openai/text-embedding-3-small");

    // Parse metadata JSON if supplied
    J metadata = J::object();
    const std::string meta_str = io.str("metadata");
    if (!meta_str.empty()) {
        try { metadata = J::parse(meta_str); }
        catch (...) { /* ignore; store as-is */ metadata = J::object({{"raw", meta_str}}); }
    }

    // Step 1  -  embed
    nlohmann::json vec;
    std::string    embed_err;
    if (!embed_text(io, ctx, text, model, vec, embed_err)) {
        io.error(K, embed_err, 1).set("storeId", store_id);
        return BlockResult::Error;
    }

    // Step 2  -  insert via /pgvector pass-through
    J insert_body;
    insert_body["content"]   = text;
    insert_body["embedding"] = vec;
    insert_body["metadata"]  = metadata;

    const std::string insert_path = "/pgvector/v1/vector_stores/" + store_id + "/embeddings";
    const auto r = post_json(io, ctx, insert_path, insert_body);
    if (!r.ok) return emit_http_error(io, K, r);

    const std::string doc_id = r.body.is_object()
        ? r.body.value("id", r.body.value("vector_store_record_id", std::string{}))
        : std::string{};

    const long long dims = static_cast<long long>(vec.size());

    io.ok(K, doc_id.empty() ? "inserted" : "inserted " + doc_id)
      .set("url",        r.url)
      .set("docId",      doc_id)
      .set("dims",       ParamKind::integer_, Value{dims})
      .set("storeId",    store_id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.status_code)})
      .result(Value::from_json(r.body), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── vectorStoreAddFile ────────────────────────────────────────────────────────
//
// Read a local text file -> embed -> insert into the vector store.
// Uses the same LLM fs-guard as llm_blocks (blocks binary, refuses NUL, 1 MiB cap).

constexpr std::uintmax_t k_vs_max_file_bytes = 1024ULL * 1024;

BlockResult vector_store_add_file_block(BlockIO& io)
{
    constexpr const char* K = "vectorStoreAddFile";

    VectorContext ctx = resolve_vector_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string store_id = io.str("storeId");
    if (store_id.empty()) { io.error(K, "storeId param is required", 2); return BlockResult::Error; }

    const std::string raw_path = io.str("file");
    if (raw_path.empty()) { io.error(K, "file param is required", 2); return BlockResult::Error; }

    // Resolve + guard file path
    std::error_code ec;
    fs::path abs = fs::absolute(media::path::path_from_utf8(raw_path), ec);
    if (ec) { io.error(K, "could not resolve path: " + raw_path + ": " + ec.message(), 2); return BlockResult::Error; }
    {
        fs::path can = fs::weakly_canonical(abs, ec);
        if (!ec && !can.empty()) abs = std::move(can);
    }

    if (!fs::is_regular_file(abs, ec)) {
        io.error(K, "path is not a regular file: " + media::path::path_to_utf8(abs), 2);
        return BlockResult::Error;
    }

    const std::string deny = media::llm::llm_fs_guard_deny_reason(abs);
    if (!deny.empty()) { io.error(K, deny, 2); return BlockResult::Error; }

    {
        std::string ext = media::path::path_to_utf8(abs.extension());
        ext = media::strutil::lower_ascii(std::move(ext));
        if (media::llm::glob_file_extension_blocked_like_file_read(ext)) {
            io.error(K, "refusing binary or image extension: " + ext, 2);
            return BlockResult::Error;
        }
    }

    const std::uintmax_t sz = fs::file_size(abs, ec);
    if (ec) { io.error(K, "could not read file size: " + ec.message(), 2); return BlockResult::Error; }
    if (sz > k_vs_max_file_bytes) {
        io.error(K, "file exceeds 1 MiB embed limit: " + media::path::path_to_utf8(abs), 2);
        return BlockResult::Error;
    }

    std::string text;
    {
        std::ifstream in(abs, std::ios::binary);
        if (!in) { io.error(K, "could not open file for reading: " + media::path::path_to_utf8(abs), 2); return BlockResult::Error; }
        text.resize(static_cast<std::size_t>(sz));
        if (sz > 0) {
            in.read(text.data(), static_cast<std::streamsize>(sz));
            if (!in || static_cast<std::uintmax_t>(in.gcount()) != sz) {
                io.error(K, "could not read full file: " + media::path::path_to_utf8(abs), 2);
                return BlockResult::Error;
            }
        }
        if (text.find('\0') != std::string::npos) {
            io.error(K, "refusing NUL-containing binary content: " + media::path::path_to_utf8(abs), 2);
            return BlockResult::Error;
        }
    }

    const std::string model = io.str("model", "openai/text-embedding-3-small");

    J metadata = J::object({{"filename", media::path::path_to_utf8(abs.filename())}});
    const std::string meta_str = io.str("metadata");
    if (!meta_str.empty()) {
        try {
            J extra = J::parse(meta_str);
            if (extra.is_object()) {
                for (auto& [k, v] : extra.items())
                    metadata[k] = v;
            }
        } catch (...) { /* ignore */ }
    }

    // Step 1  -  embed
    nlohmann::json vec;
    std::string    embed_err;
    if (!embed_text(io, ctx, text, model, vec, embed_err)) {
        io.error(K, embed_err, 1).set("file", media::path::path_to_utf8(abs)).set("storeId", store_id);
        return BlockResult::Error;
    }

    // Step 2  -  insert
    J insert_body;
    insert_body["content"]   = text;
    insert_body["embedding"] = vec;
    insert_body["metadata"]  = metadata;

    const std::string insert_path = "/pgvector/v1/vector_stores/" + store_id + "/embeddings";
    const auto r = post_json(io, ctx, insert_path, insert_body);
    if (!r.ok) return emit_http_error(io, K, r);

    const std::string doc_id = r.body.is_object()
        ? r.body.value("id", r.body.value("vector_store_record_id", std::string{}))
        : std::string{};

    const long long dims = static_cast<long long>(vec.size());

    io.ok(K, doc_id.empty() ? "inserted " + media::path::path_to_utf8(abs.filename()) : "inserted " + doc_id)
      .set("url",        r.url)
      .set("docId",      doc_id)
      .set("file",       media::path::path_to_utf8(abs))
      .set("dims",       ParamKind::integer_, Value{dims})
      .set("storeId",    store_id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.status_code)})
      .result(Value::from_json(r.body), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── registration ──────────────────────────────────────────────────────────────

} // namespace (anon)

void register_vector_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_VECTOR_XBLOX) && FEATURE_VECTOR_XBLOX

    using bdb = BlockDescriptorBuilder;
    auto auth_params = vector_auth_params;

    // ── vectorStoreList ───────────────────────────────────────────────────────
    registry["vectorStoreList"] = bdb(
        "vectorStoreList", vector_store_list_block,
        "List Vector Stores", "Vector",
        "GET /vector_store/list  -  list all registered vector stores in the LiteLLM proxy. "
        "Uses Zitadel SSO auth (same as tanit-cli login). "
        "PREVIOUS = array or object of store descriptors.",
        J{{"kind","vectorStoreList"},{"auth","auto"},{"storeAs","stores"}})
        .params([&]{
            auto p = auth_params();
            p.push_back(vs_timeout_param());
            p.push_back(pd_store_as().dflt("stores"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Stores")
        .output("count", ParamKind::integer_, "Store count")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    // ── vectorStoreCreate ─────────────────────────────────────────────────────
    registry["vectorStoreCreate"] = bdb(
        "vectorStoreCreate", vector_store_create_block,
        "Create Vector Store", "Vector",
        "POST /vector_store/new  -  register a new pg_vector store in the LiteLLM proxy. "
        "PREVIOUS = created store descriptor with vector_store_id.",
        J{{"kind","vectorStoreCreate"},{"auth","auto"},{"storeAs","store"}})
        .params([&]{
            auto p = auth_params();
            p.insert(p.begin(), {
                pd_str("name").in().req().deep().dflt("").desc("Store name (human-readable identifier)."),
                pd_str("description").in().deep().dflt("").desc("Optional description."),
                pd_str("storeId").in().deep().dflt("").desc("Optional explicit UUID. Auto-generated if empty."),
            });
            p.push_back(vs_timeout_param());
            p.push_back(pd_store_as().dflt("store"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Store")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── vectorStoreSearch ─────────────────────────────────────────────────────
    registry["vectorStoreSearch"] = bdb(
        "vectorStoreSearch", vector_store_search_block,
        "Search Vector Store", "Vector",
        "POST /v1/vector_stores/{storeId}/search  -  semantic search in a vector store. "
        "Returns {data:[{id,score,content:[{type:text,text:...}]},...],object:list}. "
        "PREVIOUS = the full OpenAI-compatible search response.",
        J{{"kind","vectorStoreSearch"},{"auth","auto"},{"maxResults",10},{"storeAs","searchResult"}})
        .params([&]{
            auto p = auth_params();
            p.insert(p.begin(), {
                pd_str("storeId").in().req().deep().dflt("").desc("vector_store_id UUID. Supports ${var} interpolation."),
                pd_str("query").in().req().deep().dflt("").desc("Semantic search query. Supports ${var} interpolation."),
                pd_int("maxResults").grp("filter").dflt(10).range(1, 100).desc("Max results to return."),
            });
            p.push_back(vs_timeout_param());
            p.push_back(pd_store_as().dflt("searchResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Search result")
        .output("count", ParamKind::integer_, "Hit count")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    // ── vectorStoreAddText ────────────────────────────────────────────────────
    registry["vectorStoreAddText"] = bdb(
        "vectorStoreAddText", vector_store_add_text_block,
        "Add Text to Vector Store", "Vector",
        "Two-step: POST /v1/embeddings to get a 1536-dim vector, then "
        "POST /pgvector/v1/vector_stores/{storeId}/embeddings to insert the document. "
        "PREVIOUS = connector insert response {id,...}.",
        J{{"kind","vectorStoreAddText"},{"auth","auto"},{"model","openai/text-embedding-3-small"},{"storeAs","docResult"}})
        .params([&]{
            auto p = auth_params();
            p.insert(p.begin(), {
                pd_str("storeId").in().req().deep().dflt("").desc("vector_store_id UUID. Supports ${var} interpolation."),
                pd_str("text").in().req().deep().dflt("").desc("Text content to embed and insert. Supports ${var} interpolation."),
                pd_str("metadata").in().deep().dflt("").desc("JSON object string with extra metadata, e.g. {\"src\":\"note\"}. Supports ${var} interpolation."),
                pd_str("model").grp("advanced").deep().dflt("openai/text-embedding-3-small").desc("Embedding model. Must match the model used at store creation time."),
            });
            p.push_back(vs_timeout_param());
            p.push_back(pd_store_as().dflt("docResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Document result")
        .output("dims", ParamKind::integer_, "Dimensions")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── vectorStoreAddFile ────────────────────────────────────────────────────
    registry["vectorStoreAddFile"] = bdb(
        "vectorStoreAddFile", vector_store_add_file_block,
        "Add File to Vector Store", "Vector",
        "Read a local text file, embed via POST /v1/embeddings, and insert into a vector store. "
        "Binary files, images, and files > 1 MiB are rejected (same guard as llm embed). "
        "PREVIOUS = connector insert response {id,...}.",
        J{{"kind","vectorStoreAddFile"},{"auth","auto"},{"model","openai/text-embedding-3-small"},{"storeAs","docResult"}})
        .params([&]{
            auto p = auth_params();
            p.insert(p.begin(), {
                pd_str("storeId").in().req().deep().dflt("").desc("vector_store_id UUID. Supports ${var} interpolation."),
                pd_file("file").in().req().deep().must_exist().guard().dflt("").desc("Path to a text file to embed and insert. Supports ${var} interpolation."),
                pd_str("metadata").in().deep().dflt("").desc("Extra JSON metadata object. filename is auto-added. Supports ${var} interpolation."),
                pd_str("model").grp("advanced").deep().dflt("openai/text-embedding-3-small").desc("Embedding model. Must match the model used at store creation time."),
            });
            p.push_back(vs_timeout_param());
            p.push_back(pd_store_as().dflt("docResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Document result")
        .output("dims", ParamKind::integer_, "Dimensions")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

#endif // FEATURE_VECTOR_XBLOX  -  registration guard
}

#endif // FEATURE_VECTOR_XBLOX  -  handler implementations

} // namespace media::xblox::blocks
