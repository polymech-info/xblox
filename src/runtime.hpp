#pragma once

#include "core/command_variables.hpp"
#include "video/creator_fx.hpp"
#include "video/filters/beauty_config.hpp"
#include "video/video_recording_status.hpp"
#include "types.hpp"
#include "xblox_constants.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace media::xblox {

struct ExecutionOutput {
    std::string slot;
    std::string export_name;
    std::string kind;
    nlohmann::json value;
    bool primary = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// RunScope  -  a run-scoped, type-erased resource cache.
//
// Lives for exactly one run: a single pass, or a whole persisting `--loop` (the
// PassEngine owns one across passes). Block handlers use it to cache expensive,
// reusable resources  -  a loaded ONNX session, a live overlay window  -  keyed by an
// identity string, and reuse them on every subsequent pass instead of rebuilding
// per frame. Everything is released when the RunScope is destroyed (loop
// cancelled, run finished, or `xblox session stop`), each resource via its own
// destructor. Mutex-guarded so background blocks can share it safely.
// ─────────────────────────────────────────────────────────────────────────────
class RunScope {
public:
    RunScope() = default;
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;

    // Return the resource cached under `key`, constructing it via `make` (which
    // must return std::shared_ptr<T> / std::unique_ptr<T>) on first use.
    // Subsequent calls with the same key reuse the same instance; T must match
    // across calls for a given key.
    template <class T, class Make>
    T& acquire(const std::string& key, Make&& make) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = resources_.find(key);
        if (it == resources_.end()) {
            std::shared_ptr<T> sp = std::forward<Make>(make)();
            it = resources_.emplace(key, std::shared_ptr<void>(std::move(sp))).first;
        }
        return *static_cast<T*>(it->second.get());
    }

    template <class T>
    T* find(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = resources_.find(key);
        return it == resources_.end() ? nullptr : static_cast<T*>(it->second.get());
    }

    void release(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx_);
        resources_.erase(key);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        resources_.clear();
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<void>> resources_;
};

struct ExecutionEvent {
    std::string path;
    std::string kind;
    std::string status;
    std::string message;
    int exit_code = 0;
    nlohmann::json data = nlohmann::json::object();
    std::string block_id;
    std::string type;
    std::vector<std::string> stdout_lines;
    std::vector<std::string> stderr_lines;
    std::vector<ExecutionOutput> outputs;
    int error_code = 0;
};

struct CommandInvocation {
    std::string path;
    std::string id;
    std::string label;
    std::string action;
    nlohmann::json payload = nlohmann::json::object();
    nlohmann::json command = nlohmann::json::object();
    std::vector<std::string> extra_args;
};

using EventSink = std::function<void(const ExecutionEvent&)>;
using CommandHandler = std::function<int(const CommandInvocation&, std::string* message)>;
using ConsoleCommandHandler = std::function<int(const std::string& line, bool new_shell, bool close_on_exit, std::string* message)>;
using CancelPredicate = std::function<bool()>;

// Lifecycle bridge for the shared command hub. The runtime stays portable and
// host-agnostic: it only reports phase transitions for the process-backed
// commands it runs itself (cli/external), keyed by the custom command id. The
// host wires this to media::command_control so other surfaces (ribbon, launcher,
// page buttons) reflect commands started from xblox. Empty by default.
enum class CommandLifecyclePhase { Started, Exited, Failed };
using CommandLifecycleSink = std::function<void(const std::string& commandId, CommandLifecyclePhase phase)>;

// Per-thread nested cancellation used by controller blocks that run child lists
// inside a sub-operation. The executor's normal `cancel_requested(options)` path
// honors this in addition to the run-wide cancel predicate.
void push_thread_cancel_override(CancelPredicate predicate);
void pop_thread_cancel_override();
bool thread_cancel_requested();

