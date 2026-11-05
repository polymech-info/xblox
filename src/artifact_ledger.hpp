#pragma once
// artifact_ledger — central run-scoped collection of file outputs from xblox blocks.
//
// Harvests typed path outputs from ExecutionEvent streams (no handler changes),
// optionally mirrors to SessionServer (ipcReceive inbox + action:"artifacts"),
// fans out to live UI instances via cmd_ipc (action:"xblox_artifact"), and
// persists an MRU ledger to settings.json on flush.
//
// Run ledger dedupes by normalized path (one row per path, refreshed on rewrite).
// Live session/UI fan-out is rate-limited to avoid notification floods.

#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace media::xblox::session {
class SessionServer;
}

namespace media::xblox::artifacts {

struct RunContext {
    std::string src;   // originating .xblox path (or inline label)
    std::string cwd;   // run working directory
    std::string label; // human label (often script basename)
};

/// Normalized path key for dedupe (trailing slashes stripped; case-folded on Windows).
std::string path_key(std::string path);

/// Extract file-artifact rows from one ok block event (empty when not applicable).
/// Output-driven: any typed path output or common data path fields, with a small
/// denylist for read-only blocks (fsRead, imageUnderstand, …) and llm dst-only filter.
std::vector<nlohmann::json> harvest_from_event(const ExecutionEvent& event,
                                               const RunContext*       ctx = nullptr);

/// Build one ledger row from a block event + resolved path metadata.
nlohmann::json make_record(const ExecutionEvent& event,
                           std::string           path,
                           std::string           path_kind,
                           std::string           slot,
                           const RunContext*     ctx = nullptr);

class ArtifactLedger {
public:
    void set_run_context(RunContext ctx);
    void set_session_sink(session::SessionServer* server) noexcept { session_ = server; }

    /// When true, each newly-notified row is pushed to live UI cmd_ipc instances
    /// (action:"xblox_artifact"), subject to flood limits. Slice-2 hosts handle display.
    void set_ui_notify(bool enabled) noexcept { ui_notify_ = enabled; }

    /// Harvest + record (+ optional session / UI fan-out).
    void ingest(const ExecutionEvent& event);

    void record(nlohmann::json entry);

    nlohmann::json snapshot() const;
    std::size_t    count() const;

    /// Rows suppressed because the run ledger cap was reached.
    std::size_t suppressed_run_cap() const;

    /// Live notify attempts suppressed by per-path cooldown or global burst limits.
    std::size_t suppressed_flood() const;

    /// Merge this run's rows into the persisted settings MRU (dedupe by path, cap 30).
    bool persist_to_settings(std::string& err, std::size_t max_entries = 30) const;

private:
    bool allow_live_notify_locked(const std::string& path_key, std::int64_t now_ms);
    void push_session_ipc(const nlohmann::json& entry);
    void notify_ui_instances(const nlohmann::json& entry);

    mutable std::mutex mtx_;
    nlohmann::json     files_ = nlohmann::json::array();
    RunContext         ctx_;
    session::SessionServer* session_ = nullptr;
    bool               ui_notify_    = false;

    // One row per normalized path for this run (path_key → index in files_).
    std::unordered_map<std::string, std::size_t> path_index_;
    std::size_t suppressed_run_cap_ = 0;
    std::size_t suppressed_flood_   = 0;

    // Live fan-out flood protection (session inbox + UI cmd_ipc).
    std::int64_t window_start_ms_ = 0;
    int          window_notify_count_ = 0;
    std::unordered_map<std::string, std::int64_t> last_live_notify_ms_;
};

} // namespace media::xblox::artifacts
