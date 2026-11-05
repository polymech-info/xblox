// XBlox input blocks  -  keyWait (keyboard trigger / listener).
//
// Win32 only: key polling via GetAsyncKeyState; shortcut string parsing via
// media::hotkeys::parse_shortcut (same format as ShortcutInput.tsx).
//
// keyWait contract:
//   - `keys`      : shortcut string, e.g. "F9" or "Ctrl+Shift+R".
//   - `on`        : "pressed" (leading edge), "held" (while down),
//                   "released" (trailing edge), "toggle" (start/stop).
//   - `repeat`    : bool  -  keep listening after the first event.
//   - `anyKey`    : bool  -  match any key; stores the key name in PREVIOUS.
//   - `timeoutMs` : 0 = wait forever.
//   - `storeAs`   : variable name for the fired key string.
//   - `items`     : child blocks run on each event (PREVIOUS = key name).
//
// Toggle mode:
//   Children run on a background thread with a private cancel token so the
//   main polling loop can cancel them on the second keypress while leaving
//   the keyWait block itself alive. The parent BlockRuntime is not modified;
//   we replace cancel_requested in a value-copy of it and pass that copy to
//   the child BlockIO. Both threads share the same context/result references
//   which is safe because the main thread only sleeps + polls key state and
//   never touches context during the child run.

#include "blocks/input_blocks.hpp"

#include "blocks/block_params.hpp"
#include "blocks/block_registry.hpp"
#include "logger/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include "core/hotkeys.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace media::xblox::blocks {

#if defined(_WIN32)

namespace {

constexpr long long kPollMs = 50;

// ── Thread-local cancel plumbing for toggle mode ──────────────────────────────
// toggle mode runs `items` children on a background thread with a private cancel
// signal. We replace BlockRuntime::cancel_requested with a raw function pointer
// (required  -  it cannot be a capturing lambda). The function reads per-thread
// state set before the child thread calls execute_block_list.

thread_local std::atomic<bool>*              tl_child_stop    = nullptr;
thread_local BlockRuntime::CancelRequestedFn tl_parent_cancel = nullptr;
thread_local void*                           tl_parent_ud     = nullptr;

static bool child_cancel_fn(void* ud)
{
    if (tl_child_stop && tl_child_stop->load(std::memory_order_relaxed))
        return true;
    if (tl_parent_cancel)
        return tl_parent_cancel(tl_parent_ud);
    (void)ud;
    return false;
}

std::once_flag g_stdin_once;
std::mutex g_stdin_mu;
std::deque<std::string> g_stdin_keys;

std::string trim_ascii(std::string s)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

std::string upper_ascii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string shortcut_match_key(std::string s)
{
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), s.end());
    return upper_ascii(std::move(s));
}

bool stdin_controller_enabled()
{
    const char* v = std::getenv("PM_XBLOX_KEYWAIT_STDIN");
    if (!v || !*v) return false;
    const std::string s = upper_ascii(trim_ascii(v));
    return s != "0" && s != "FALSE" && s != "NO" && s != "OFF";
}

void ensure_stdin_controller()
{
    if (!stdin_controller_enabled()) return;
    std::call_once(g_stdin_once, [] {
        std::thread([] {
            logger::trace("[keyWait] stdin controller enabled");
            std::string line;
            while (std::getline(std::cin, line)) {
                line = trim_ascii(std::move(line));
                if (line.empty()) continue;
                std::lock_guard<std::mutex> lk(g_stdin_mu);
                g_stdin_keys.push_back(std::move(line));
            }
            logger::trace("[keyWait] stdin controller EOF");
        }).detach();
    });
}

bool consume_stdin_shortcut(const std::string& keys, bool any_key, std::string& out_key)
{
    ensure_stdin_controller();
    if (!stdin_controller_enabled()) return false;

    const std::string want = shortcut_match_key(keys);
    std::lock_guard<std::mutex> lk(g_stdin_mu);
    while (!g_stdin_keys.empty()) {
        std::string key = std::move(g_stdin_keys.front());
        g_stdin_keys.pop_front();
        if (any_key || shortcut_match_key(key) == want) {
            out_key = std::move(key);
            return true;
        }
        logger::trace("[keyWait] stdin ignored key=" + key + " want=" + keys);
    }
    return false;
}

struct KeySampleState {
    bool down = false;
    bool toggled = false;
};

std::mutex g_key_state_mu;
std::unordered_map<std::string, KeySampleState> g_key_states;

