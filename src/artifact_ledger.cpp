#include "artifact_ledger.hpp"

#include "core/command_variables.hpp"
#include "fs/path_sanitizer.hpp"
#include "core/settings_runtime.hpp"
#include "logger/logger.h"
#include "xblox/xblox_session.hpp"

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
#include "core/cmd_ipc.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "string/string_utils.hpp"

namespace fs = std::filesystem;

namespace media::xblox::artifacts {

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr std::size_t kMaxRunEntries = 5000;

constexpr std::int64_t kLiveNotifyCooldownMs = 1500;
constexpr int          kLiveNotifyBurstMax   = 64;
constexpr std::int64_t kLiveNotifyWindowMs   = 1000;

bool looks_like_http_artifact_url(const std::string& path)
{
    return path.size() >= 7
        && (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0);
}

bool is_artifact_path_kind(std::string_view kind)
{
    return kind == "file_path" || kind == "dir_path" || kind == "output_path"
        || kind == "image_path" || kind == "video_path" || kind == "audio_path"
        || kind == "url";
}

std::string artifact_op_for_event(const ExecutionEvent& event)
{
    if (event.data.is_object()) {
        if (event.data.value("append", false))
            return "append";
        if (event.data.value("created", false) || event.data.value("mkdir", false))
            return "create";
        if (event.data.value("deleted", false))
            return "delete";
    }
    const std::string& block = event.kind;
    if (block == "fsMkdir" || block == "fsEnsureDir")
        return "create_dir";
    if (block == "fsDelete" || block == "fsRemove")
        return "delete";
    return "write";
}

bool is_delete_fs_block(std::string_view kind)
{
    return kind == "fsDelete" || kind == "fsRemove";
}

bool is_llm_dst_block(std::string_view kind)
{
    return kind == "llm" || kind == "llmChat";
}

/// Blocks that emit paths but must never be treated as produced artifacts.
bool is_non_artifact_block(std::string_view kind)
{
    static constexpr const char* kDenied[] = {
        "fsRead",
        "fsHash",
        "fsList",
        "fsExists",
        "imageUnderstand",
        "imageAnalyze",
        "imageFind",
        "videoListDevices",
        "videoListScreens",
    };
    for (const char* denied : kDenied) {
        if (kind == denied)
            return true;
    }
    return false;
}

bool json_nonempty_path(const nlohmann::json& value, std::string& out)
{
    if (value.is_string()) {
        out = value.get<std::string>();
        return !out.empty();
    }
    if (value.is_object() && value.contains("path") && value["path"].is_string()) {
        out = value["path"].get<std::string>();
        return !out.empty();
    }
    return false;
}

std::wstring lowercase_ext(const std::wstring& ext)
{
    std::wstring out = ext;
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z')
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return out;
}

bool looks_like_filesystem_artifact_path(const std::string& path)
{
    if (path.empty())
        return false;
    if (path.rfind("mem://", 0) == 0)
        return false;
    if (path == "screen" || path.rfind("screen:", 0) == 0)
        return false;
    // Use the shared path token heuristic only — the old ".ext" suffix rule
    // matched free text (picker/LLM/OCR prose) and crashed during normalization.
    return media::path::looks_like_local_path(path);
}

std::optional<std::string> resolve_artifact_path(std::string_view raw,
                                                 const std::string& cwd,
                                                 const std::string& src,
                                                 bool require_exists)
{
    if (raw.empty())
        return std::nullopt;

    media::commands::VariableContext vctx;
    vctx.cwd         = cwd;
    vctx.source_file = src;
    media::commands::VariableMap vars = media::commands::make_variable_map(vctx);
    if (!src.empty())
        media::commands::apply_source_file_variables(vars, src, cwd);

    std::string resolved = media::commands::deep_resolve_variables(
        raw, vars, nullptr, media::commands::kMaxDeepVariablePasses);
    if (!media::path::looks_like_local_path(resolved))
        return std::nullopt;
    resolved = media::path::normalize_local_path(resolved, true);
    if (resolved.empty() || resolved.find("${") != std::string::npos)
        return std::nullopt;

    const fs::path p = media::path::path_from_utf8(resolved);
    if (!p.is_absolute())
        return std::nullopt;

    if (require_exists) {
        std::error_code ec;
        if (!fs::exists(p, ec) || ec)
            return std::nullopt;
    }

    return resolved;
}

bool prepare_artifact_path(std::string& path, const RunContext* ctx, bool require_exists)
{
    if (looks_like_http_artifact_url(path))
        return true;
    if (!looks_like_filesystem_artifact_path(path))
        return false;
    const std::string cwd = ctx ? ctx->cwd : std::string{};
    const std::string src = ctx ? ctx->src : std::string{};
    const auto resolved   = resolve_artifact_path(path, cwd, src, require_exists);
    if (!resolved)
        return false;
    path = *resolved;
    return true;
}

std::string infer_path_kind(const std::string& path)
{
    if (looks_like_http_artifact_url(path))
        return "url";
    const std::wstring ext = lowercase_ext(media::path::path_from_utf8(path).extension().wstring());
    if (ext == L".mp4" || ext == L".webm" || ext == L".mkv" || ext == L".mov" || ext == L".avi")
        return "video_path";
    if (ext == L".wav" || ext == L".mp3" || ext == L".flac" || ext == L".ogg" || ext == L".m4a")
        return "audio_path";
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" || ext == L".webp"
        || ext == L".bmp" || ext == L".tif" || ext == L".tiff" || ext == L".heic")
        return "image_path";
    return "file_path";
}