struct VideoRecorderRequest {
    std::string action;       // start | update | status | stop
    std::string instance;     // host-visible instance key
    std::string input;        // empty/default camera, camera name, or screen:* spec
    std::filesystem::path output_path;
    int width = 0;
    int height = 0;
    int fps = 30;
    int bitrate_kbps = 0;        // 0 = automatic
    std::string capture_engine;  // auto | fast | compat
    std::string encoder;         // auto | nvenc | mediaFoundation
    bool include_cursor = true;
    std::string size_mode;       // native | pad | fit | fill
    /// Output/viewport width÷height. 0 = use source / width×height as given.
    /// When set, zoom/follow keep this ratio in the GPU crop.
    double aspect_ratio = 0.0;
    double zoom = 1.0;
    std::string follow;          // none | anchor | cursor
    double follow_speed = 0.2;
    int follow_deadzone = 3;
    int transition_ms = 300;
    int update_interval_ms = 16;
    double focus_x = -1.0;
    double focus_y = -1.0;
    bool interactive_zoom = false;
    double zoom_step = 0.15;
    double zoom_min = 1.0;
    double zoom_max = 4.0;
    bool consume_zoom_input = true;
    bool show_zoom_status = true;
    std::string hud_capture = "auto";
    /// off | overlay | taskbar | both. Mirrors videoCapture hud + hudMode.
    std::string status_hud = "off";
    std::string pause_key;
    std::string stop_key;
    // Keystroke OSD (reusable key_overlay param bundle).
    bool key_overlay = false;
    bool key_overlay_burn_in = false;
    std::string key_overlay_align = "bottom";
    int key_overlay_margin_x = 0;
    int key_overlay_margin_y = 0;
    int key_overlay_font_size = 0;
    std::string key_overlay_color = "#F8F9FB";
    std::string key_overlay_bg_color = "#181B21";
    std::string key_overlay_accent_color = "#EB445A";
    float key_overlay_opacity = 0.9f;
    int key_overlay_hold_ms = 1200;
    int key_overlay_fade_ms = 450;
    int key_overlay_max_entries = 5;
    std::string key_overlay_filter = "all";
    std::string key_overlay_stack = "auto";
    bool key_overlay_modifiers = true;
    std::string key_overlay_capture = "visible";
    // Creator FX (docs/video/video-next.md) — option schemas; runtime reserved.
    bool cursor_highlight = false;
    std::string cursor_highlight_color = "#F5C542";
    int cursor_highlight_size = 0;
    bool cursor_clicks = false;
    std::string cursor_click_color = "#EB445A";
    bool cursor_trail = false;
    bool hide_cursor_when_typing = false;
    bool camera_overlay = false;
    std::string camera_input;
    std::string camera_position = "bottom-right";
    float camera_size = 22.0f;
    std::string camera_shape = "circle";
    bool camera_border = true;
    std::string camera_border_color = "#FFFFFF";
    bool camera_shadow = true;
    bool camera_audio = false;
    float camera_audio_gain = 1.0f;
    std::string camera_resolution = "auto";
    pm::video::BeautyConfig camera_beauty{};
    bool audio_normalize = false;
    bool audio_auto_gain = false;
    std::string audio_filter;
    std::string audio_filter_model;
    float audio_filter_atten = 100.f;
    bool audio_filter_post = false;
    bool audio_filter_compensate_delay = true;
    pm::video::IdlePauseConfig idle_pause{};
    pm::video::CaptionConfig captions{};
    // Audio mux (mic / desktop / mix). Empty audioSource = none.
    std::string audio_source;
    std::string audio_device;     // mic device (substring)
    std::string desktop_device;   // loopback device (substring)
    float mic_gain = 1.0f;
    float desktop_gain = 1.0f;
};

struct VideoRecorderResponse {
    std::string message;
    pm::video::RecordingStatus status;
};

using VideoRecorderHandler =
    std::function<bool(const VideoRecorderRequest&, VideoRecorderResponse* response)>;

// ─────────────────────────────────────────────────────────────────────────────
// Resolve hooks  -  app-level policy injected at xblox initiation. The executor's
// param resolver runs them for any ParamDef that declares ParamResolve::Custom,
// *after* variable/glob expansion and before the handler. A hook may rewrite the
// resolved value and/or veto it; a scalar veto becomes a fatal diagnostic so the
// block is skipped (predicted, not run)  -  the same route a failed precondition
// takes. For a glob-expanded list, a veto filters that entry (non-fatal) so the
// surviving matches still flow through.
//
// Value is passed as a string (the path/string use-case) to keep this layer free
// of the blocks Value/ParamDef types. Hooks are synchronous predicates; the
// host-mediated / async capability gate (ParamConstraint::RequiresPermission) is
// a separate, later layer that can await a host RPC.
// ─────────────────────────────────────────────────────────────────────────────