BlockResult key_event_block(BlockIO& io)
{
    const std::string keys_str = io.str("keys");
    const bool any_key = io.flag("anyKey", false);
    const bool toggle = io.flag("toggle", true);
    const std::string state_key = io.path() + "|" + keys_str + "|" + (any_key ? "any" : "one");

    unsigned mods = 0, vk = 0;
    if (!any_key && !keys_str.empty()) {
        if (!media::hotkeys::parse_shortcut(keys_str, mods, vk)) {
            io.error("keyEvent", "keyEvent: invalid shortcut: " + keys_str, 2);
            return BlockResult::Error;
        }
    }

    std::string stdin_key;
    const bool stdin_down = consume_stdin_shortcut(keys_str, any_key, stdin_key);
    auto physical_down = [&]() -> bool {
        if (stdin_down) return true;
        if (any_key) {
            for (int k = 8; k < 256; ++k) {
                if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
                if (GetAsyncKeyState(k) & 0x8000) return true;
            }
            return false;
        }
        return media::hotkeys::shortcut_down(mods, vk);
    };

    auto physical_key_name = [&]() -> std::string {
        if (stdin_down) return stdin_key;
        if (!any_key) return keys_str;
        for (int k = 8; k < 256; ++k) {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
            if (!(GetAsyncKeyState(k) & 0x8000)) continue;
            char buf[64] = {};
            const UINT sc = MapVirtualKeyW(static_cast<UINT>(k), MAPVK_VK_TO_VSC);
            GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof(buf));
            return buf[0] ? std::string(buf) : std::to_string(k);
        }
        return {};
    };

    const bool down = physical_down();
    bool pressed = false;
    bool released = false;
    bool toggled = false;
    {
        std::lock_guard<std::mutex> lk(g_key_state_mu);
        KeySampleState& state = g_key_states[state_key];
        pressed = down && !state.down;
        released = !down && state.down;
        if (toggle && pressed)
            state.toggled = !state.toggled;
        toggled = state.toggled;
        state.down = stdin_down ? false : down;
    }

    const std::string key = (pressed || down || released) ? physical_key_name() : keys_str;
    nlohmann::json event = {
        {"key", key},
        {"down", down},
        {"pressed", pressed},
        {"released", released},
        {"toggle", toggled},
    };
    logger::trace("[keyEvent] key=" + key +
                  " down=" + std::to_string(down) +
                  " pressed=" + std::to_string(pressed) +
                  " released=" + std::to_string(released) +
                  " toggle=" + std::to_string(toggled));

    auto ev = io.ok("keyEvent", "keyEvent " + key +
                    " down=" + std::to_string(down) +
                    " pressed=" + std::to_string(pressed) +
                    " released=" + std::to_string(released) +
                    " toggle=" + std::to_string(toggled));
    const std::string store = io.str("storeAs");
    const bool export_state = !store.empty() && store != "PREVIOUS";
    const std::string export_prefix = export_state ? store + "." : std::string{};
    ev.output("key", ParamKind::shortcut_, Value{key},
              export_state ? export_prefix + "key" : std::string{})
      .output("down", ParamKind::boolean_, Value{down},
              export_state ? export_prefix + "down" : std::string{})
      .output("pressed", ParamKind::boolean_, Value{pressed},
              export_state ? export_prefix + "pressed" : std::string{})
      .output("released", ParamKind::boolean_, Value{released},
              export_state ? export_prefix + "released" : std::string{})
      .output("toggle", ParamKind::boolean_, Value{toggled},
              export_state ? export_prefix + "toggle" : std::string{})
      .result(Value::from_json(std::move(event)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ─────────────────────────────────────────────────────────────────────────────

BlockResult key_wait_block(BlockIO& io)
{
    const std::string keys_str   = io.str("keys");
    const std::string on_str     = io.str("on",    "pressed");
    const bool        repeat     = io.flag("repeat",  false);
    const bool        any_key    = io.flag("anyKey",  false);
    const long long   timeout_ms = io.i64("timeoutMs", 0);

    unsigned mods = 0, vk = 0;
    if (!any_key && !keys_str.empty()) {
        if (!media::hotkeys::parse_shortcut(keys_str, mods, vk)) {
            io.error("keyWait", "keyWait: invalid shortcut: " + keys_str, 2);
            return BlockResult::Error;
        }
    }

    using clk = std::chrono::steady_clock;
    const auto deadline = timeout_ms > 0
        ? clk::now() + std::chrono::milliseconds(timeout_ms)
        : clk::time_point::max();

    auto timed_out = [&] {
        return timeout_ms > 0 && clk::now() >= deadline;
    };

    // Returns true when the configured key combo is physically down.
    auto key_is_down = [&]() -> bool {
        if (any_key) {
            for (int k = 8; k < 256; ++k) {
                if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
                if (GetAsyncKeyState(k) & 0x8000) return true;
            }
            return false;
        }
        return media::hotkeys::shortcut_down(mods, vk);
    };

    // Returns a human-readable name for the currently pressed key (anyKey mode).
    auto current_key_name = [&]() -> std::string {
        if (!any_key) return keys_str;
        for (int k = 8; k < 256; ++k) {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON) continue;
            if (!(GetAsyncKeyState(k) & 0x8000)) continue;
            char buf[64] = {};
            const UINT sc = MapVirtualKeyW(static_cast<UINT>(k), MAPVK_VK_TO_VSC);
            GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof(buf));
            return buf[0] ? std::string(buf) : std::to_string(k);
        }
        return {};
    };

    // ── toggle mode ───────────────────────────────────────────────────────────
    // First press -> spawn children with a private cancel token.
    // Second press -> set child_stop -> children see io.cancelled() -> clean exit.
    if (on_str == "toggle") {
        std::atomic<bool> child_stop{false};
        std::atomic<bool> child_running{false};
        std::thread       child_thread;
        std::string       result_key;

        BlockRuntime& orig_rt = io.runtime();

        auto launch_children = [&](const std::string& key_name) {
            if (child_running.load(std::memory_order_acquire)) return; // already running
            if (child_thread.joinable())
                child_thread.join(); // reap a completed previous cycle before reusing std::thread
            if (child_running.exchange(true)) return; // defensive: already running
            child_stop.store(false);
            result_key = key_name;
            logger::trace("[keyWait] toggle start key=" + key_name + " path=" + io.path());

            child_thread = std::thread([&io, &orig_rt, &child_stop, &child_running, key_name]() {
                struct Cleanup {
                    std::atomic<bool>& running;
                    ~Cleanup() {
                        tl_child_stop    = nullptr;
                        tl_parent_cancel = nullptr;
                        tl_parent_ud     = nullptr;
                        media::xblox::pop_thread_cancel_override();
                        running.store(false, std::memory_order_release);
                    }
                } cleanup{child_running};

                tl_child_stop    = &child_stop;
                tl_parent_cancel = orig_rt.cancel_requested;
                tl_parent_ud     = orig_rt.user_data;
                media::xblox::push_thread_cancel_override([&child_stop, &orig_rt]() {
                    if (child_stop.load(std::memory_order_relaxed))
                        return true;
                    return orig_rt.cancel_requested && orig_rt.cancel_requested(orig_rt.user_data);
                });

                BlockRuntime child_rt     = orig_rt;
                child_rt.cancel_requested = child_cancel_fn;

                BlockIO child_io{io.raw(), io.path(), child_rt};
                logger::trace("[keyWait] toggle child run begin key=" + key_name);
                child_io.run("items");
                logger::trace("[keyWait] toggle child run end key=" + key_name);
            });
        };

        auto stop_children = [&]() {
            logger::trace("[keyWait] toggle stop requested path=" + io.path() +
                          " running=" + std::to_string(child_running.load(std::memory_order_acquire)));
            child_stop.store(true, std::memory_order_release);
            if (child_thread.joinable())
                child_thread.join();
            child_running.store(false, std::memory_order_release);
            logger::trace("[keyWait] toggle stop joined path=" + io.path());
        };

        bool prev_down = false;
        while (!io.cancelled() && !timed_out()) {
            std::string stdin_key;
            const bool stdin_down = consume_stdin_shortcut(keys_str, any_key, stdin_key);
            const bool down = stdin_down || key_is_down();
            if (stdin_down || (down && !prev_down)) {
                if (!child_running.load(std::memory_order_acquire)) {
                    launch_children(stdin_down ? stdin_key : current_key_name());
                } else {
                    stop_children();
                    // repeat=false -> one start/stop cycle then exit
                    if (!repeat) break;
                }
            }
            prev_down = stdin_down ? false : down;
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
        stop_children(); // ensure clean exit on cancel/timeout

        const std::string key = result_key.empty() ? keys_str : result_key;
        io.ok("keyWait", "keyWait toggle done: " + key)
          .set("key",    Value{key})
          .set("on",     Value{on_str})
          .result(key, ParamKind::shortcut_);
        return BlockResult::Ok;
    }

    // ── pressed / held / released ─────────────────────────────────────────────
    bool        prev_down     = false;
    std::string last_key_name;  // captured on leading edge for released events
    std::string fired_key;

    auto fire = [&](const std::string& key_name) {
        fired_key = key_name;
        if (!key_name.empty())
            io.runtime().set_context_value(io.runtime().user_data, "PREVIOUS",
                                           nlohmann::json(key_name));
        if (io.has_children("items"))
            io.run("items");
    };

    while (!io.cancelled() && !timed_out()) {
        std::string stdin_key;
        const bool stdin_down   = consume_stdin_shortcut(keys_str, any_key, stdin_key);
        const bool down         = stdin_down || key_is_down();
        const bool edge_press   =  stdin_down || (down && !prev_down);
        const bool edge_release = !down &&  prev_down;

        if (edge_press)
            last_key_name = stdin_down ? stdin_key : current_key_name();

        if (on_str == "pressed" && edge_press) {
            fire(last_key_name);
            if (!repeat) break;
        } else if (on_str == "released" && edge_release) {
            fire(last_key_name);
            if (!repeat) break;
        } else if (on_str == "held" && down) {
            fire(last_key_name);
            if (!repeat) break;
        }

        prev_down = stdin_down ? false : down;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }

    const std::string key = fired_key.empty() ? keys_str : fired_key;
    io.ok("keyWait", "keyWait (" + on_str + "): " + key)
      .set("key", Value{key})
      .set("on",  Value{on_str})
      .result(key, ParamKind::shortcut_);
    return BlockResult::Ok;
}

} // namespace

