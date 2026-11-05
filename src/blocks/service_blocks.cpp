// service_blocks.cpp  -  Tanit service API blocks for XBlox.
//
// Blocks: serviceImagesUpload
//         serviceFilesUpload · serviceFilesList · serviceFilesRead · serviceFilesRemove
//         servicePagesCreate · servicePagesUpdate · servicePagesList · servicePagesGet · servicePagesRemove
//         servicePostsCreate · servicePostsList · servicePostsGet · servicePostsRemove
//         servicePicturesList · servicePicturesGet · servicePicturesRemove
//         serviceCategoriesList · serviceCategoriesGet · serviceCategoriesItems
//         serviceCategoriesCreate · serviceCategoriesUpdate · serviceCategoriesRemove
//         serviceSearch · serviceFilesSearch
//
// Auth:
//   By default (auth:"auto") the bearer token is read from zitadel-oauth.json
//   (same as the CLI `tanit-cli login` flow). Set auth:"bearer" + bearerToken for
//   an explicit token, or auth:"none" to call unauthenticated endpoints.
//
// Server URL:
//   serverUrl resolution order: explicit param -> login pin -> env vars -> compiled default
//   (see pm::services::resolve_cms_server_base).
//   Auth issuer default: pm::services::resolve_zitadel_authority().
//
// Feature guard: compiled only when FEATURE_SERVICE_XBLOX=1.

#include "blocks/service_blocks.hpp"
#include "blocks/block_registry.hpp"
#include "blocks/block_params.hpp"
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_SERVICE_XBLOX) && FEATURE_SERVICE_XBLOX

#include "constants.hpp"               // k_tanit_default_user_id_u8
#include "service_defaults.hpp"
#include "lib/net/http_client.hpp"
#include "lib/pm_zitadel_oauth.hpp"    // pm_zitadel_oauth_read_access_token
#include "media/probe.hpp"             // probe_buffer, MediaKind
#include "cms/pm_cms_http.hpp"
#include "cms/pm_cms_list.hpp"
#include "cms/pm_cms_upload.hpp"
#include "cms/pm_cms_pages.hpp"
#include "cms/pm_cms_posts.hpp"
#include "cms/pm_cms_categories.hpp"
#include "cms/pm_cms_detail.hpp"
#include "cms/pm_cms_download.hpp"
#include "cms/pm_vfs_listing.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace media::xblox::blocks {
namespace {

// ── Auth / URL resolution ──────────────────────────────────────────────────

struct ServiceContext {
    std::string base_url;    // trimmed, no trailing slash
    std::string bearer;      // may be empty for auth:none
    bool ok = false;
    std::string err;
};

ServiceContext resolve_service_context(BlockIO& io)
{
    ServiceContext ctx;

    // Server URL: explicit param -> env vars -> compiled-in default constant
    const std::string explicit_url = io.str("serverUrl");
    ctx.base_url = pm::services::resolve_cms_server_base(explicit_url);
    if (ctx.base_url.empty()) {
        ctx.err = "serverUrl is empty  -  set serverUrl param or configure service defaults";
        return ctx;
    }

    // Auth
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

    // auth:auto  -  try explicit param first, then zitadel-oauth.json
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

nlohmann::json parse_http_body_json(const std::string& body)
{
    try {
        return nlohmann::json::parse(body);
    } catch (...) {
        return body.empty() ? nlohmann::json() : nlohmann::json(body);
    }
}

std::vector<std::string> split_csv_tokens(const std::string& tags)
{
    std::vector<std::string> out;
    std::string cur;
    for (char ch : tags + ",") {
        if (ch == ',') {
            while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front())))
                cur.erase(cur.begin());
            while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back())))
                cur.pop_back();
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    return out;
}

std::vector<std::string> collect_page_category_id_list(BlockIO& io)
{
    std::vector<std::string> out;
    const nlohmann::json& ids = io.json("categoryIds");
    if (ids.is_array()) {
        for (const auto& v : ids) {
            if (v.is_string() && !v.get<std::string>().empty())
                out.push_back(v.get<std::string>());
            else if (v.is_number())
                out.push_back(v.dump());
        }
    }
    const std::string one = io.str("categoryId");
    if (!one.empty())
        out.push_back(one);
    return out;
}

void apply_page_flags_to_create(BlockIO& io, pm::cms::MarkdownPageOptions& opt)
{
    opt.description  = io.str("description");
    opt.parent_id    = io.str("parent");
    opt.category_ids = collect_page_category_id_list(io);
    const std::string vis = io.str("visibility", "");
    if (vis == "private" || io.flag("private", false)) {
        opt.is_public         = false;
        opt.is_public_cli_set = true;
    } else if (vis == "listed") {
        opt.is_public         = true;
        opt.is_public_cli_set = true;
        opt.visible           = false;
        opt.visible_cli_set   = true;
    } else if (vis == "public") {
        opt.is_public         = true;
        opt.is_public_cli_set = true;
        opt.visible           = true;
        opt.visible_cli_set   = true;
    }
    if (io.flag("hidden", false)) {
        opt.visible         = false;
        opt.visible_cli_set = true;
    }
}

void apply_page_flags_to_update(BlockIO& io, pm::cms::UpdateMarkdownPageOptions& opt)
{
    opt.description  = io.str("description");
    opt.category_ids = collect_page_category_id_list(io);
    opt.category_ids_set = !opt.category_ids.empty();
    const std::string parent = io.str("parent");
    if (!parent.empty()) {
        opt.parent_id  = parent;
        opt.parent_set = true;
    }
    if (io.has("private")) {
        opt.is_public     = !io.flag("private", false);
        opt.is_public_set = true;
    }
    if (io.has("hidden")) {
        opt.visible     = !io.flag("hidden", false);
        opt.visible_set = true;
    }
}