std::string path_kind_for_output(std::string_view output_kind, const std::string& path)
{
    if (is_artifact_path_kind(output_kind))
        return std::string(output_kind);
    return infer_path_kind(path);
}

void harvest_path_array(std::vector<nlohmann::json>& out,
                        const ExecutionEvent&       event,
                        const RunContext*           ctx,
                        const nlohmann::json&       arr,
                        std::unordered_set<std::string>& seen)
{
    if (!arr.is_array())
        return;
    for (const auto& el : arr) {
        std::string path;
        if (el.is_string()) {
            path = el.get<std::string>();
        } else if (el.is_object() && el.contains("path")) {
            if (!json_nonempty_path(el["path"], path))
                continue;
        } else {
            continue;
        }
        const bool require_exists = artifact_op_for_event(event) != "delete";
        if (!prepare_artifact_path(path, ctx, require_exists))
            continue;
        const std::string key = path_key(path);
        if (key.empty() || !seen.insert(key).second)
            continue;
        const std::string kind = infer_path_kind(path);
        out.push_back(make_record(event, std::move(path), kind, "output", ctx));
    }
}

bool data_field_has_artifact_path(const nlohmann::json& data, const char* key)
{
    if (!data.is_object() || !data.contains(key))
        return false;
    std::string path;
    return json_nonempty_path(data[key], path) && looks_like_filesystem_artifact_path(path);
}

bool event_has_harvestable_paths(const ExecutionEvent& event)
{
    if (event.data.is_object() && event.data.value("running", false))
        return false;

    for (const auto& output : event.outputs) {
        if (is_llm_dst_block(event.kind) && output.slot != "dst")
            continue;
        if (!is_artifact_path_kind(output.kind)
            && output.slot != "result" && output.slot != "path")
            continue;
        std::string path;
        if (!json_nonempty_path(output.value, path))
            continue;
        if (is_artifact_path_kind(output.kind) || looks_like_filesystem_artifact_path(path)
            || looks_like_http_artifact_url(path))
            return true;
    }

    if (!event.data.is_object())
        return false;

    for (const char* key : {"path", "outputPath", "recordPath", "output", "file", "dest",
                             "savedPath", "result"}) {
        if (data_field_has_artifact_path(event.data, key))
            return true;
    }

    if (event.data.contains("outputs") && event.data["outputs"].is_array()) {
        for (const auto& el : event.data["outputs"]) {
            std::string path;
            if (el.is_string()) {
                path = el.get<std::string>();
            } else if (el.is_object() && el.contains("path")) {
                if (!json_nonempty_path(el["path"], path))
                    continue;
            } else {
                continue;
            }
            if (looks_like_filesystem_artifact_path(path))
                return true;
        }
    }

    return false;
}

bool should_harvest_event(const ExecutionEvent& event)
{
    if (is_non_artifact_block(event.kind))
        return false;
    if (is_delete_fs_block(event.kind))
        return event.data.is_object() && event.data.value("deleted", false);
    return event_has_harvestable_paths(event);
}

bool is_media_output_block(std::string_view kind)
{
    return kind == "imageScreenshot" || kind == "videoCapture" || kind == "videoScreenCapture"
        || kind == "pictureOut" || kind == "videoWriter" || kind == "imageResize";
}

std::string path_key(std::string path)
{
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
#ifdef _WIN32
    path = media::strutil::lower_ascii(std::move(path));
#endif
    return path;
}