void register_input_blocks(BlockRegistry& registry)
{
    registry["keyEvent"] = bdb(
        "keyEvent", key_event_block,
        "Key Event", "Input",
        "Sample a keyboard shortcut once and emit down/pressed/released/toggle state. "
        "Designed for document loops: run it each pass, then branch with if blocks.",
        {{"kind","keyEvent"}, {"keys","F9"}, {"anyKey",false}, {"toggle",true}, {"storeAs","key"}})
        .params({
            pd_shortcut("keys").in().lbl("Key").dflt("F9")
                .desc("Keyboard shortcut to sample, e.g. F9 or Ctrl+Shift+R. Ignored when anyKey is on."),
            pd_bool("anyKey").grp("options").dflt(false)
                .desc("Match any key press instead of the configured shortcut."),
            pd_bool("toggle").grp("options").dflt(true)
                .desc("Flip the emitted toggle state on each press edge."),
            pd_store_as().dflt("key"),
        })
        .primary_output(ParamKind::json_value, "Key event")
        .output_from_param("key", ParamKind::shortcut_, "Key", "storeAs", ".key")
        .output_from_param("down", ParamKind::boolean_, "Down", "storeAs", ".down")
        .output_from_param("pressed", ParamKind::boolean_, "Pressed", "storeAs", ".pressed")
        .output_from_param("released", ParamKind::boolean_, "Released", "storeAs", ".released")
        .output_from_param("toggle", ParamKind::boolean_, "Toggle", "storeAs", ".toggle")
        .pure();

    registry["keyWait"] = bdb(
        "keyWait", key_wait_block,
        "Key Wait", "Input",
        "Wait for a keyboard shortcut then run child blocks. "
        "on=pressed fires on key-down edge; on=released fires on key-up edge; "
        "on=held fires while the key is held; on=toggle starts children on the "
        "first press and cancels them on the second. "
        "repeat=true keeps listening across multiple events. "
        "Stores the fired key string in PREVIOUS / storeAs when the wait cycle completes.",
        {{"kind","keyWait"}, {"keys","F9"}, {"on","pressed"}, {"repeat",false},
         {"anyKey",false}, {"timeoutMs",0}, {"storeAs",""}})
        .params({
            pd_shortcut("keys").in().lbl("Key").dflt("F9")
                .desc("Keyboard shortcut to listen for, e.g. F9 or Ctrl+Shift+R. "
                      "Ignored when anyKey is on."),
            pd_enum("on", {"pressed","held","released","toggle"})
                .in().lbl("On").dflt("pressed")
                .desc("When to fire: pressed (leading edge), held (while down), "
                      "released (trailing edge), or toggle (first press starts child "
                      "blocks, second press cancels them)."),
            pd_bool("repeat").grp("options").dflt(false)
                .desc("Keep listening and fire on every matching event. "
                      "Not applicable to toggle (which always re-arms after each cycle)."),
            pd_bool("anyKey").grp("options").dflt(false)
                .desc("Match any key press instead of the configured shortcut. "
                      "Stores the key name in PREVIOUS / storeAs."),
            pd_ms("timeoutMs").adv().grp("options").lbl("Timeout").dflt(0)
                .desc("Stop listening after this many milliseconds. 0 = wait forever."),
            pd_store_as(),
        })
        .primary_output(ParamKind::shortcut_, "Fired key")
        .children({{"items", "Then", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});
}

#else

void register_input_blocks(BlockRegistry& registry)
{
    (void)registry;
    // Keyboard input blocks require Win32 (GetAsyncKeyState).
}

#endif // _WIN32

} // namespace media::xblox::blocks