struct ResolveHookInput {
    std::string block_kind;        // e.g. "fsWrite"
    std::string param;             // param name, e.g. "path"
    std::string param_kind;        // ParamKind token, e.g. "file_path"
    std::string value;             // resolved value (string form)
    bool        writable = false;  // param declares Writable / CreateParents
    bool        simulate = false;
};

struct ResolveHookOutput {
    bool        deny = false;      // veto this value
    int         severity = 2;      // 2 = fatal veto (scalar); 1 = advisory
    std::string message;           // diagnostic text (e.g. deny reason)
    bool        rewrite = false;   // replace the value with `value` below
    std::string value;             // replacement value when rewrite == true
};

using ResolveHook = std::function<ResolveHookOutput(const ResolveHookInput&)>;

// Scope/state handling between loop passes. Persist (default) reuses one scope
// session so variables carry across passes (n climbs 1,2,3...) and NH_Context is
// built once. Reset rebuilds a fresh scope each pass (clean restart). Kept as an
// enum so future modes (e.g. Snapshot) can slot in.
enum class LoopScopePolicy {
    Persist = 0,
    Reset   = 1,
};

struct ExecutionOptions {
    nlohmann::json custom_commands = nlohmann::json::object();
    std::filesystem::path cli_executable_path;
    std::filesystem::path source_path;
    std::filesystem::path default_cwd;
    media::commands::VariableContext command_context;
    std::vector<std::string> process_args;
    std::string process_command_line;
    std::vector<std::string> extra_args;
    bool execute_external_commands = true;
    bool wait_blocks = true;
    bool collect_events = true;
    // Retain only error events while keeping the successful-event hot paths
    // muted. This preserves actionable CLI diagnostics without an event vector
    // that grows with every successful loop iteration.
    bool collect_error_events = false;
    // Simulation/emulation: resolve every block's inputs as usual, but suppress
    // side-effecting blocks (fs writes, shell, network, capture, external
    // commands). Pure dataflow/control blocks (setVariable, log, if/for/while...)
    // still run, so variable flow and predicted results stay intact.
    bool simulate = false;
    bool quiet    = false;  // suppress stdout block output (resolve + emit event, but skip std::cout)
    int max_loop_iterations = media::xblox::constants::kDefaultMaxLoopIterations;
    int default_timeout_ms = media::xblox::constants::kDefaultTimeoutMs;

    // Document-level loop: re-run the whole document until cancelled. Off by
    // default. The effective values are resolved in run_blocks_file as
    // CLI-override > document `loop` setting > default. The interval is the gap
    // between full passes, clamped to [0, kLoopIntervalMaxMs] (0 == "as fast as
    // possible, but yield a tick each pass" so the run stays cancellable and the
    // host message pump / web UI keeps breathing). These overrides let the CLI
    // (and host) force the loop on/off and set the interval regardless of what
    // the document stored.
    static constexpr long long kLoopIntervalMaxMs = media::xblox::constants::kLoopIntervalMaxMs;
    std::optional<bool> loop_override;                    // CLI --loop / --no-loop
    std::optional<long long> loop_interval_ms_override;   // CLI --loop-interval-ms
    std::optional<bool> loop_reset_override;              // CLI --loop-reset
    std::optional<int64_t> loop_limit_override;           // CLI --loop-limit (0 = unlimited)

    CommandHandler app_command_handler;
    CommandHandler ribbon_command_handler;
    CommandHandler open_url_handler;
    CommandHandler open_path_handler;
    ConsoleCommandHandler console_command_handler;
    VideoRecorderHandler video_recorder_handler;
    CancelPredicate cancel_requested;
    EventSink event_sink;

    // Host window that initiated this run (UI root HWND). Used by screen:current
    // capture specs. Opaque void* so this header stays free of windows.h; on
    // Win32 it is an HWND. CLI children also accept PM_IMAGE_HOST_HWND.
    void* host_hwnd = nullptr;

    // Optional bridge to the shared command hub. Invoked around the dispatch of
    // process-backed commands (cli/external) the runtime runs itself, so other
    // UI surfaces can reflect xblox-initiated runs. Delegated actions (app/
    // ribbon/url/path) are not reported here; their host targets own lifecycle.
    CommandLifecycleSink command_lifecycle;