BlockResult emit_cms_json(BlockIO& io, const char* block_kind, const std::string& msg, long http_status,
                          nlohmann::json parsed, const std::string& url = {})
{
    io.ok(block_kind, msg)
      .set("url", url)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(http_status)})
      .result(Value::from_json(std::move(parsed)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult emit_list_result(BlockIO& io, const char* block_kind, const pm::cms::CmsListResult& r)
{
    if (!r.ok) {
        io.error(block_kind, r.err.empty() ? "request failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    return emit_cms_json(io, block_kind, "ok", r.http_status, r.json);
}

BlockResult emit_detail_result(BlockIO& io, const char* block_kind, const pm::cms::CmsJsonDetailResult& r)
{
    if (!r.ok) {
        io.error(block_kind, r.err.empty() ? "request failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    return emit_cms_json(io, block_kind, "ok", r.http_status, r.json);
}

void add_auth_header(media::net::HttpRequest& req, const std::string& bearer)
{
    if (bearer.empty()) return;
    req.auth.kind  = media::net::AuthKind::Bearer;
    req.auth.token = bearer;
}

long default_timeout_ms(BlockIO& io)
{
    return std::max<long>(1, static_cast<long>(io.i64("timeoutMs", 30000)));
}

std::string encode_path_segment(std::string_view s)
{
    return pm::cms::percent_encode_path_segment(s);
}

std::string join_vfs_remote(const std::string& dir, const std::string& rel)
{
    std::string out = dir;
    while (!out.empty() && (out.front() == '/' || out.front() == '\\'))
        out.erase(out.begin());
    while (!out.empty() && (out.back() == '/' || out.back() == '\\'))
        out.pop_back();
    std::string r = rel;
    while (!r.empty() && (r.front() == '/' || r.front() == '\\'))
        r.erase(r.begin());
    for (char& c : r) {
        if (c == '\\')
            c = '/';
    }
    if (out.empty())
        return r;
    if (r.empty())
        return out;
    return out + "/" + r;
}

std::int64_t local_file_mtime_ms(const std::filesystem::path& p)
{
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(p, ec);
    if (ec)
        return 0;
    const auto sys = std::chrono::system_clock::now()
                     + (ft - std::filesystem::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

std::string files_upload_parent_dir(const std::string& remote)
{
    const size_t slash = remote.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return remote.substr(0, slash);
}

pm::cms::VfsUploadConflict files_upload_policy_from_io(BlockIO& io, std::string& err)
{
    pm::cms::VfsUploadConflict policy = pm::cms::VfsUploadConflict::IfNewer;
    const std::string conflict = io.str("conflict", "if-newer");
    if (!pm::cms::parse_vfs_upload_conflict(conflict, policy, err))
        return policy;
    if (io.flag("overwrite", false))
        policy = pm::cms::VfsUploadConflict::Overwrite;
    if (io.flag("skip", false))
        policy = pm::cms::VfsUploadConflict::Skip;
    if (io.flag("ifNewer", false))
        policy = pm::cms::VfsUploadConflict::IfNewer;
    return policy;
}

bool apply_page_upload_options(BlockIO& io, const char* block_kind, pm::cms::VfsUploadConflict& policy_out)
{
    std::string policy_err;
    policy_out = files_upload_policy_from_io(io, policy_err);
    if (!policy_err.empty()) {
        io.error(block_kind, policy_err, 2);
        return false;
    }
    return true;
}

std::filesystem::path resolve_markdown_file_path(const std::string& markdown_file)
{
    std::error_code ec;
    std::filesystem::path path = media::path::path_from_utf8(markdown_file);
    if (!path.is_absolute())
        path = std::filesystem::absolute(path, ec);
    return path;
}

nlohmann::json page_uploaded_images_json(const std::vector<pm::cms::PageImageUploadEntry>& uploads)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const pm::cms::PageImageUploadEntry& u : uploads) {
        nlohmann::json row = {{"local_path", u.local_path},
                              {"remote_vfs_path", u.remote_vfs_path},
                              {"public_ref", u.public_ref},
                              {"browser_url", u.browser_url},
                              {"api_get_url", u.api_get_url},
                              {"skipped", u.skipped}};
        if (!u.skip_reason.empty())
            row["skip_reason"] = u.skip_reason;
        arr.push_back(std::move(row));
    }
    return arr;
}

nlohmann::json merge_create_page_result(nlohmann::json parsed, const pm::cms::CreateMarkdownPageResult& r)
{
    if (!parsed.is_object())
        parsed = nlohmann::json::object();
    if (!r.page_id.empty()) {
        parsed["id"]      = r.page_id;
        parsed["page_id"] = r.page_id;
    }
    if (!r.slug.empty())
        parsed["slug"] = r.slug;
    if (!r.owner_id.empty())
        parsed["owner"] = r.owner_id;
    if (!r.url.empty())
        parsed["url"] = r.url;
    if (!r.uploaded_images.empty())
        parsed["uploaded_images"] = page_uploaded_images_json(r.uploaded_images);
    return parsed;
}

pm::cms::MarkdownPageOptions build_pages_create_options(BlockIO& io)
{
    pm::cms::MarkdownPageOptions opt;
    opt.slug            = io.str("slug");
    opt.title           = io.str("title");
    opt.owner_id        = io.str("ownerId");
    opt.tags            = split_csv_tokens(io.str("tags"));
    opt.include_images  = io.flag("includeImages", false);
    opt.use_frontmatter = io.flag("useFrontmatter", true);
    apply_page_flags_to_create(io, opt);
    return opt;
}

nlohmann::json collect_page_category_ids(BlockIO& io)
{
    nlohmann::json out = nlohmann::json::array();
    const nlohmann::json& ids = io.json("categoryIds");
    if (ids.is_array()) {
        for (const auto& v : ids) {
            if (v.is_string() && !v.get<std::string>().empty())
                out.push_back(v.get<std::string>());
            else if (v.is_number())
                out.push_back(v.dump());
        }
    }
    const std::string one = io.str("categoryId");
    if (!one.empty())
        out.push_back(one);
    return out;
}

void apply_page_cli_fields(BlockIO& io, nlohmann::json& payload)
{
    const std::string description = io.str("description");
    const nlohmann::json cats     = collect_page_category_ids(io);
    nlohmann::json meta = payload.contains("meta") && payload["meta"].is_object() ? payload["meta"] : nlohmann::json::object();
    if (!description.empty())
        meta["description"] = description;
    if (!cats.empty())
        meta["categoryIds"] = cats;
    else if (!meta.contains("categoryIds"))
        meta["categoryIds"] = nlohmann::json::array({"uncategorized"});
    if (!meta.empty())
        payload["meta"] = std::move(meta);
    const std::string parent = io.str("parent");
    if (!parent.empty())
        payload["parent"] = parent;
    const std::string vis = io.str("visibility", "");
    if (io.has("private"))
        payload["is_public"] = !io.flag("private", false);
    else if (vis == "private")
        payload["is_public"] = false;
    else if (!payload.contains("is_public"))
        payload["is_public"] = true;
    if (io.has("hidden"))
        payload["visible"] = !io.flag("hidden", false);
    else if (vis == "listed")
        payload["visible"] = false;
    else if (vis == "public")
        payload["visible"] = true;
    else if (!payload.contains("visible"))
        payload["visible"] = false;
}

std::string default_page_owner_or_identifier(BlockIO& io)
{
    std::string owner = io.str("ownerId");
    if (owner.empty())
        owner = io.str("userId");
    if (!owner.empty())
        return owner;
    std::string err;
    if (pm_zitadel_oauth_read_app_user_id(owner, err) && !owner.empty())
        return owner;
    return pm::k_tanit_default_user_id_u8;
}

std::string json_object_string_field(const nlohmann::json& obj, const char* key)
{
    if (obj.is_object() && obj.contains(key) && obj[key].is_string())
        return obj[key].get<std::string>();
    return {};
}

bool resolve_page_id_for_block(BlockIO& io, const char* block_kind, const ServiceContext& ctx,
                               std::string& page_id, nlohmann::json* detail_out = nullptr)
{
    page_id = io.str("pageId");
    if (!page_id.empty())
        return true;

    const std::string slug = io.str("slug");
    if (slug.empty()) {
        io.error(block_kind, std::string(block_kind) + ": provide 'pageId' or 'slug'", 2);
        return false;
    }

    const std::string owner = default_page_owner_or_identifier(io);
    const auto detail = pm::cms::get_user_page_detail(ctx.base_url, ctx.bearer, owner, slug);
    if (!detail.ok) {
        io.error(block_kind, detail.err.empty() ? "page slug lookup failed" : detail.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(detail.http_status)});
        return false;
    }
    const nlohmann::json& page =
        detail.json.contains("page") && detail.json["page"].is_object() ? detail.json["page"] : detail.json;
    page_id = json_object_string_field(page, "id");
    if (page_id.empty()) {
        io.error(block_kind, "page slug resolved without an id", 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(detail.http_status)});
        return false;
    }
    if (detail_out)
        *detail_out = detail.json;
    return true;
}

// ── serviceImagesUpload ───────────────────────────────────────────────────────
//
// POST /api/images?forward=vfs&original=true  (multipart field "file")
// PREVIOUS = array of per-file response objects.

BlockResult service_images_upload_block(BlockIO& io)
{
    constexpr const char* K = "serviceImagesUpload";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    // Collect file paths
    const nlohmann::json& files_json = io.json("files");
    std::vector<std::string> paths;
    if (files_json.is_array()) {
        for (const auto& v : files_json)
            if (v.is_string()) paths.push_back(v.get<std::string>());
    } else {
        const std::string f = io.str("file");
        if (!f.empty()) paths.push_back(f);
    }
    if (paths.empty()) {
        io.error(K, "serviceImagesUpload: no file(s) specified (set 'file' or 'files' param)", 2);
        return BlockResult::Error;
    }

    const std::string endpoint = ctx.base_url + "/api/images";
    const long tmout = default_timeout_ms(io);
    auto& rt = io.runtime();

    nlohmann::json results = nlohmann::json::array();
    for (const auto& fpath : paths) {
        media::net::HttpRequest req;
        req.url    = endpoint;
        req.method = "POST";
        req.query_params.push_back("forward=vfs");
        req.query_params.push_back("original=true");
        add_auth_header(req, ctx.bearer);
        req.timeout_ms       = tmout;
        req.follow_redirects = true;
        req.body_parts.push_back("file@" + fpath);
        req.cancel_check = [&rt]() { return rt.cancel_requested && rt.cancel_requested(rt.user_data); };

        const auto resp = media::net::http_fetch(req);
        nlohmann::json row{
            {"file", fpath},
            {"statusCode", resp.status_code},
            {"ok", resp.ok && resp.status_code >= 200 && resp.status_code < 300},
        };
        if (!resp.ok) {
            row["error"] = resp.error;
        } else {
            try { row["data"] = nlohmann::json::parse(resp.body); }
            catch (...) { row["data"] = resp.body; }
        }
        results.push_back(std::move(row));
    }

    const auto total = static_cast<long long>(results.size());
    const auto failed = static_cast<long long>(
        std::count_if(results.begin(), results.end(),
                      [](const auto& r) { return !r.value("ok", false); }));
    io.ok(K, "uploaded " + std::to_string(total - failed) + "/" + std::to_string(total))
      .set("total",  total)
      .set("failed", failed)
      .result(Value::from_json(std::move(results)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── serviceFilesUpload ────────────────────────────────────────────────────────
//
// POST /api/vfs/upload/{mount}/{remotePath}  (multipart field "file")

BlockResult service_files_upload_block(BlockIO& io)
{
    constexpr const char* K = "serviceFilesUpload";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    std::vector<std::string> paths;
    const nlohmann::json& files_json = io.json("files");
    if (files_json.is_array()) {
        for (const auto& v : files_json)
            if (v.is_string() && !v.get<std::string>().empty())
                paths.push_back(v.get<std::string>());
    }
    const std::string single = io.str("file");
    if (!single.empty())
        paths.push_back(single);
    if (paths.empty()) {
        io.error(K, "serviceFilesUpload: 'file' or 'files' param is required", 2);
        return BlockResult::Error;
    }

    std::string policy_err;
    const pm::cms::VfsUploadConflict policy = files_upload_policy_from_io(io, policy_err);
    if (!policy_err.empty()) {
        io.error(K, policy_err, 2);
        return BlockResult::Error;
    }

    const std::string mount      = io.str("mount", "home");
    const bool upload_public     = io.flag("public", false);
    std::string remote_dir       = io.str("remoteDir");
    const std::string remote_path_override = io.str("remotePath");
    if (upload_public) {
        if (!remote_dir.empty() && remote_dir != "public") {
            io.error(K, "serviceFilesUpload: cannot combine public=true with a different remoteDir", 2);
            return BlockResult::Error;
        }
        remote_dir = "public";
    }
    if (!remote_path_override.empty() && paths.size() != 1u) {
        io.error(K, "serviceFilesUpload: remotePath requires exactly one file", 2);
        return BlockResult::Error;
    }

    struct DirCache {
        std::unordered_map<std::string, std::vector<pm::cms::VfsListEntry>> dirs;
        std::unordered_map<std::string, int> status; // 1 ok, 2 missing, 3 error

        const pm::cms::VfsListEntry* lookup(const std::string& base, const std::string& bearer,
                                            const std::string& mount_name, const std::string& remote)
        {
            const std::string parent = files_upload_parent_dir(remote);
            const std::string key    = mount_name + "\n" + parent;
            auto it = status.find(key);
            if (it == status.end()) {
                const auto listing = pm::cms::list_vfs_files(base, bearer, mount_name, parent, false);
                if (!listing.ok) {
                    status[key] = listing.http_status == 404 ? 2 : 3;
                    dirs[key]   = {};
                } else {
                    status[key] = 1;
                    dirs[key]   = pm::cms::normalize_vfs_listing(listing.json);
                }
                it = status.find(key);
            }
            if (it == status.end() || it->second != 1)
                return nullptr;
            return pm::cms::find_vfs_list_entry(dirs[key], remote);
        }

        bool listed_ok(const std::string& mount_name, const std::string& remote) const
        {
            const std::string key = mount_name + "\n" + files_upload_parent_dir(remote);
            auto it = status.find(key);
            return it != status.end() && it->second != 3;
        }
    };

    DirCache listing_cache;
    const bool need_stat = policy != pm::cms::VfsUploadConflict::Overwrite;
    nlohmann::json results = nlohmann::json::array();
    long long failed = 0;

    for (const std::string& fpath : paths) {
        std::string remote = remote_path_override;
        if (remote.empty())
            remote = join_vfs_remote(remote_dir, media::path::path_to_utf8(media::path::path_from_utf8(fpath).filename()));
        if (!remote.empty() && remote.front() == '/')
            remote.erase(remote.begin());

        nlohmann::json row{{"file", fpath}, {"mount", mount}, {"remotePath", remote}, {"skipped", false}};

        if (need_stat) {
            const pm::cms::VfsListEntry* found = listing_cache.lookup(ctx.base_url, ctx.bearer, mount, remote);
            const bool exists = listing_cache.listed_ok(mount, remote) && found != nullptr;
            const auto skip = pm::cms::decide_vfs_upload_skip(
                policy, exists, local_file_mtime_ms(fpath), found ? found->mtime_ms : 0);
            if (skip.skip) {
                row["ok"]         = true;
                row["skipped"]    = true;
                row["skipReason"] = skip.reason;
                results.push_back(std::move(row));
                continue;
            }
        }

        const pm::cms::VfsUploadResult r
            = pm::cms::upload_vfs_file(ctx.base_url, ctx.bearer, media::path::path_from_utf8(fpath), mount, remote);
        row["ok"]         = r.ok;
        row["statusCode"] = r.http_status;
        row["url"]        = r.browser_url;
        row["apiGetUrl"]  = r.api_get_url;
        row["remotePath"] = r.path.empty() ? remote : r.path;
        if (!r.ok) {
            row["error"] = r.err;
            ++failed;
        } else {
            try { row["data"] = nlohmann::json::parse(r.body_raw); }
            catch (...) { row["data"] = r.body_raw; }
        }
        results.push_back(std::move(row));
    }

    const auto total = static_cast<long long>(results.size());
    if (paths.size() == 1u) {
        const auto& row = results.front();
        if (!row.value("ok", false)) {
            io.error(K, row.value("error", "upload failed"), 1)
              .set("mount", mount)
              .set("remotePath", row.value("remotePath", std::string()))
              .set("statusCode", ParamKind::integer_,
                   Value{row.contains("statusCode") ? row["statusCode"].get<long long>() : 0LL});
            return BlockResult::Error;
        }
        auto b = io.ok(K, row.value("skipped", false)
                            ? ("skipped " + paths.front())
                            : ("uploaded " + paths.front() + " -> " + row.value("remotePath", std::string())));
        b.set("mount", mount)
         .set("remotePath", row.value("remotePath", std::string()))
         .set("skipped", Value{row.value("skipped", false)});
        if (row.contains("skipReason") && row["skipReason"].is_string())
            b.set("skipReason", row["skipReason"].get<std::string>());
        if (row.contains("url") && row["url"].is_string())
            b.set("url", row["url"].get<std::string>());
        if (row.contains("statusCode"))
            b.set("statusCode", ParamKind::integer_, Value{row["statusCode"].get<long long>()});
        b.result(Value::from_json(row), ParamKind::json_value);
        return BlockResult::Ok;
    }

    io.ok(K, "uploaded " + std::to_string(total - failed) + "/" + std::to_string(total))
      .set("total", total)
      .set("failed", failed)
      .result(Value::from_json(std::move(results)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── serviceFilesList ──────────────────────────────────────────────────────────
//
// GET /api/vfs/ls/{mount}/{path}

BlockResult service_files_list_block(BlockIO& io)
{
    constexpr const char* K = "serviceFilesList";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) {
        io.error(K, ctx.err, 2);
        return BlockResult::Error;
    }
    const std::string mount = io.str("mount", "home");
    std::string path = io.str("path", "");
    if (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    const auto r = pm::cms::list_vfs_files(ctx.base_url, ctx.bearer, mount, path, io.flag("includeSize", true));
    return emit_list_result(io, K, r);
}

// ── serviceFilesRead ──────────────────────────────────────────────────────────
//
// GET /api/vfs/read/{mount}/{path}

BlockResult service_files_read_block(BlockIO& io)
{
    constexpr const char* K = "serviceFilesRead";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string mount = io.str("mount", "home");
    std::string path = io.str("path");
    if (path.empty()) { io.error(K, "serviceFilesRead: 'path' param is required", 2); return BlockResult::Error; }
    if (path.front() == '/') path.erase(path.begin());

    const std::string download_as = io.str("downloadAs");
    if (!download_as.empty()) {
        pm::cms::VfsDownloadOptions opt;
        opt.bearer_token   = ctx.bearer;
        opt.endpoint       = pm::cms::VfsDownloadEndpoint::Read;
        opt.overwrite      = io.flag("overwrite", false);
        opt.skip_existing  = !opt.overwrite;
        opt.timeout_ms     = default_timeout_ms(io);
        auto& rt           = io.runtime();
        opt.cancel_check   = [&rt]() { return rt.cancel_requested && rt.cancel_requested(rt.user_data); };
        const auto r = pm::cms::download_vfs_file_to_path(ctx.base_url, mount, path, download_as, opt);
        if (r.skipped) {
            io.ok(K, "skipped existing " + download_as)
              .set("mount", mount)
              .set("path", path)
              .set("skipped", Value{true})
              .set("skipReason", std::string("exists"))
              .result(Value{download_as}, ParamKind::string_);
            return BlockResult::Ok;
        }
        if (!r.ok) {
            io.error(K, r.err.empty() ? "download failed" : r.err, 1)
              .set("url", r.effective_url)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
            return BlockResult::Error;
        }
        const std::string saved = r.local_path.empty() ? download_as : media::path::path_to_utf8(r.local_path);
        io.ok(K, "read " + path)
          .set("url", r.effective_url)
          .set("mount", mount)
          .set("path", path)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
          .result(Value{saved}, ParamKind::string_);
        return BlockResult::Ok;
    }

    const auto r = pm::cms::read_vfs_file_raw(ctx.base_url, ctx.bearer, mount, path);
    if (!r.ok) {
        io.error(K, r.err.empty() ? "request failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }

    constexpr size_t k_max_body_bytes = 4u * 1024u * 1024u;
    if (r.bytes.size() > k_max_body_bytes) {
        io.error(K,
                 "response body is " + std::to_string(r.bytes.size()) +
                     " bytes (limit: 4 MiB without downloadAs). Set 'downloadAs' to save to disk.",
                 2)
          .set("size", ParamKind::integer_, Value{static_cast<long long>(r.bytes.size())});
        return BlockResult::Error;
    }
    const size_t probe_len = std::min<size_t>(r.bytes.size(), 65536u);
    if (probe_len > 0) {
        const auto probe = media::probe::probe_buffer(
            std::string_view(reinterpret_cast<const char*>(r.bytes.data()), probe_len), path);
        const bool is_text_like = !probe.ok
            || probe.kind == media::MediaKind::Text
            || probe.kind == media::MediaKind::Code
            || probe.kind == media::MediaKind::Script;
        if (!is_text_like) {
            io.error(K,
                     "file appears to be binary (" +
                         (probe.content_type.empty() ? std::string("unknown") : probe.content_type) +
                         "). Set 'downloadAs' to save to disk.",
                     2)
              .set("contentType", probe.content_type);
            return BlockResult::Error;
        }
    }

    const std::string result_str(r.bytes.begin(), r.bytes.end());
    io.ok(K, "read " + path)
      .set("mount", mount)
      .set("path", path)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value{result_str}, ParamKind::string_);
    return BlockResult::Ok;
}

// ── serviceFilesRemove ────────────────────────────────────────────────────────
//
// DELETE /api/vfs/delete/{mount}/{path}

BlockResult service_files_remove_block(BlockIO& io)
{
    constexpr const char* K = "serviceFilesRemove";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string mount = io.str("mount", "home");
    std::string path = io.str("path");
    if (path.empty()) { io.error(K, "serviceFilesRemove: 'path' param is required", 2); return BlockResult::Error; }
    if (path.front() == '/') path.erase(path.begin());

    const auto r = pm::cms::delete_vfs_path(ctx.base_url, ctx.bearer, mount, path);
    if (!r.ok) {
        io.error(K, r.err.empty() ? "request failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }

    io.ok(K, "removed " + mount + "/" + path)
      .set("mount", mount)
      .set("path", path)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value{mount + "/" + path}, ParamKind::string_);
    return BlockResult::Ok;
}

// ── servicePagesCreate ────────────────────────────────────────────────────────
//
// POST /api/pages   -  create or update a CMS page.

BlockResult service_pages_create_block(BlockIO& io)
{
    constexpr const char* K = "servicePagesCreate";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const nlohmann::json& body_json = io.json("body");
    if (!body_json.is_null() && body_json.is_object()) {
        nlohmann::json payload = body_json;
        apply_page_cli_fields(io, payload);
        long http = 0;
        std::string body;
        std::string err;
        if (!pm::cms::http_post_json(ctx.base_url, "/api/pages", ctx.bearer, payload.dump(), http, body, err)) {
            io.error(K, err.empty() ? "request failed" : err, 1)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(http)});
            return BlockResult::Error;
        }
        if (http < 200 || http >= 300) {
            io.error(K, "HTTP " + std::to_string(http) + " body=" + body.substr(0, 600), 1)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(http)});
            return BlockResult::Error;
        }
        return emit_cms_json(io, K, "page created", http, parse_http_body_json(body), ctx.base_url + "/api/pages");
    }

    const std::string markdown_file = io.str("markdownFile");
    pm::cms::MarkdownPageOptions opt = build_pages_create_options(io);
    pm::cms::VfsUploadConflict upload_policy = pm::cms::VfsUploadConflict::IfNewer;
    if (!apply_page_upload_options(io, K, upload_policy))
        return BlockResult::Error;
    opt.upload_conflict = upload_policy;

    if (!markdown_file.empty()) {
        const auto r = pm::cms::create_markdown_page_from_file(
            ctx.base_url, ctx.bearer, resolve_markdown_file_path(markdown_file), std::move(opt), {});
        if (!r.ok) {
            io.error(K, r.err.empty() ? "create page failed" : r.err, 1)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
            return BlockResult::Error;
        }
        nlohmann::json parsed = merge_create_page_result(parse_http_body_json(r.body_raw), r);
        return emit_cms_json(io, K, "page created", r.http_status, std::move(parsed), r.url);
    }

    if (opt.slug.empty()) {
        io.error(K, "servicePagesCreate: provide slug, markdownFile, or full body JSON", 2);
        return BlockResult::Error;
    }

    const auto r = pm::cms::create_markdown_page(ctx.base_url, ctx.bearer, io.str("content"), std::move(opt));
    if (!r.ok) {
        io.error(K, r.err.empty() ? "create page failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }

    nlohmann::json parsed = merge_create_page_result(parse_http_body_json(r.body_raw), r);
    return emit_cms_json(io, K, "page created", r.http_status, std::move(parsed), r.url);
}

// ── servicePagesUpdate ────────────────────────────────────────────────────────
//
// PATCH /api/pages/{id}; with slug, resolve /api/user-page/{owner}/{slug} first.

BlockResult service_pages_update_block(BlockIO& io)
{
    constexpr const char* K = "servicePagesUpdate";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    std::string page_id;
    if (!resolve_page_id_for_block(io, K, ctx, page_id))
        return BlockResult::Error;

    const nlohmann::json& body_json = io.json("body");
    if (!body_json.is_null() && body_json.is_object()) {
        nlohmann::json payload = body_json;
        apply_page_cli_fields(io, payload);
        if (payload.empty()) {
            io.error(K, "servicePagesUpdate: provide content/title/newSlug/tags/categoryId or full body JSON", 2);
            return BlockResult::Error;
        }
        const std::string path = "/api/pages/" + encode_path_segment(page_id);
        long http = 0;
        std::string body;
        std::string err;
        if (!pm::cms::http_patch_json(ctx.base_url, path, ctx.bearer, payload.dump(), http, body, err)) {
            io.error(K, err.empty() ? "request failed" : err, 1)
              .set("pageId", page_id)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(http)});
            return BlockResult::Error;
        }
        if (http < 200 || http >= 300) {
            io.error(K, "HTTP " + std::to_string(http) + " body=" + body.substr(0, 600), 1)
              .set("pageId", page_id)
              .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(http)});
            return BlockResult::Error;
        }
        io.ok(K, "page updated")
          .set("url", ctx.base_url + path)
          .set("pageId", page_id)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(http)})
          .result(Value::from_json(parse_http_body_json(body)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    const std::string markdown_file = io.str("markdownFile");
    pm::cms::UpdateMarkdownPageOptions opt;
    opt.page_id              = page_id;
    opt.owner_or_identifier  = default_page_owner_or_identifier(io);
    opt.target_slug          = io.str("slug");
    opt.title                = io.str("title");
    opt.new_slug             = io.str("newSlug");
    opt.use_frontmatter      = io.flag("useFrontmatter", true);
    opt.include_images       = io.flag("includeImages", false);
    const std::vector<std::string> tags = split_csv_tokens(io.str("tags"));
    if (!tags.empty()) {
        opt.tags     = tags;
        opt.tags_set = true;
    }
    apply_page_flags_to_update(io, opt);
    pm::cms::VfsUploadConflict upload_policy = pm::cms::VfsUploadConflict::IfNewer;
    if (!apply_page_upload_options(io, K, upload_policy))
        return BlockResult::Error;
    opt.upload_conflict = upload_policy;

    const pm::cms::CreateMarkdownPageResult r =
        !markdown_file.empty()
            ? pm::cms::update_markdown_page_from_file(
                  ctx.base_url, ctx.bearer, resolve_markdown_file_path(markdown_file), std::move(opt), {})
            : pm::cms::update_markdown_page(ctx.base_url, ctx.bearer, io.str("content"), std::move(opt));
    if (!r.ok) {
        io.error(K, r.err.empty() ? "update page failed" : r.err, 1)
          .set("pageId", page_id)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    nlohmann::json parsed = merge_create_page_result(parse_http_body_json(r.body_raw), r);
    io.ok(K, "page updated")
      .set("url", r.url)
      .set("pageId", r.page_id.empty() ? page_id : r.page_id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value::from_json(std::move(parsed)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── servicePagesList ──────────────────────────────────────────────────────────
//
// GET /api/pages?userId=... (logged-in user when userId is empty)

BlockResult service_pages_list_block(BlockIO& io)
{
    constexpr const char* K = "servicePagesList";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const auto r = pm::cms::list_user_pages(ctx.base_url, ctx.bearer, io.str("userId"));
    return emit_list_result(io, K, r);
}

// ── servicePagesGet ───────────────────────────────────────────────────────────
//
// GET page detail by pageId, or by owner/userId + slug.

BlockResult service_pages_get_block(BlockIO& io)
{
    constexpr const char* K = "servicePagesGet";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string page_id = io.str("pageId");
    if (!page_id.empty())
        return emit_detail_result(io, K, pm::cms::get_page_detail_by_id(ctx.base_url, ctx.bearer, page_id));

    const std::string slug = io.str("slug");
    if (slug.empty()) {
        io.error(K, "servicePagesGet: provide 'pageId' or 'slug'", 2);
        return BlockResult::Error;
    }
    const auto r = pm::cms::get_user_page_detail(ctx.base_url, ctx.bearer, default_page_owner_or_identifier(io), slug);
    if (!r.ok)
        return emit_detail_result(io, K, r);
    const nlohmann::json& page = r.json.contains("page") && r.json["page"].is_object() ? r.json["page"] : r.json;
    io.ok(K, "ok")
      .set("pageId", json_object_string_field(page, "id"))
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value::from_json(r.json), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── servicePagesRemove ────────────────────────────────────────────────────────
//
// DELETE /api/pages/{id}; with slug, resolve /api/user-page/{owner}/{slug} first.

BlockResult service_pages_remove_block(BlockIO& io)
{
    constexpr const char* K = "servicePagesRemove";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string page_id = io.str("pageId");
    const std::string slug    = io.str("slug");
    if (page_id.empty() && slug.empty()) {
        io.error(K, "servicePagesRemove: provide 'pageId' or 'slug'", 2);
        return BlockResult::Error;
    }
    std::string owner = io.str("ownerId");
    if (owner.empty())
        owner = io.str("userId");
    const auto r = pm::cms::delete_page(ctx.base_url, ctx.bearer, page_id, owner, slug);
    nlohmann::json extra = {{"pageId", page_id.empty() ? slug : page_id}};
    if (!r.ok) {
        io.error(K, r.err.empty() ? "request failed" : r.err, 1)
          .set("pageId", extra["pageId"].get<std::string>())
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    nlohmann::json parsed = parse_http_body_json(r.body_raw);
    io.ok(K, "removed page " + extra["pageId"].get<std::string>())
      .set("pageId", extra["pageId"].get<std::string>())
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value::from_json(std::move(parsed)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── servicePicturesList ───────────────────────────────────────────────────────
//
// GET /api/pictures?userId=...&page=...&limit=...

BlockResult service_pictures_list_block(BlockIO& io)
{
    constexpr const char* K = "servicePicturesList";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const auto r = pm::cms::list_user_pictures(ctx.base_url, ctx.bearer, io.str("userId"),
                                               static_cast<int>(io.i64("page", 1)),
                                               static_cast<int>(io.i64("limit", 20)));
    return emit_list_result(io, K, r);
}

// ── servicePicturesGet ────────────────────────────────────────────────────────
//
// GET /api/pictures/{id}

BlockResult service_pictures_get_block(BlockIO& io)
{
    constexpr const char* K = "servicePicturesGet";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("pictureId");
    if (id.empty()) { io.error(K, "servicePicturesGet: 'pictureId' param is required", 2); return BlockResult::Error; }
    return emit_detail_result(io, K, pm::cms::get_picture_detail_by_id(ctx.base_url, ctx.bearer, id));
}

// ── servicePicturesRemove ─────────────────────────────────────────────────────
//
// DELETE /api/pictures/{id}

BlockResult service_pictures_remove_block(BlockIO& io)
{
    constexpr const char* K = "servicePicturesRemove";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("pictureId");
    if (id.empty()) { io.error(K, "servicePicturesRemove: 'pictureId' param is required", 2); return BlockResult::Error; }
    const auto r = pm::cms::delete_picture(ctx.base_url, ctx.bearer, id);
    if (!r.ok) {
        io.error(K, r.err.empty() ? "request failed" : r.err, 1)
          .set("pictureId", id)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    io.ok(K, "removed picture " + id)
      .set("pictureId", id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value::from_json(parse_http_body_json(r.body_raw)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── servicePostsList ──────────────────────────────────────────────────────────
//
// GET /api/posts?page=...&limit=...&userId=...

BlockResult service_posts_list_block(BlockIO& io)
{
    constexpr const char* K = "servicePostsList";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const auto r = pm::cms::list_user_posts(ctx.base_url, ctx.bearer, io.str("userId"),
                                            static_cast<int>(io.i64("page", 1)),
                                            static_cast<int>(io.i64("limit", 20)),
                                            io.str("visibilityFilter"));
    return emit_list_result(io, K, r);
}

// ── servicePostsGet ───────────────────────────────────────────────────────────
//
// GET /api/posts/{id}

BlockResult service_posts_get_block(BlockIO& io)
{
    constexpr const char* K = "servicePostsGet";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("postId");
    if (id.empty()) { io.error(K, "servicePostsGet: 'postId' param is required", 2); return BlockResult::Error; }
    return emit_detail_result(io, K, pm::cms::get_post_detail_by_id(ctx.base_url, ctx.bearer, id));
}

// ── servicePostsRemove ────────────────────────────────────────────────────────
//
// DELETE /api/posts/{id}

BlockResult service_posts_remove_block(BlockIO& io)
{
    constexpr const char* K = "servicePostsRemove";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("postId");
    if (id.empty()) { io.error(K, "servicePostsRemove: 'postId' param is required", 2); return BlockResult::Error; }
    const auto r = pm::cms::delete_post(ctx.base_url, ctx.bearer, id);
    if (!r.ok) {
        io.error(K, r.err.empty() ? "request failed" : r.err, 1)
          .set("postId", id)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    io.ok(K, "removed post " + id)
      .set("postId", id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value::from_json(parse_http_body_json(r.body_raw)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── servicePostsCreate ────────────────────────────────────────────────────────
//
// Full publish flow via CMS lib: POST /api/posts + images + /api/pictures.

BlockResult service_posts_create_block(BlockIO& io)
{
    constexpr const char* K = "servicePostsCreate";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const nlohmann::json& files_json = io.json("files");
    std::vector<std::filesystem::path> image_files;
    if (files_json.is_array()) {
        for (const auto& v : files_json)
            if (v.is_string()) image_files.push_back(v.get<std::string>());
    }
    const std::string single_file = io.str("file");
    if (!single_file.empty())
        image_files.push_back(single_file);

    const auto r = pm::cms::create_post_with_pictures(ctx.base_url, ctx.bearer, image_files, io.str("title"),
                                                      io.str("description"), io.str("visibility", "public"));
    if (!r.ok) {
        io.error(K, r.err.empty() ? "create post failed" : r.err, 1);
        return BlockResult::Error;
    }

    nlohmann::json post_resp;
    if (!r.http_raw_steps.empty())
        post_resp = parse_http_body_json(r.http_raw_steps[0].body_raw);
    nlohmann::json result{
        {"postId", r.post_id},
        {"pictureIds", r.picture_ids},
        {"post", post_resp},
    };
    io.ok(K, "post created: " + r.post_id)
      .set("postId", r.post_id)
      .set("statusCode", ParamKind::integer_,
           Value{static_cast<long long>(r.http_raw_steps.empty() ? 0 : r.http_raw_steps[0].http_status)})
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── serviceCategoriesList ─────────────────────────────────────────────────────

BlockResult service_categories_list_block(BlockIO& io)
{
    constexpr const char* K = "serviceCategoriesList";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    std::string owner_id;
    if (!io.flag("all", false)) {
        owner_id = io.str("userId");
        if (owner_id.empty())
            owner_id = pm::cms::resolve_current_app_user_id_or_default();
    }
    const auto r = pm::cms::list_categories(ctx.base_url, ctx.bearer, io.str("parentSlug"),
                                            io.flag("includeChildren", false), io.str("lang"),
                                            io.str("type", "pages"), owner_id);
    return emit_list_result(io, K, r);
}

BlockResult service_categories_get_block(BlockIO& io)
{
    constexpr const char* K = "serviceCategoriesGet";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("categoryId");
    if (id.empty()) { io.error(K, "serviceCategoriesGet: 'categoryId' param is required", 2); return BlockResult::Error; }
    return emit_detail_result(io, K, pm::cms::get_category_by_id(ctx.base_url, ctx.bearer, id));
}

BlockResult service_categories_items_block(BlockIO& io)
{
    constexpr const char* K = "serviceCategoriesItems";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string slug = io.str("slug");
    if (slug.empty()) { io.error(K, "serviceCategoriesItems: 'slug' param is required", 2); return BlockResult::Error; }
    return emit_detail_result(io, K,
                              pm::cms::get_category_items(ctx.base_url, ctx.bearer, slug,
                                                          static_cast<int>(io.i64("limit", 0)),
                                                          io.flag("includeDescendants", true)));
}

BlockResult service_categories_create_block(BlockIO& io)
{
    constexpr const char* K = "serviceCategoriesCreate";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string name = io.str("name");
    if (name.empty()) { io.error(K, "serviceCategoriesCreate: 'name' param is required", 2); return BlockResult::Error; }
    pm::cms::CategoryOptions opt;
    opt.name          = name;
    opt.slug          = io.str("slug");
    opt.description   = io.str("description");
    opt.visibility    = io.str("visibility");
    opt.parent_id     = io.str("parentId");
    opt.relation_type = io.str("relationType");
    opt.meta_type     = io.str("type", "pages");
    opt.meta_type_set = true;
    const auto r = pm::cms::create_category(ctx.base_url, ctx.bearer, std::move(opt));
    if (!r.ok) {
        io.error(K, r.err.empty() ? "create category failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    nlohmann::json parsed = r.json.is_null() ? parse_http_body_json(r.body_raw) : r.json;
    if (!parsed.is_object())
        parsed = nlohmann::json::object();
    if (!r.id.empty())
        parsed["id"] = r.id;
    if (!r.slug.empty())
        parsed["slug"] = r.slug;
    if (!r.name.empty())
        parsed["name"] = r.name;
    return emit_cms_json(io, K, "category created", r.http_status, std::move(parsed));
}

BlockResult service_categories_update_block(BlockIO& io)
{
    constexpr const char* K = "serviceCategoriesUpdate";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("categoryId");
    if (id.empty()) { io.error(K, "serviceCategoriesUpdate: 'categoryId' param is required", 2); return BlockResult::Error; }
    pm::cms::CategoryOptions opt;
    opt.name          = io.str("name");
    opt.slug          = io.str("slug");
    opt.description   = io.str("description");
    opt.visibility    = io.str("visibility");
    opt.meta_type     = io.str("type", "pages");
    opt.meta_type_set = true;
    const auto r = pm::cms::update_category(ctx.base_url, ctx.bearer, id, std::move(opt));
    if (!r.ok) {
        io.error(K, r.err.empty() ? "update category failed" : r.err, 1)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    nlohmann::json parsed = r.json.is_null() ? parse_http_body_json(r.body_raw) : r.json;
    return emit_cms_json(io, K, "category updated", r.http_status, std::move(parsed));
}

BlockResult service_categories_remove_block(BlockIO& io)
{
    constexpr const char* K = "serviceCategoriesRemove";
    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }
    const std::string id = io.str("categoryId");
    if (id.empty()) { io.error(K, "serviceCategoriesRemove: 'categoryId' param is required", 2); return BlockResult::Error; }
    const std::string expected_type = io.str("type", "pages");
    const auto cr = pm::cms::get_category_by_id(ctx.base_url, ctx.bearer, id);
    if (!cr.ok)
        return emit_detail_result(io, K, cr);
    const nlohmann::json& node = cr.json.contains("category") && cr.json["category"].is_object()
        ? cr.json["category"] : cr.json;
    if (!pm::cms::category_matches_meta_type(node, expected_type)) {
        io.error(K, "category meta.type is " + pm::cms::category_meta_type(node) + " (expected type " + expected_type + ")",
                 2);
        return BlockResult::Error;
    }
    const auto r = pm::cms::delete_category(ctx.base_url, ctx.bearer, id);
    if (!r.ok) {
        io.error(K, r.err.empty() ? "request failed" : r.err, 1)
          .set("categoryId", id)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)});
        return BlockResult::Error;
    }
    io.ok(K, "removed category " + id)
      .set("categoryId", id)
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(r.http_status)})
      .result(Value::from_json(parse_http_body_json(r.body_raw)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── serviceSearch ─────────────────────────────────────────────────────────────
//
// GET /api/search?q=...&type=...&limit=...&sizes=...&formats=...&visibilityFilter=...
//
// Full-text search across pages, posts, pictures, VFS files and places.
// Returns an array of enriched FeedPost-shaped objects.  Bearer token is
// optional  -  unauthenticated calls only see public content.

BlockResult service_search_block(BlockIO& io)
{
    constexpr const char* K = "serviceSearch";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string q = io.str("q");
    if (q.empty()) { io.error(K, "serviceSearch: 'q' param is required", 2); return BlockResult::Error; }

    media::net::HttpRequest req;
    req.method = "GET";
    req.url    = ctx.base_url + "/api/search";
    add_auth_header(req, ctx.bearer);
    req.timeout_ms       = default_timeout_ms(io);
    req.follow_redirects = true;

    req.query_params.push_back("q=" + q);

    const long long limit = io.i64("limit", 20);
    req.query_params.push_back("limit=" + std::to_string(limit));

    const std::string type = io.str("type", "all");
    if (type != "all" && !type.empty())
        req.query_params.push_back("type=" + type);

    const std::string sizes = io.str("sizes");
    if (!sizes.empty()) req.query_params.push_back("sizes=" + sizes);

    const std::string formats = io.str("formats");
    if (!formats.empty()) req.query_params.push_back("formats=" + formats);

    const std::string vis = io.str("visibilityFilter");
    if (!vis.empty()) req.query_params.push_back("visibilityFilter=" + vis);

    auto& rt = io.runtime();
    req.cancel_check = [&rt]() { return rt.cancel_requested && rt.cancel_requested(rt.user_data); };

    const auto resp = media::net::http_fetch(req);
    if (!resp.ok) {
        io.error(K, resp.error.empty() ? "request failed" : resp.error, 1)
          .set("url",        req.url)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)});
        return BlockResult::Error;
    }

    nlohmann::json parsed;
    try { parsed = nlohmann::json::parse(resp.body); }
    catch (...) { parsed = resp.body; }

    const long long count = parsed.is_array() ? static_cast<long long>(parsed.size()) : 0LL;
    io.ok(K, std::to_string(count) + " result(s)")
      .set("url",        req.url)
      .set("count",      ParamKind::integer_, Value{count})
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)})
      .result(Value::from_json(parsed), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── serviceFilesSearch ────────────────────────────────────────────────────────
//
// GET /api/vfs/search/{mount}/{subpath}?q=...&type=...&maxResults=...&fts=1
//
// Searches VFS files by name-walk (default) or PostgreSQL FTS index (fts=1).
// Returns {results:[...], total:N, truncated:bool}.
// Note: FTS results only reflect the last index run  -  fresh uploads may not
// appear until the index is rebuilt.

BlockResult service_files_search_block(BlockIO& io)
{
    constexpr const char* K = "serviceFilesSearch";

    ServiceContext ctx = resolve_service_context(io);
    if (!ctx.ok) { io.error(K, ctx.err, 2); return BlockResult::Error; }

    const std::string q = io.str("q");
    if (q.empty()) { io.error(K, "serviceFilesSearch: 'q' param is required (min 2 chars on server)", 2); return BlockResult::Error; }

    const std::string mount = io.str("mount", "home");
    std::string path = io.str("path");
    if (!path.empty() && path.front() == '/') path.erase(path.begin());

    const std::string url_path = "/api/vfs/search/" + mount + (path.empty() ? "" : "/" + path);

    media::net::HttpRequest req;
    req.method = "GET";
    req.url    = ctx.base_url + url_path;
    add_auth_header(req, ctx.bearer);
    req.timeout_ms       = default_timeout_ms(io);
    req.follow_redirects = true;

    req.query_params.push_back("q=" + q);

    const long long max_results = io.i64("maxResults", 200);
    req.query_params.push_back("maxResults=" + std::to_string(max_results));

    const std::string type = io.str("type", "all");
    if (!type.empty()) req.query_params.push_back("type=" + type);

    if (io.i64("fts", 0) != 0) req.query_params.push_back("fts=1");

    auto& rt = io.runtime();
    req.cancel_check = [&rt]() { return rt.cancel_requested && rt.cancel_requested(rt.user_data); };

    const auto resp = media::net::http_fetch(req);
    if (!resp.ok) {
        io.error(K, resp.error.empty() ? "request failed" : resp.error, 1)
          .set("url",        req.url)
          .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)});
        return BlockResult::Error;
    }

    nlohmann::json parsed;
    try { parsed = nlohmann::json::parse(resp.body); }
    catch (...) { parsed = resp.body; }

    const long long total     = parsed.is_object() ? static_cast<long long>(parsed.value("total", 0))   : 0LL;
    const bool      truncated = parsed.is_object() && parsed.value("truncated", false);

    io.ok(K, std::to_string(total) + " found" + (truncated ? " (truncated)" : ""))
      .set("url",        req.url)
      .set("mount",      mount)
      .set("total",      ParamKind::integer_, Value{total})
      .set("truncated",  Value{truncated})
      .set("statusCode", ParamKind::integer_, Value{static_cast<long long>(resp.status_code)})
      .result(Value::from_json(parsed), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── Shared auth params ─────────────────────────────────────────────────────────

std::vector<ParamDef> service_auth_params()
{
    return {
        pd_str("serverUrl").grp("auth").deep().dflt("")
            .desc("Tanit service base URL (e.g. https://tanit.polymech.info). "
                  "Supports ${var} / ${ENV:NAME} interpolation. "
                  "Empty = resolved from SERVER_URL / VITE_SERVER_IMAGE_API_URL env vars."),
        pd_enum("auth", {"auto","bearer","none"}).grp("auth").dflt("auto")
            .desc("Auth mode. auto = bearerToken param if set, else zitadel-oauth.json (run `tanit-cli login` first). "
                  "bearer = explicit bearerToken only. none = unauthenticated."),
        pd_str("bearerToken").grp("auth").deep().dflt("")
            .desc("Explicit JWT bearer token. Supports ${var} interpolation."),
    };
}

ParamDef timeout_param()
{
    return pd_ms("timeoutMs").grp("timeouts").dflt(30000).range(1000, 300000)
        .desc("HTTP timeout in ms.");
}

} // namespace

// ── registration ───────────────────────────────────────────────────────────────

void register_service_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_SERVICE_XBLOX) && FEATURE_SERVICE_XBLOX
    using J = nlohmann::json;

    // ── Images ──────────────────────────────────────────────────────────────
    registry["serviceImagesUpload"] = bdb(
        "serviceImagesUpload", service_images_upload_block,
        "Upload Image(s)", "Service",
        "POST /api/images?forward=vfs&original=true  -  multipart upload one or more image files. "
        "PREVIOUS = array of per-file response objects.",
        J{{"kind","serviceImagesUpload"},{"auth","auto"},{"storeAs","uploadResults"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_file("file").in().deep().must_exist().guard().dflt("")
                    .desc("Single image file path to upload. Supports ${var} interpolation. Use 'files' for multiple."),
                pd_args("files").in().deep().guard().ui({{"picks_path",true},{"item_kind","file_path"}}).dflt(nullptr)
                    .desc("Multiple image file paths. Each is uploaded separately."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_int("failed").out().desc("Number of files that failed to upload."));
            p.push_back(pd_store_as().dflt("uploadResults"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Upload results")
        .exec({false, true, true});

    // ── Files ────────────────────────────────────────────────────────────────
    registry["serviceFilesUpload"] = bdb(
        "serviceFilesUpload", service_files_upload_block,
        "Upload File", "Service",
        "POST /api/vfs/upload/{mount}/{remotePath}  -  upload any file to the VFS. "
            "Default conflict: if-newer (overwrite older remotes; skip when remote is newer or the same).",
        J{{"kind","serviceFilesUpload"},{"auth","auto"},{"mount","home"},{"conflict","if-newer"},{"storeAs","fileResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_file("file").in().deep().must_exist().guard().dflt("")
                    .desc("Local file path to upload. Supports ${var} interpolation. Use 'files' for multiple."),
                pd_args("files").in().deep().guard().ui({{"picks_path",true},{"item_kind","file_path"}}).dflt(nullptr)
                    .desc("Multiple local file paths. Each is uploaded separately."),
                pd_str("mount").grp("vfs").deep().dflt("home")
                    .desc("VFS mount name (e.g. home). Supports ${var} interpolation."),
                pd_str("remoteDir").grp("vfs").deep().dflt("")
                    .desc("Remote VFS directory. Each file is placed under this path as its filename. Empty = mount root. Prefer public=true for the shared public folder."),
                pd_bool("public").grp("vfs").dflt(false)
                    .desc("Upload into home/public (same as remoteDir=public). Filebrowser links use ?mode=thumb&file=…"),
                pd_str("remotePath").grp("vfs").deep().dflt("")
                    .desc("Exact remote VFS path for one file (overrides remoteDir). Supports ${var} interpolation."),
                pd_enum("conflict", {"if-newer","skip","overwrite"}).grp("conflict").dflt("if-newer")
                    .desc("Conflict policy. if-newer (default) overwrites older remotes; skip leaves existing; overwrite always replaces."),
                pd_bool("skip").grp("conflict").dflt(false)
                    .desc("Skip when the remote file already exists (any age)."),
                pd_bool("overwrite").grp("conflict").dflt(false)
                    .desc("Always replace the remote file."),
                pd_bool("ifNewer").grp("conflict").dflt(false)
                    .desc("Overwrite remote only when the local file is newer (default policy)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_int("failed").out().desc("Number of files that failed to upload (files[] mode)."));
            p.push_back(pd_store_as().dflt("fileResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "File result")
        .output("statusCode", ParamKind::integer_, "Status code")
        .output("skipped", ParamKind::boolean_, "Skipped existing remote")
        .exec({false, true, true});

    registry["serviceFilesList"] = bdb(
        "serviceFilesList", service_files_list_block,
        "List Files", "Service",
        "GET /api/vfs/ls/{mount}/{path}  -  list files in a VFS directory.",
        J{{"kind","serviceFilesList"},{"auth","auto"},{"mount","home"},{"path",""},{"storeAs","fileList"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("mount").in().deep().dflt("home").desc("VFS mount name. Supports ${var} interpolation."),
                pd_str("path").in().deep().dflt("").desc("Directory path within the mount (empty = root). Supports ${var} interpolation."),
                pd_bool("includeSize").grp("vfs").dflt(true)
                    .desc("Include file/directory sizes in the listing (CLI default)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("fileList"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "File list")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["serviceFilesRead"] = bdb(
        "serviceFilesRead", service_files_read_block,
        "Read File", "Service",
        "GET /api/vfs/read/{mount}/{path}  -  download a VFS file. "
        "Set downloadAs to save to disk (PREVIOUS = saved path); else PREVIOUS = body string.",
        J{{"kind","serviceFilesRead"},{"auth","auto"},{"mount","home"},{"storeAs","fileContent"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("mount").in().deep().dflt("home").desc("VFS mount name. Supports ${var} interpolation."),
                pd_str("path").in().req().deep().dflt("").desc("File path within the mount. Supports ${var} interpolation."),
                pd_outpath("downloadAs").grp("download").deep().writable().create_parents().guard().dflt("")
                    .desc("Save response body to this path. Supports ${var} interpolation. Parent directories are created automatically. Empty = return body as string."),
                pd_bool("overwrite").grp("download").dflt(false)
                    .desc("Re-download even when downloadAs already exists (default: skip existing)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("fileContent"));
            return p;
        }())
        .primary_output(ParamKind::string_, "File content or saved path")
        .output("statusCode", ParamKind::integer_, "Status code")
        .output("size", ParamKind::integer_, "Response size", {}, true)
        .output("skipped", ParamKind::boolean_, "Skipped existing local download")
        .exec({false, true, true});

    registry["serviceFilesRemove"] = bdb(
        "serviceFilesRemove", service_files_remove_block,
        "Remove File", "Service",
        "DELETE /api/vfs/delete/{mount}/{path}  -  remove a file or folder from the VFS.",
        J{{"kind","serviceFilesRemove"},{"auth","auto"},{"mount","home"},{"storeAs","removeResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("mount").in().deep().dflt("home").desc("VFS mount name. Supports ${var} interpolation."),
                pd_str("path").in().req().deep().dflt("").desc("File or folder path to remove. Supports ${var} interpolation."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("removeResult"));
            return p;
        }())
        .primary_output(ParamKind::string_, "Removed path")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── Pages ─────────────────────────────────────────────────────────────────
    registry["servicePagesCreate"] = bdb(
        "servicePagesCreate", service_pages_create_block,
        "Create Page", "Service",
        "POST /api/pages  -  create a CMS page. Provide slug + title + content, or a full body JSON object.",
        J{{"kind","servicePagesCreate"},{"auth","auto"},{"visibility","listed"},{"storeAs","page"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("slug").in().deep().dflt("").desc("Page slug (URL-safe identifier). Required unless 'body' or 'markdownFile' is set. Supports ${var} interpolation."),
                pd_str("title").in().deep().dflt("").desc("Page title. Supports ${var} interpolation."),
                pd_str("markdownFile").in().deep().guard().ui({{"picks_path",true},{"item_kind","file_path"}}).dflt("")
                    .desc("Local .md/.page file to publish (same as CLI pages create). Supports ${var} interpolation."),
                pd_prompt("content").in().deep().dflt("").desc("Page content (Markdown). Wrapped as a markdown-text widget. Supports ${var} interpolation."),
                pd_bool("includeImages").grp("images").dflt(false)
                    .desc("Extract co-located image refs from markdownFile, upload to home/public, rewrite refs."),
                pd_bool("useFrontmatter").grp("meta").dflt(true)
                    .desc("When markdownFile is .md, apply YAML front matter to title/slug/tags/category."),
                pd_str("tags").grp("meta").deep().dflt("").desc("Comma-separated tag list. Supports ${var} interpolation."),
                pd_str("description").grp("meta").deep().dflt("").desc("Optional page description (stored in meta.description)."),
                pd_str("parent").grp("meta").deep().dflt("").desc("Optional parent page UUID or slug."),
                pd_str("categoryId").grp("meta").deep().dflt("")
                    .desc("Category slug, display name, or UUID. Missing cats are created in user space on the server."),
                pd_args("categoryIds").grp("meta").dflt(nullptr)
                    .desc("Multiple category slugs/names/UUIDs. Missing cats are created in user space on the server."),
                pd_bool("private").grp("meta").dflt(false).desc("Create with is_public=false."),
                pd_bool("hidden").grp("meta").dflt(false).desc("Create with visible=false (default is link-only)."),
                pd_enum("visibility",{"","public","listed","private"}).grp("meta").dflt("")
                    .desc("Page visibility: listed=link-only (default), public=listed in feeds, private."),
                pd_str("ownerId").grp("meta").deep().dflt("").desc("Owner user UUID (default: from zitadel-oauth.json)."),
                pd_enum("conflict", {"if-newer","skip","overwrite"}).grp("images").dflt("if-newer")
                    .desc("Image upload conflict policy when includeImages is true."),
                pd_bool("skip").grp("images").dflt(false).desc("Skip existing remote images."),
                pd_bool("overwrite").grp("images").dflt(false).desc("Always replace remote images."),
                pd_bool("ifNewer").grp("images").dflt(false).desc("Replace remote images only when local is newer."),
                pd_json("body").in().dflt(nullptr).desc("Full POST body as JSON object  -  overrides all individual params."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("page"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Page")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["servicePagesUpdate"] = bdb(
        "servicePagesUpdate", service_pages_update_block,
        "Update Page", "Service",
        "PATCH /api/pages/{pageId}; or resolve ownerId/userId + slug before updating.",
        J{{"kind","servicePagesUpdate"},{"auth","auto"},{"storeAs","page"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("pageId").in().deep().dflt("").desc("Page UUID. If empty, slug lookup is used. Supports ${var} interpolation."),
                pd_str("slug").in().deep().dflt("").desc("Existing page slug for lookup when pageId is empty. Supports ${var} interpolation."),
                pd_str("ownerId").in().deep().dflt("").desc("Owner user UUID or username for slug lookup. Defaults to logged-in app user when empty."),
                pd_str("userId").grp("lookup").deep().dflt("").desc("Alias/fallback for ownerId in slug lookup. Supports ${var} interpolation."),
                pd_str("markdownFile").in().deep().guard().ui({{"picks_path",true},{"item_kind","file_path"}}).dflt("")
                    .desc("Local .md/.page file to publish. Supports ${var} interpolation."),
                pd_prompt("content").in().deep().dflt("").desc("Replacement page content (Markdown). Wrapped as a markdown-text layout. Supports ${var} interpolation."),
                pd_bool("includeImages").grp("images").dflt(false)
                    .desc("Extract co-located image refs from markdownFile, upload to home/public, rewrite refs."),
                pd_bool("useFrontmatter").grp("meta").dflt(true)
                    .desc("When markdownFile is .md, apply YAML front matter to title/slug/tags/category."),
                pd_str("title").in().deep().dflt("").desc("Optional replacement page title. Supports ${var} interpolation."),
                pd_str("newSlug").in().deep().dflt("").desc("Optional replacement page slug. Supports ${var} interpolation."),
                pd_str("tags").grp("meta").deep().dflt("").desc("Comma-separated replacement tag list. Supports ${var} interpolation."),
                pd_str("description").grp("meta").deep().dflt("").desc("Optional replacement meta description."),
                pd_str("parent").grp("meta").deep().dflt("").desc("Optional replacement parent page UUID or slug."),
                pd_str("categoryId").grp("meta").deep().dflt("")
                    .desc("Category slug, display name, or UUID. Missing cats are created in user space on the server."),
                pd_args("categoryIds").grp("meta").dflt(nullptr)
                    .desc("Multiple category slugs/names/UUIDs. Missing cats are created in user space on the server."),
                pd_bool("private").grp("meta").dflt(false).desc("Set is_public=false when this flag is present."),
                pd_bool("hidden").grp("meta").dflt(false).desc("Set visible=false when this flag is present."),
                pd_enum("conflict", {"if-newer","skip","overwrite"}).grp("images").dflt("if-newer")
                    .desc("Image upload conflict policy when includeImages is true."),
                pd_bool("skip").grp("images").dflt(false).desc("Skip existing remote images."),
                pd_bool("overwrite").grp("images").dflt(false).desc("Always replace remote images."),
                pd_bool("ifNewer").grp("images").dflt(false).desc("Replace remote images only when local is newer."),
                pd_json("body").in().dflt(nullptr).desc("Full PATCH body as JSON object  -  overrides individual update params."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("page"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Page")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["servicePagesList"] = bdb(
        "servicePagesList", service_pages_list_block,
        "List Pages", "Service",
        "GET /api/pages?userId=...&page=...&limit=...  -  list pages.",
        J{{"kind","servicePagesList"},{"auth","auto"},{"page",1},{"limit",20},{"storeAs","pages"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("userId").in().deep().dflt("").desc("Filter by owner user UUID. Supports ${var} interpolation."),
                pd_int("page").grp("pagination").dflt(1).range(1, 10000).desc("Page number (1-based)."),
                pd_int("limit").grp("pagination").dflt(20).range(1, 200).desc("Results per page."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("pages"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Pages")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["servicePagesGet"] = bdb(
        "servicePagesGet", service_pages_get_block,
        "Get Page", "Service",
        "GET /api/user-page/{identifier}/{slug}  -  fetch a single page by pageId or ownerId/userId + slug.",
        J{{"kind","servicePagesGet"},{"auth","auto"},{"storeAs","page"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("pageId").in().deep().dflt("").desc("Page UUID. If empty, slug lookup is used. Supports ${var} interpolation."),
                pd_str("slug").in().deep().dflt("").desc("Page slug for lookup when pageId is empty. Supports ${var} interpolation."),
                pd_str("ownerId").in().deep().dflt("").desc("Owner user UUID or username for slug lookup. Defaults to logged-in app user when empty."),
                pd_str("userId").grp("lookup").deep().dflt("").desc("Alias/fallback for ownerId in slug lookup. Supports ${var} interpolation."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("page"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Page")
        .output("statusCode", ParamKind::integer_, "Status code", {}, true)
        .exec({false, true, true});

    registry["servicePagesRemove"] = bdb(
        "servicePagesRemove", service_pages_remove_block,
        "Remove Page", "Service",
        "DELETE /api/pages/{pageId}; or resolve ownerId/userId + slug before deleting.",
        J{{"kind","servicePagesRemove"},{"auth","auto"},{"storeAs","removeResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("pageId").in().deep().dflt("").desc("Page UUID. If empty, slug lookup is used. Supports ${var} interpolation."),
                pd_str("slug").in().deep().dflt("").desc("Page slug for lookup when pageId is empty. Supports ${var} interpolation."),
                pd_str("ownerId").in().deep().dflt("").desc("Owner user UUID or username for slug lookup. Defaults to logged-in app user when empty."),
                pd_str("userId").grp("lookup").deep().dflt("").desc("Alias/fallback for ownerId in slug lookup. Supports ${var} interpolation."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("removeResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Remove result")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── Pictures ──────────────────────────────────────────────────────────────
    registry["servicePicturesList"] = bdb(
        "servicePicturesList", service_pictures_list_block,
        "List Pictures", "Service",
        "GET /api/pictures?userId=...&page=...&limit=...  -  list picture records.",
        J{{"kind","servicePicturesList"},{"auth","auto"},{"page",1},{"limit",20},{"storeAs","pictures"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("userId").in().deep().dflt("").desc("Filter by owner user UUID. Supports ${var} interpolation."),
                pd_int("page").grp("pagination").dflt(1).range(1, 10000).desc("Page number (1-based)."),
                pd_int("limit").grp("pagination").dflt(20).range(1, 200).desc("Results per page."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("pictures"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Pictures")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["servicePicturesGet"] = bdb(
        "servicePicturesGet", service_pictures_get_block,
        "Get Picture", "Service",
        "GET /api/pictures/{pictureId}  -  fetch a picture record.",
        J{{"kind","servicePicturesGet"},{"auth","auto"},{"storeAs","picture"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), pd_str("pictureId").in().req().deep().dflt("").desc("Picture record ID. Supports ${var} interpolation."));
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("picture"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Picture")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["servicePicturesRemove"] = bdb(
        "servicePicturesRemove", service_pictures_remove_block,
        "Remove Picture", "Service",
        "DELETE /api/pictures/{pictureId}  -  remove a picture record.",
        J{{"kind","servicePicturesRemove"},{"auth","auto"},{"storeAs","removeResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), pd_str("pictureId").in().req().deep().dflt("").desc("Picture record ID. Supports ${var} interpolation."));
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("removeResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Remove result")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── Posts ─────────────────────────────────────────────────────────────────
    registry["servicePostsCreate"] = bdb(
        "servicePostsCreate", service_posts_create_block,
        "Create Post", "Service",
        "Full publish flow: POST /api/posts then upload each image via /api/images and attach via /api/pictures. "
        "PREVIOUS = {postId, pictureIds, post}.",
        J{{"kind","servicePostsCreate"},{"auth","auto"},{"visibility","public"},{"storeAs","post"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("title").in().deep().dflt("").desc("Post title. Supports ${var} interpolation."),
                pd_str("description").in().deep().dflt("").desc("Post description. Supports ${var} interpolation."),
                pd_enum("visibility",{"public","listed","private"}).grp("meta").dflt("public")
                    .desc("Post visibility."),
                pd_str("ownerId").grp("meta").deep().dflt("").desc("Owner user UUID."),
                pd_file("file").grp("images").deep().must_exist().guard().dflt("").desc("Single image file to upload and attach. Supports ${var} interpolation."),
                pd_args("files").grp("images").deep().guard().ui({{"picks_path",true},{"item_kind","file_path"}}).dflt(nullptr)
                    .desc("Multiple image files to upload and attach (positional)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("post"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Post")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["servicePostsList"] = bdb(
        "servicePostsList", service_posts_list_block,
        "List Posts", "Service",
        "GET /api/posts?page=...&limit=...&userId=...&visibilityFilter=...  -  list posts.",
        J{{"kind","servicePostsList"},{"auth","auto"},{"page",1},{"limit",20},{"storeAs","posts"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("userId").in().deep().dflt("").desc("Filter by owner user UUID. Supports ${var} interpolation."),
                pd_int("page").grp("pagination").dflt(1).range(1, 10000).desc("Page number (1-based)."),
                pd_int("limit").grp("pagination").dflt(20).range(1, 200).desc("Results per page."),
                pd_enum("visibilityFilter",{"","listed","unlisted","private","non-public","all","public"}).grp("filter").dflt("")
                    .desc("Owner-only: listed/unlisted (link-only), private, non-public, all, or public (default)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("posts"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Posts")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["servicePostsGet"] = bdb(
        "servicePostsGet", service_posts_get_block,
        "Get Post", "Service",
        "GET /api/posts/{postId}  -  fetch a post with its pictures.",
        J{{"kind","servicePostsGet"},{"auth","auto"},{"storeAs","post"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), pd_str("postId").in().req().deep().dflt("").desc("Post ID. Supports ${var} interpolation."));
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("post"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Post")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["servicePostsRemove"] = bdb(
        "servicePostsRemove", service_posts_remove_block,
        "Remove Post", "Service",
        "DELETE /api/posts/{postId}  -  remove a post.",
        J{{"kind","servicePostsRemove"},{"auth","auto"},{"storeAs","removeResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), pd_str("postId").in().req().deep().dflt("").desc("Post ID. Supports ${var} interpolation."));
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("removeResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Remove result")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── Categories ────────────────────────────────────────────────────────────
    registry["serviceCategoriesList"] = bdb(
        "serviceCategoriesList", service_categories_list_block,
        "List Categories", "Service",
        "GET /api/categories  -  list categories (default: own only; optional parentSlug / includeChildren / type).",
        J{{"kind","serviceCategoriesList"},{"auth","auto"},{"type","pages"},{"storeAs","categories"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("userId").in().deep().dflt("").desc("Owner UUID for own-only list (default: current app user)."),
                pd_bool("all").grp("filter").dflt(false).desc("List all visible categories, not just own."),
                pd_str("parentSlug").in().deep().dflt("").desc("Optional parent category slug."),
                pd_bool("includeChildren").grp("filter").dflt(false).desc("Include nested children."),
                pd_str("lang").grp("filter").deep().dflt("").desc("Optional Accept-Language / lang query."),
                pd_str("type").grp("filter").deep().dflt("pages").desc("Category meta.type filter (default: pages)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("categories"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Categories")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["serviceCategoriesGet"] = bdb(
        "serviceCategoriesGet", service_categories_get_block,
        "Get Category", "Service",
        "GET /api/categories/{categoryId}.",
        J{{"kind","serviceCategoriesGet"},{"auth","auto"},{"storeAs","category"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), pd_str("categoryId").in().req().deep().dflt("").desc("Category UUID."));
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("category"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Category")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["serviceCategoriesItems"] = bdb(
        "serviceCategoriesItems", service_categories_items_block,
        "Category Items", "Service",
        "GET /api/categories/{slug}/items.",
        J{{"kind","serviceCategoriesItems"},{"auth","auto"},{"includeDescendants",true},{"storeAs","items"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("slug").in().req().deep().dflt("").desc("Category slug."),
                pd_int("limit").grp("filter").dflt(0).range(0, 1000).desc("Optional item limit (0 = server default)."),
                pd_bool("includeDescendants").grp("filter").dflt(true).desc("Include descendant category items."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("items"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Items")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["serviceCategoriesCreate"] = bdb(
        "serviceCategoriesCreate", service_categories_create_block,
        "Create Category", "Service",
        "POST /api/categories.",
        J{{"kind","serviceCategoriesCreate"},{"auth","auto"},{"type","pages"},{"storeAs","category"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("name").in().req().deep().dflt("").desc("Category display name."),
                pd_str("slug").in().deep().dflt("").desc("Optional slug (derived from name when empty)."),
                pd_str("description").grp("meta").deep().dflt("").desc("Optional description."),
                pd_str("visibility").grp("meta").deep().dflt("").desc("public | unlisted | private."),
                pd_str("parentId").grp("meta").deep().dflt("").desc("Optional parent category UUID."),
                pd_str("relationType").grp("meta").deep().dflt("").desc("Relation type for the parent link."),
                pd_str("type").grp("meta").deep().dflt("pages").desc("Stored as meta.type."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("category"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Category")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["serviceCategoriesUpdate"] = bdb(
        "serviceCategoriesUpdate", service_categories_update_block,
        "Update Category", "Service",
        "PATCH /api/categories/{categoryId}.",
        J{{"kind","serviceCategoriesUpdate"},{"auth","auto"},{"type","pages"},{"storeAs","category"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("categoryId").in().req().deep().dflt("").desc("Category UUID."),
                pd_str("name").in().deep().dflt("").desc("Replacement name."),
                pd_str("slug").in().deep().dflt("").desc("Replacement slug."),
                pd_str("description").grp("meta").deep().dflt("").desc("Replacement description."),
                pd_str("visibility").grp("meta").deep().dflt("").desc("public | unlisted | private."),
                pd_str("type").grp("meta").deep().dflt("pages").desc("Stored as meta.type."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("category"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Category")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    registry["serviceCategoriesRemove"] = bdb(
        "serviceCategoriesRemove", service_categories_remove_block,
        "Remove Category", "Service",
        "DELETE /api/categories/{categoryId} after checking meta.type.",
        J{{"kind","serviceCategoriesRemove"},{"auth","auto"},{"type","pages"},{"storeAs","removeResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("categoryId").in().req().deep().dflt("").desc("Category UUID."),
                pd_str("type").grp("filter").deep().dflt("pages").desc("Expected meta.type (default: pages)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("removeResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Remove result")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true});

    // ── Search ────────────────────────────────────────────────────────────────
    registry["serviceSearch"] = bdb(
        "serviceSearch", service_search_block,
        "Search", "Service",
        "GET /api/search?q=...&type=...&limit=...  -  full-text search across pages, posts, "
        "pictures, VFS files and places. "
        "Auth is optional (unauthenticated calls see only public content). "
        "PREVIOUS = array of enriched FeedPost objects.",
        J{{"kind","serviceSearch"},{"auth","auto"},{"type","all"},{"limit",20},{"storeAs","results"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("q").in().req().deep().dflt("").desc("Search query. Supports ${var} interpolation."),
                pd_enum("type",{"all","pages","posts","pictures","files","places"}).grp("filter").dflt("all")
                    .desc("Content type to search (default: all)."),
                pd_int("limit").grp("filter").dflt(20).range(1, 50)
                    .desc("Max results (server cap: 50)."),
                pd_str("sizes").grp("filter").deep().dflt("")
                    .desc("Comma-separated image widths for responsive variants, e.g. \"320,640,1024\"."),
                pd_str("formats").grp("filter").deep().dflt("")
                    .desc("Comma-separated image formats, e.g. \"avif,webp\"."),
                pd_enum("visibilityFilter",{"","invisible","private"}).grp("filter").dflt("")
                    .desc("Visibility filter (requires auth). empty = no filter."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("results"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Results")
        .output("count", ParamKind::integer_, "Count")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

    registry["serviceFilesSearch"] = bdb(
        "serviceFilesSearch", service_files_search_block,
        "Search Files (VFS)", "Service",
        "GET /api/vfs/search/{mount}/{path}?q=...&type=...&maxResults=...&fts=1  -  "
        "search VFS files by filename walk (default) or PostgreSQL FTS index (fts=1). "
        "FTS only reflects the last index run; fresh uploads may not appear until the index is rebuilt. "
        "PREVIOUS = {results:[...], total:N, truncated:bool}.",
        J{{"kind","serviceFilesSearch"},{"auth","auto"},{"mount","home"},{"type","all"},
          {"maxResults",200},{"fts",0},{"storeAs","searchResult"}})
        .params([]{
            auto p = service_auth_params();
            p.insert(p.begin(), {
                pd_str("q").in().req().deep().dflt("").desc("Search query (min 2 chars). Supports ${var} interpolation."),
                pd_str("mount").in().deep().dflt("home").desc("VFS mount name (e.g. \"home\", \"shared\")."),
                pd_str("path").in().deep().dflt("").desc("Subpath to restrict search within (optional)."),
                pd_enum("type",{"all","file","dir"}).grp("filter").dflt("all")
                    .desc("Node type filter."),
                pd_int("maxResults").grp("filter").dflt(200).range(1, 500)
                    .desc("Max results (server cap: 500)."),
                pd_int("fts").grp("filter").dflt(0).range(0, 1)
                    .desc("1 = use PostgreSQL full-text index (fast, but stale for fresh uploads). 0 = live filesystem walk (slower, always current)."),
            });
            p.push_back(timeout_param());
            p.push_back(pd_store_as().dflt("searchResult"));
            return p;
        }())
        .primary_output(ParamKind::json_value, "Search result")
        .output("total", ParamKind::integer_, "Total")
        .output("statusCode", ParamKind::integer_, "Status code")
        .exec({false, true, true})
        .children({{"items", "Then", "body", {"*"}, 0, -1}});

#endif // FEATURE_SERVICE_XBLOX  -  registration guard
}

#endif // FEATURE_SERVICE_XBLOX  -  handler implementations

} // namespace media::xblox::blocks