nlohmann::json make_record(const ExecutionEvent& event,
                           std::string           path,
                           std::string           path_kind,
                           std::string           slot,
                           const RunContext*     ctx)
{
    nlohmann::json row = nlohmann::json::object();
    row["path"]        = path;
    row["pathKind"]    = path_kind;
    row["slot"]        = slot;
    row["op"]          = artifact_op_for_event(event);
    row["block"]       = event.kind;
    row["blockKind"]   = event.kind;
    row["blockPath"]   = event.path;
    row["status"]      = event.status;
    row["ts"]          = now_ms();
    if (!event.block_id.empty())
        row["blockId"] = event.block_id;
    row["eventType"]   = !event.type.empty() ? event.type : event.kind;
    if (!event.message.empty())
        row["message"] = event.message;
    if (ctx) {
        if (!ctx->src.empty())
            row["src"] = ctx->src;
        if (!ctx->cwd.empty())
            row["cwd"] = ctx->cwd;
        if (!ctx->label.empty())
            row["label"] = ctx->label;
    }
    if (event.data.is_object()) {
        for (const char* key : {"size", "append", "exists", "mime", "width", "height",
                                 "durationMs", "format", "frames"}) {
            if (event.data.contains(key))
                row[key] = event.data[key];
        }
    }
    return row;
}

std::vector<nlohmann::json> harvest_from_event(const ExecutionEvent& event,
                                               const RunContext*       ctx)
{
    std::vector<nlohmann::json> out;
    if (!should_harvest_event(event))
        return out;

    std::unordered_set<std::string> seen;
    const bool require_exists = artifact_op_for_event(event) != "delete";
    const auto try_path = [&](std::string path, std::string path_kind, std::string slot) {
        if (path.empty())
            return;
        if (!prepare_artifact_path(path, ctx, require_exists))
            return;
        const std::string key = path_key(path);
        if (key.empty() || !seen.insert(key).second)
            return;
        out.push_back(make_record(event, std::move(path), std::move(path_kind), std::move(slot), ctx));
    };

    if (is_delete_fs_block(event.kind)) {
        std::string path;
        if (json_nonempty_path(event.data.value("path", nlohmann::json{}), path))
            try_path(std::move(path), "file_path", "path");
        return out;
    }

    for (const auto& output : event.outputs) {
        if (is_llm_dst_block(event.kind) && output.slot != "dst")
            continue;
        std::string path;
        if (!json_nonempty_path(output.value, path))
            continue;
        const bool typed_path = is_artifact_path_kind(output.kind);
        if (!typed_path && output.slot != "result" && output.slot != "path")
            continue;
        // Typed outputs (image_path, file_path, …) always harvest; legacy
        // untagged "result" strings must look like path tokens (not prose).
        if (!typed_path && !looks_like_filesystem_artifact_path(path)
            && !looks_like_http_artifact_url(path))
            continue;
        const std::string kind = path_kind_for_output(output.kind, path);
        try_path(std::move(path), kind, output.slot);
    }

    if (event.data.is_object()) {
        for (const char* key : {"path", "outputPath", "recordPath", "output", "file", "dest",
                                 "savedPath", "result"}) {
            if (!event.data.contains(key))
                continue;
            std::string path;
            if (!json_nonempty_path(event.data[key], path))
                continue;
            std::string kind = infer_path_kind(path);
            if (std::string_view(key) == "outputPath" || std::string_view(key) == "savedPath")
                kind = "output_path";
            try_path(std::move(path), std::move(kind), key);
        }

        if (event.data.contains("outputs"))
            harvest_path_array(out, event, ctx, event.data["outputs"], seen);
    }

    return out;
}

void ArtifactLedger::set_run_context(RunContext ctx)
{
    std::lock_guard<std::mutex> lock(mtx_);
    ctx_ = std::move(ctx);
}

void ArtifactLedger::ingest(const ExecutionEvent& event)
{
    if (event.status != "ok")
        return;

    RunContext ctx_copy;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ctx_copy = ctx_;
    }

    const auto rows = harvest_from_event(event, &ctx_copy);
    if (rows.empty()) {
        if (is_media_output_block(event.kind) || event_has_harvestable_paths(event)) {
            const std::string data_path = event.data.is_object()
                ? event.data.value("path", std::string{})
                : std::string{};
            logger::warn(std::string("[xblox-artifacts] ingest skipped harvest for ")
                          + event.kind + " status=ok outputs=" + std::to_string(event.outputs.size())
                          + " data.path=" + data_path
                          + (event.data.is_object() && event.data.value("running", false)
                                 ? " (running=true)" : ""));
        }
        return;
    }

    logger::debug(std::string("[xblox-artifacts] ingest ") + event.kind + " +"
                 + std::to_string(rows.size()) + " "
                 + rows.front().value("path", std::string{}));

    for (auto row : rows)
        record(std::move(row));
}