    // App-level resolve policy (fs guards, permission gates, path redirection),
    // consulted by the resolver for ParamResolve::Custom params. Registered at
    // xblox initiation; empty by default so the runtime stays self-contained.
    std::vector<ResolveHook> resolve_hooks;

    // Loop variable bridge (session bus). In a persisting loop the executor, at
    // each pass boundary, pulls externally-issued variable sets (applied before
    // the pass) and publishes a snapshot of the run's variables (after the pass)
    // so other processes can get/set scope across the control socket. Empty by
    // default; only a looping run that hosts a session wires these.
    std::function<std::vector<std::pair<std::string, nlohmann::json>>()> pull_external_vars;
    std::function<void(const nlohmann::json& context)> publish_vars;
};

// ─────────────────────────────────────────────────────────────────────────────
// BlockRunFlag  -  per-invocation execution flags as an extensible bitmask.
//
// Pure flags only; non-flag values (timeout) live in BlockRunSpec alongside.
// Resolved from the block JSON (enabled/abortOnError/background/...) and, in the
// new design, seeded from the descriptor's ExecFlags before dispatch.
// ─────────────────────────────────────────────────────────────────────────────

enum class BlockRunFlag : std::uint32_t {
    None          = 0,
    Enabled       = 1u << 0,  // block participates in execution
    AbortOnError  = 1u << 1,  // failure stops the sibling chain
    Background    = 1u << 2,  // fire-and-forget on a detached worker
    Cancellable   = 1u << 3,  // polls cancel_requested
    ConsumeEvents = 1u << 4,  // emit running/ok/error events
    Isolate       = 1u << 5,  // run children in a discarded child scope
    Collect       = 1u << 6,  // gather child-scope vars into the block result
    Singleton     = 1u << 7,  // at most one instance running at a time
    // room to grow: Retry, Timed, Detached, Trace, ...
};

constexpr BlockRunFlag operator|(BlockRunFlag a, BlockRunFlag b) {
    return static_cast<BlockRunFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr BlockRunFlag operator&(BlockRunFlag a, BlockRunFlag b) {
    return static_cast<BlockRunFlag>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr BlockRunFlag operator~(BlockRunFlag a) {
    return static_cast<BlockRunFlag>(~static_cast<std::uint32_t>(a));
}
constexpr BlockRunFlag& operator|=(BlockRunFlag& a, BlockRunFlag b) { a = a | b; return a; }
constexpr BlockRunFlag& operator&=(BlockRunFlag& a, BlockRunFlag b) { a = a & b; return a; }
constexpr bool has_flag(BlockRunFlag set, BlockRunFlag f) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0u;
}

constexpr BlockRunFlag kDefaultBlockRunFlags =
    BlockRunFlag::Enabled | BlockRunFlag::AbortOnError |
    BlockRunFlag::Cancellable | BlockRunFlag::ConsumeEvents;

// Scope exit policy for a container's child frame (mirrors Scope.h ScopeExit;
// kept as a plain enum here so block handlers need not include the NH headers).
enum class ScopePolicy : int {
    Inherit = 0, // merge locals up into parent
    Isolate = 1, // discard locals on exit
    Collect = 2, // gather locals into the block result
};

struct BlockRunSpec {
    BlockRunFlag flags = kDefaultBlockRunFlags;
    int timeout_ms = 0;

    bool enabled()       const { return has_flag(flags, BlockRunFlag::Enabled); }
    bool abort_on_error()const { return has_flag(flags, BlockRunFlag::AbortOnError); }
    bool background()    const { return has_flag(flags, BlockRunFlag::Background); }
    bool cancellable()   const { return has_flag(flags, BlockRunFlag::Cancellable); }
    bool consume_events()const { return has_flag(flags, BlockRunFlag::ConsumeEvents); }

    void set(BlockRunFlag f, bool on) { if (on) flags |= f; else flags &= ~f; }
};

struct ExecutionResult {
    bool ok = true;
    int exit_code = 0;
    // Set by the `exit` block: the run ended deliberately with `exit_code` as
    // the authoritative process exit code (0 = success). The executor must not
    // apply its generic failure bookkeeping on top, and a document loop stops.
    bool exit_requested = false;
    size_t event_count = 0;
    size_t error_count = 0;
    nlohmann::json last_block_result = nullptr;
    std::vector<ExecutionEvent> events;
};

} // namespace media::xblox