void ArtifactLedger::record(nlohmann::json entry)
{
    if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string())
        return;

    const std::string key = path_key(entry["path"].get<std::string>());
    if (key.empty())
        return;

    nlohmann::json notify_row;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = path_index_.find(key);
        if (it != path_index_.end()) {
            files_[it->second] = std::move(entry);
            notify_row           = files_[it->second];
        } else {
            if (files_.size() >= kMaxRunEntries) {
                ++suppressed_run_cap_;
                return;
            }
            path_index_[key] = files_.size();
            files_.push_back(std::move(entry));
            notify_row = files_.back();
        }
    }

    push_session_ipc(notify_row);
    notify_ui_instances(notify_row);
}

nlohmann::json ArtifactLedger::snapshot() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return files_;
}

std::size_t ArtifactLedger::count() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return files_.size();
}

std::size_t ArtifactLedger::suppressed_run_cap() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return suppressed_run_cap_;
}

std::size_t ArtifactLedger::suppressed_flood() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return suppressed_flood_;
}

bool ArtifactLedger::persist_to_settings(std::string& err, std::size_t max_entries) const
{
    err.clear();
    const nlohmann::json rows = snapshot();
    if (!rows.is_array() || rows.empty()) {
        logger::debug("[xblox-artifacts] persist skipped (run ledger empty)");
        return true;
    }

    nlohmann::json committable = nlohmann::json::array();
    for (const auto& row : rows) {
        if (!row.is_object() || !row.contains("path") || !row["path"].is_string())
            continue;
        std::string path = row["path"].get<std::string>();
        const std::string cwd = row.value("cwd", std::string{});
        const std::string src = row.value("src", std::string{});
        const bool require_exists = row.value("op", std::string{}) != "delete";
        const auto resolved = resolve_artifact_path(path, cwd, src, require_exists);
        if (!resolved)
            continue;
        nlohmann::json out = row;
        out["path"]        = *resolved;
        committable.push_back(std::move(out));
    }

    if (committable.empty()) {
        logger::debug("[xblox-artifacts] persist skipped (no committable paths)");
        return true;
    }

    if (!media::runtime_settings::append_xblox_artifact_entries(committable, err, max_entries)) {
        logger::error(std::string("[xblox-artifacts] persist failed: ") + err);
        return false;
    }

    return true;
}

bool ArtifactLedger::allow_live_notify_locked(const std::string& path_key, std::int64_t now_ms_val)
{
    if (path_key.empty())
        return false;

    if (window_start_ms_ == 0 || now_ms_val - window_start_ms_ >= kLiveNotifyWindowMs) {
        window_start_ms_      = now_ms_val;
        window_notify_count_  = 0;
    }
    if (window_notify_count_ >= kLiveNotifyBurstMax)
        return false;

    const auto it = last_live_notify_ms_.find(path_key);
    if (it != last_live_notify_ms_.end()
        && now_ms_val - it->second < kLiveNotifyCooldownMs)
        return false;

    last_live_notify_ms_[path_key] = now_ms_val;
    ++window_notify_count_;
    return true;
}

void ArtifactLedger::push_session_ipc(const nlohmann::json& entry)
{
    session::SessionServer* server = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        server = session_;
    }
    if (!server)
        return;

    server->record_artifact(entry);

    const std::string key = path_key(entry.value("path", std::string{}));
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!allow_live_notify_locked(key, now_ms()))
            return;
    }

    session::IpcMessage msg;
    msg.from     = "xblox";
    msg.message  = "artifact";
    msg.has_data = true;
    msg.data     = entry;
    server->push_message(msg);
}

void ArtifactLedger::notify_ui_instances(const nlohmann::json& entry)
{
#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    if (!ui_notify_ || !entry.is_object())
        return;

    const std::string key = path_key(entry.value("path", std::string{}));
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!allow_live_notify_locked(key, now_ms()))
            return;
    }

    for (const auto& inst : media::cmd_ipc::list_instances()) {
        if (inst.kind != media::cmd_ipc::ProcessKind::Ui)
            continue;
        nlohmann::json reply;
        std::string    err;
        (void)media::cmd_ipc::send_action(
            inst, "xblox_artifact", {{"artifact", entry}}, reply, err, 500);
    }
#endif
}

} // namespace media::xblox::artifacts
