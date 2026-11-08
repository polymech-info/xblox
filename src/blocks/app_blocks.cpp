// XBlox "App" block family  -  desktop automation.
//
// Ports every `app_*` computer-use tool from src/llm/tools/computer_use into
// the visual block runtime. Migrated to the native handler surface (no legacy
// adapter): each handler is a BlockResult(BlockIO&).
//   1. Resolves a target window from { hwnd | pid | title | process | foreground }
//      (most actions auto-activate the window first, matching the agent path).
//   2. Calls the same underlying `media::assistant::app_use|app_inspect|app_batch`
//      libraries the agent uses  -  there is intentionally no behavioural drift.
//   3. Publishes a typed envelope via io.ok(...).payload({...}).result(value) so
//      fixtures can assert on `event.data.*` exactly as before, and failures
//      return BlockResult::Error so the executor's error policy applies.
//
// App actions take many optional positional/nested fields (coords, arc/path,
// batch steps); those structured inputs are read straight off io.raw()  -  the
// same explicit JSON boundary the data/parse blocks use  -  while outputs go
// through BlockIO.
//
// Win32-only. On other platforms (or when FEATURE_ASSISTANT is off) the registry
// stays empty and the handlers are no-ops.

#include "blocks/app_blocks.hpp"
#include "blocks/block_variables.hpp"

#include "logging/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <future>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/host_backends.hpp"
#include "inspect/inspect.hpp"
#include "win/chrome_inspect/bridge.hpp"
#include "win/chrome_inspect/picker_enrich.hpp"
#include "video/video_picker_input_spec.hpp"
#include "win/window_bounds.hpp"
#include "win/context/context_store.hpp"
#include <shlobj.h>

#endif

namespace media::xblox::blocks {
namespace {

// ── small JSON accessors (lenient, never throw) ────────────────────────────
// App handlers read their many optional positional / nested fields directly off
// the raw block (io.raw()); these helpers keep that lenient and exception-free.
std::string jstr(const nlohmann::json& o, const char* key, std::string fallback = {})
{
    return o.is_object() && o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::move(fallback);
}

int jint(const nlohmann::json& o, const char* key, int fallback = 0)
{
    if (!o.is_object() || !o.contains(key))
        return fallback;
    const auto& v = o[key];
    if (v.is_number_integer())
        return v.get<int>();
    if (v.is_number_unsigned())
        return static_cast<int>(v.get<std::uint64_t>());
    if (v.is_number_float())
        return static_cast<int>(v.get<double>());
    return fallback;
}

std::uint64_t parse_hwnd_token(std::string_view s)
{
    if (s.empty())
        return 0;
    try {
        std::size_t consumed = 0;
        const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
        const unsigned long long v = std::stoull(std::string(s), &consumed, base);
        if (consumed == 0)
            return 0;
        return static_cast<std::uint64_t>(v);
    } catch (...) {
        return 0;
    }
}

std::uint64_t juint64(const nlohmann::json& o, const char* key, std::uint64_t fallback = 0)
{
    if (!o.is_object() || !o.contains(key))
        return fallback;
    const auto& v = o[key];
    if (v.is_number_unsigned())
        return v.get<std::uint64_t>();
    if (v.is_number_integer())
        return static_cast<std::uint64_t>(v.get<std::int64_t>());
    if (v.is_number_float())
        return static_cast<std::uint64_t>(v.get<double>());
    if (v.is_string())
        return parse_hwnd_token(v.get_ref<const std::string&>());
    return fallback;
}

bool jbool(const nlohmann::json& o, const char* key, bool fallback = false)
{
    return o.is_object() && o.contains(key) && o[key].is_boolean() ? o[key].get<bool>() : fallback;
}

// ── event helpers (native BlockIO surface) ─────────────────────────────────
//
// app_ok mirrors the old emit_event + attach_result: it flags ok/kind on the
// data envelope (so harnesses keep asserting `event.data.tool`, `.x`, ...) and
// flushes `result` into PREVIOUS / storeAs. app_err emits an error envelope and
// returns BlockResult::Error so the executor's continueOnError/abortOnError
// policy decides flow.
BlockResult app_ok(BlockIO& io, const std::string& kind, std::string message,
                   nlohmann::json payload, const std::string& id, nlohmann::json result)
{
    payload["ok"] = true;
    if (!payload.contains("kind"))
        payload["kind"] = kind;
    io.ok(kind, std::move(message)).id(id).type("app")
      .payload(std::move(payload))
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult app_err(BlockIO& io, const std::string& kind, std::string message,
                    const std::string& id, nlohmann::json extra = nlohmann::json::object())
{
    if (!extra.is_object())
        extra = nlohmann::json::object();
    extra["error"] = message;
    extra["ok"] = false;
    if (!extra.contains("kind"))
        extra["kind"] = kind;
    io.error(kind, message, 1).id(id).type("app").payload(std::move(extra));
    return BlockResult::Error;
}

#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT

namespace ab = pm::host::app_batch;
namespace ai = pm::host::app_inspect;
namespace au = pm::host::app_use;
namespace insp = pm::host::inspect;
namespace ci = pm::host::chrome_inspect;

// ── unicode bridges (kept private to this TU; no leakage) ──────────────────
std::wstring u2w(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string w2u(const std::wstring& ws)
{
    if (ws.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

bool contains_ci(const std::wstring& haystack, const std::wstring& needle)
{
    if (needle.empty())
        return true;
    std::wstring h = haystack;
    std::wstring n = needle;
    auto lower = [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); };
    std::transform(h.begin(), h.end(), h.begin(), lower);
    std::transform(n.begin(), n.end(), n.begin(), lower);
    return h.find(n) != std::wstring::npos;
}

// ── JSON shape helpers (mirrors Tool_ComputerUse for envelope parity) ──────
nlohmann::json rect_json(const ai::Rect& r)
{
    return {
        {"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h},
        {"left", r.x}, {"top", r.y}, {"right", r.x + r.w}, {"bottom", r.y + r.h},
        {"center", {{"x", r.x + r.w / 2}, {"y", r.y + r.h / 2}}},
    };
}

nlohmann::json window_meta_json(HWND hwnd)
{
    nlohmann::json out = nlohmann::json::object();
    if (!hwnd || !::IsWindow(hwnd))
        return out;
    wchar_t title[1024] = {};
    ::GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    out["hwnd"] = reinterpret_cast<std::uintptr_t>(hwnd);
    out["title"] = w2u(title);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid)
        out["pid"] = pid;
    RECT r{};
    if (::GetWindowRect(hwnd, &r) && r.right > r.left && r.bottom > r.top) {
        ai::Rect rr{r.left, r.top, r.right - r.left, r.bottom - r.top};
        out["rect"] = rect_json(rr);
    }
    return out;
}

nlohmann::json element_json(const ai::ElementInfo& e)
{
    return {
        {"index", e.index},
        {"pid", e.pid},
        {"hwnd", reinterpret_cast<std::uintptr_t>(e.hwnd)},
        {"process", w2u(e.process)},
        {"windowTitle", w2u(e.window_title)},
        {"controlType", w2u(e.control_type)},
        {"name", w2u(e.name)},
        {"value", w2u(e.value)},
        {"automationId", w2u(e.automation_id)},
        {"className", w2u(e.class_name)},
        {"frameworkId", w2u(e.framework_id)},
        {"enabled", e.enabled},
        {"offscreen", e.offscreen},
        {"focused", e.focused},
        {"keyboardFocusable", e.keyboard_focusable},
        {"nativeHwnd", e.native_hwnd},
        {"patterns", {
            {"invoke", e.has_invoke},
            {"value", e.has_value},
            {"text", e.has_text},
            {"selectionItem", e.has_selection_item},
            {"expandCollapse", e.has_expand_collapse},
            {"scroll", e.has_scroll},
        }},
        {"rect", rect_json(e.rect)},
        {"center", {{"x", e.rect.x + e.rect.w / 2}, {"y", e.rect.y + e.rect.h / 2}}},
    };
}

// ── target resolution (selectors: hwnd | pid | title | process | foreground)
HWND resolve_target_window(const nlohmann::json& block, std::string& err)
{
    const std::uint64_t hwnd_raw = juint64(block, "hwnd");
    if (hwnd_raw) {
        HWND h = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwnd_raw));
        if (::IsWindow(h))
            return h;
        err = "hwnd is not a valid window";
        return nullptr;
    }
    const DWORD pid = static_cast<DWORD>(juint64(block, "pid"));
    const std::wstring title = u2w(jstr(block, "title"));
    const std::wstring process = u2w(jstr(block, "process"));
    const bool foreground = jbool(block, "foreground");

    if (foreground) {
        HWND fg = ::GetForegroundWindow();
        const HWND preferred = media::win::context::ContextStore::instance().PreferredExternalHwnd();
        if (preferred && preferred != fg && ::IsWindow(preferred)) {
            DWORD fg_pid = 0;
            ::GetWindowThreadProcessId(fg, &fg_pid);
            if (!fg || fg_pid == ::GetCurrentProcessId())
                fg = preferred;
        }
        return fg;
    }
    if (!pid && title.empty() && process.empty())
        return nullptr;

    if (pid || !title.empty()) {
        HWND out = nullptr;
        if (au::find_window(pid, nullptr, title, out, err))
            return out;
        if (process.empty())
            return nullptr;
        err.clear();
    }
    ai::Query q;
    q.process_contains = process;
    q.title_contains = title;
    std::vector<ai::WindowDump> windows;
    if (!ai::dump_windows(q, windows, err))
        return nullptr;
    if (windows.empty()) {
        err = "no matching window for selector";
        return nullptr;
    }
    return windows.front().hwnd;
}

// Overlay resolved BlockIO params (bindings / ${target.hwnd}) onto the raw
// selector. Integer hwnd fields are not interpolated by the param pipeline.
nlohmann::json selector_json(BlockIO& io)
{
    nlohmann::json sel = io.raw().is_object() ? io.raw() : nlohmann::json::object();

    std::uint64_t hwnd = 0;
    if (const long long n = io.i64("hwnd", 0); n > 0)
        hwnd = static_cast<std::uint64_t>(n);
    if (!hwnd && io.has("hwnd") && io.raw()["hwnd"].is_string()) {
        std::string token = io.raw()["hwnd"].get<std::string>();
        if (token.find("${") != std::string::npos)
            token = bv::resolve(token, io.runtime());
        hwnd = parse_hwnd_token(token);
    }
    if (hwnd)
        sel["hwnd"] = hwnd;

    if (const long long pid = io.i64("pid", 0); pid > 0)
        sel["pid"] = static_cast<std::uint64_t>(pid);
    if (const std::string title = io.str("title"); !title.empty())
        sel["title"] = title;
    if (const std::string process = io.str("process"); !process.empty())
        sel["process"] = process;
    sel["foreground"] = io.flag("foreground", jbool(io.raw(), "foreground"));
    return sel;
}

// Activate when caller provided any selector. `used` is nullptr if no selector
// was supplied (caller keeps the current foreground).
bool activate_target_if_specified(BlockIO& io, HWND& used, std::string& err)
{
    const nlohmann::json sel = selector_json(io);
    HWND target = resolve_target_window(sel, err);
    if (!target) {
        const bool hwnd_requested = io.has("hwnd") || io.i64("hwnd", 0) > 0;
        if (hwnd_requested && err.empty())
            err = "hwnd did not resolve to a window";
        if (!err.empty())
            return false;
        used = nullptr;
        return true;
    }
    if (!au::activate_window(target, err))
        return false;
    used = target;
    return true;
}

ai::Query query_from_block(const nlohmann::json& block)
{
    ai::Query q;
    q.foreground = jbool(block, "foreground");
    q.process_contains = u2w(jstr(block, "process"));
    q.title_contains = u2w(jstr(block, "title"));
    const int raw_limit = jint(block, "limit", 0);
    q.limit = (raw_limit <= 0) ? std::numeric_limits<int>::max() : raw_limit;
    q.probe_cells = jbool(block, "probeCells", jbool(block, "probe_cells"));
    return q;
}

// Resolve (x|xw, y|yw) against an optional anchor window rect. Returns false
// if no coords supplied (caller's choice whether that's an error).
bool resolve_xy(const nlohmann::json& block,
                const RECT* anchor,
                const char* x_abs, const char* y_abs,
                const char* x_win, const char* y_win,
                int& out_x, int& out_y,
                std::string& err)
{
    const bool has_abs = block.is_object() && (block.contains(x_abs) || block.contains(y_abs));
    const bool has_win = block.is_object() && (block.contains(x_win) || block.contains(y_win));
    if (!has_abs && !has_win)
        return false;
    out_x = jint(block, x_abs);
    out_y = jint(block, y_abs);
    if (has_win) {
        if (!anchor) {
            err = "window-relative coords need an activated window (set process/title/pid/hwnd)";
            return false;
        }
        if (block.contains(x_win))
            out_x = anchor->left + jint(block, x_win);
        if (block.contains(y_win))
            out_y = anchor->top + jint(block, y_win);
    }
    return true;
}

// ── individual block handlers ──────────────────────────────────────────────

BlockResult app_activate_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    std::string err;
    HWND used = nullptr;
    if (!activate_target_if_specified(io, used, err))
        return app_err(io, "appActivate", "appActivate: " + err, id);
    if (!used)
        return app_err(io, "appActivate",
                       "appActivate: a target selector is required (process/title/pid/hwnd/foreground)", id);
    auto meta = window_meta_json(used);
    nlohmann::json payload{{"tool", "app_activate"}, {"activated", meta}};
    return app_ok(io, "appActivate", "window activated", std::move(payload), id, std::move(meta));
}

BlockResult app_open_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    au::LaunchOptions opts;
    opts.exe = u2w(jstr(block, "exe"));
    if (opts.exe.empty())
        return app_err(io, "appOpen", "appOpen: 'exe' is required", id);
    opts.args = u2w(jstr(block, "args"));
    opts.cwd = u2w(jstr(block, "cwd"));
    opts.x = jint(block, "x", 0);
    opts.y = jint(block, "y", 0);
    opts.w = jint(block, "width", 0);
    opts.h = jint(block, "height", 0);
    opts.wait_ms = jint(block, "waitMs", jint(block, "wait_ms", 3000));

    au::LaunchResult res;
    std::string err;
    if (!au::open_app(opts, res, err))
        return app_err(io, "appOpen", "appOpen: " + err, id);

    nlohmann::json payload{{"tool", "app_open"}, {"pid", res.pid}};
    nlohmann::json result{{"pid", res.pid}};
    if (res.hwnd) {
        auto meta = window_meta_json(res.hwnd);
        if (meta.contains("hwnd"))   { payload["hwnd"]  = meta["hwnd"];  result["hwnd"]  = meta["hwnd"]; }
        if (meta.contains("title"))  { payload["title"] = meta["title"]; result["title"] = meta["title"]; }
        if (meta.contains("rect"))   { payload["rect"]  = meta["rect"];  result["rect"]  = meta["rect"]; }
    } else {
        payload["note"] = "no main window resolved within waitMs; try appInspectDump to locate it";
    }
    return app_ok(io, "appOpen", "process launched", std::move(payload), id, std::move(result));
}

BlockResult app_close_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    const bool force = jbool(block, "force");
    std::string err;
    const nlohmann::json sel = selector_json(io);
    HWND target = resolve_target_window(sel, err);
    DWORD pid = static_cast<DWORD>(juint64(sel, "pid"));
    if (target && pid == 0)
        ::GetWindowThreadProcessId(target, &pid);

    if (!target && !pid)
        return app_err(io, "appClose", "appClose: requires hwnd, pid, process, title, or foreground", id);
    if (!target && pid && !force && !err.empty())
        return app_err(io, "appClose", "appClose: " + err, id);

    if (force) {
        if (!pid)
            return app_err(io, "appClose", "appClose: force requires a resolvable pid", id);
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h)
            return app_err(io, "appClose", "appClose: OpenProcess failed", id);
        const BOOL ok = ::TerminateProcess(h, 0);
        ::CloseHandle(h);
        if (!ok)
            return app_err(io, "appClose", "appClose: TerminateProcess failed", id);
        nlohmann::json payload{{"tool", "app_close"}, {"pid", pid}, {"method", "terminate"}};
        return app_ok(io, "appClose", "process terminated", std::move(payload), id,
                      nlohmann::json{{"pid", pid}, {"method", "terminate"}});
    }
    if (!target)
        return app_err(io, "appClose", "appClose: no window resolved for graceful close (try force=true with pid)", id);
    if (!::PostMessageW(target, WM_CLOSE, 0, 0))
        return app_err(io, "appClose", "appClose: PostMessage(WM_CLOSE) failed", id);
    nlohmann::json payload{{"tool", "app_close"},
                           {"hwnd", reinterpret_cast<std::uintptr_t>(target)},
                           {"method", "wm_close"}};
    nlohmann::json result{{"hwnd", reinterpret_cast<std::uintptr_t>(target)}, {"method", "wm_close"}};
    if (pid) {
        payload["pid"] = pid;
        result["pid"] = pid;
    }
    return app_ok(io, "appClose", "WM_CLOSE posted", std::move(payload), id, std::move(result));
}

// Resolve a screenshot path against the block's own cwd hint or the runtime's
// default. Unlike the LLM tool we don't apply the fs guard  -  block scripts
// are user-owned, not agent-owned.
std::string resolve_output_path(const std::string& raw, const BlockRuntime& runtime)
{
    if (raw.empty())
        return raw;
    std::error_code ec;
    std::filesystem::path p = media::path::path_from_utf8(raw);
    if (p.is_absolute())
        return media::path::path_to_utf8(std::filesystem::weakly_canonical(p, ec));
    std::filesystem::path base = runtime.options.default_cwd.empty()
        ? std::filesystem::current_path(ec)
        : runtime.options.default_cwd;
    if (ec)
        base = std::filesystem::current_path();
    return media::path::path_to_utf8((base / p).lexically_normal());
}

bool parse_rect(const std::string& s, ai::Rect& r)
{
    std::vector<int> nums;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == 'x' || c == 'X' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { nums.push_back(std::stoi(cur)); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        nums.push_back(std::stoi(cur));
    if (nums.size() != 4)
        return false;
    r = {nums[0], nums[1], nums[2], nums[3]};
    return true;
}

BlockResult app_screenshot_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    const std::string raw_out = jstr(block, "outputPath", jstr(block, "output_path"));
    if (raw_out.empty())
        return app_err(io, "appScreenshot", "appScreenshot: 'outputPath' is required", id);
    const std::string out_abs = resolve_output_path(raw_out, io.runtime());
    std::error_code ec;
    std::filesystem::create_directories(media::path::path_from_utf8(out_abs).parent_path(), ec);

    ai::Rect rect{};
    HWND window_capture = nullptr;
    std::string err;
    const std::string rect_s = jstr(block, "rect");
    if (!rect_s.empty()) {
        try {
            if (!parse_rect(rect_s, rect))
                return app_err(io, "appScreenshot", "appScreenshot: rect must be x,y,w,h", id);
        } catch (const std::exception& ex) {
            return app_err(io, "appScreenshot", std::string("appScreenshot: invalid rect: ") + ex.what(), id);
        }
    } else if (block.contains("elementIndex") || block.contains("element_index")) {
        const int idx = jint(block, "elementIndex", jint(block, "element_index", 0));
        if (!ai::find_element_rect(query_from_block(block), idx, rect, err))
            return app_err(io, "appScreenshot", "appScreenshot: " + err, id);
    } else {
        std::vector<ai::WindowDump> windows;
        if (!ai::dump_windows(query_from_block(block), windows, err))
            return app_err(io, "appScreenshot", "appScreenshot: " + err, id);
        if (windows.empty())
            return app_err(io, "appScreenshot", "appScreenshot: no matching window", id);
        window_capture = windows.front().hwnd;
        rect = windows.front().rect;
    }

    HWND activated = nullptr;
    if (window_capture && jbool(block, "activate", true)) {
        std::string act_err;
        if (!au::activate_window(window_capture, act_err))
            return app_err(io, "appScreenshot", "appScreenshot: " + act_err, id);
        activated = window_capture;
        RECT r{};
        if (::GetWindowRect(window_capture, &r) && r.right > r.left && r.bottom > r.top)
            rect = ai::Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
    }
    const int quality = jint(block, "quality", 85);
    if (window_capture) {
        if (!ai::save_window_jpeg(window_capture, rect, media::path::path_from_utf8(out_abs).wstring(), quality, err))
            return app_err(io, "appScreenshot", "appScreenshot: " + err, id);
    } else if (!ai::save_screen_rect_jpeg(rect, media::path::path_from_utf8(out_abs).wstring(), quality, err)) {
        return app_err(io, "appScreenshot", "appScreenshot: " + err, id);
    }
    nlohmann::json payload{{"tool", "app_screenshot"},
                           {"outputPath", out_abs},
                           {"capture", window_capture ? "window" : "screen"},
                           {"rect", rect_json(rect)}};
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appScreenshot", "screenshot saved", std::move(payload), id, out_abs);
}

BlockResult app_click_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    std::string err;
    HWND activated = nullptr;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appClick", "appClick: " + err, id);

    RECT anchor{};
    const bool have_anchor = activated && ::GetWindowRect(activated, &anchor);
    int x = 0, y = 0;
    if (!resolve_xy(block, have_anchor ? &anchor : nullptr, "x", "y", "xw", "yw", x, y, err)) {
        if (!err.empty())
            return app_err(io, "appClick", "appClick: " + err, id);
        return app_err(io, "appClick", "appClick: 'x'/'y' (screen) or 'xw'/'yw' (window-relative) required", id);
    }
    const std::string button = jstr(block, "button", "left");
    const int count = jint(block, "count", 1);
    const bool virtual_click = jbool(block, "virtual");
    if (!au::click_point(x, y, u2w(button), count, virtual_click, err))
        return app_err(io, "appClick", "appClick: " + err, id);

    nlohmann::json payload{{"tool", "app_click"},
                           {"x", x}, {"y", y},
                           {"button", button}, {"count", count},
                           {"virtual", virtual_click}};
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appClick", "click delivered", std::move(payload), id,
                  nlohmann::json{{"x", x}, {"y", y}, {"button", button}, {"count", count}});
}

BlockResult app_mouse_move_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    std::string err;
    HWND activated = nullptr;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appMouseMove", "appMouseMove: " + err, id);

    RECT anchor{};
    const bool have_anchor = activated && ::GetWindowRect(activated, &anchor);
    int x = 0, y = 0;
    if (!resolve_xy(block, have_anchor ? &anchor : nullptr, "x", "y", "xw", "yw", x, y, err))
        return app_err(io, "appMouseMove",
                       err.empty() ? std::string("appMouseMove: coords required") : "appMouseMove: " + err, id);
    const int duration_ms = jint(block, "durationMs", jint(block, "duration_ms", -1));
    const bool smooth = jbool(block, "smooth", duration_ms != 0);
    if (smooth ? !au::smooth_move(x, y, duration_ms, err) : !au::move_mouse(x, y, err))
        return app_err(io, "appMouseMove", "appMouseMove: " + err, id);

    nlohmann::json payload{{"tool", "app_mouse_move"}, {"x", x}, {"y", y},
                           {"smooth", smooth}, {"durationMs", duration_ms}};
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appMouseMove", "cursor moved", std::move(payload), id,
                  nlohmann::json{{"x", x}, {"y", y}});
}

BlockResult app_type_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    const std::string text = jstr(block, "text");
    if (text.empty())
        return app_err(io, "appType", "appType: 'text' is required", id);
    HWND activated = nullptr;
    std::string err;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appType", "appType: " + err, id);
    if (!au::type_text(u2w(text), err))
        return app_err(io, "appType", "appType: " + err, id);
    nlohmann::json payload{{"tool", "app_type"}, {"chars", static_cast<int>(text.size())}};
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appType", "text typed", std::move(payload), id, text);
}

BlockResult app_hotkey_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    const std::string keys = jstr(block, "keys");
    if (keys.empty())
        return app_err(io, "appHotkey", "appHotkey: 'keys' is required", id);
    HWND activated = nullptr;
    std::string err;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appHotkey", "appHotkey: " + err, id);
    const auto parts = au::split_keys(u2w(keys));
    if (parts.empty())
        return app_err(io, "appHotkey", "appHotkey: keys did not parse", id);
    if (!au::send_hotkey(parts, err))
        return app_err(io, "appHotkey", "appHotkey: " + err, id);
    nlohmann::json payload{{"tool", "app_hotkey"}, {"keys", keys}};
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appHotkey", "hotkey sent", std::move(payload), id, keys);
}

BlockResult app_key_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    const std::string key = jstr(block, "key");
    if (key.empty())
        return app_err(io, "appKey", "appKey: 'key' is required", id);
    HWND activated = nullptr;
    std::string err;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appKey", "appKey: " + err, id);
    std::vector<std::wstring> mods;
    if (block.contains("modifiers") && block["modifiers"].is_array()) {
        for (const auto& m : block["modifiers"])
            if (m.is_string())
                mods.push_back(u2w(m.get<std::string>()));
    }
    const int hold_ms = jint(block, "holdMs", jint(block, "hold_ms", 0));
    if (!au::press_key(u2w(key), mods, hold_ms, err))
        return app_err(io, "appKey", "appKey: " + err, id);
    nlohmann::json payload{{"tool", "app_key"}, {"key", key}, {"holdMs", hold_ms}};
    if (!mods.empty()) {
        nlohmann::json mods_json = nlohmann::json::array();
        for (const auto& m : mods)
            mods_json.push_back(w2u(m));
        payload["modifiers"] = std::move(mods_json);
    }
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appKey", "key pressed", std::move(payload), id, key);
}

BlockResult app_drag_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    HWND activated = nullptr;
    std::string err;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appDrag", "appDrag: " + err, id);

    RECT anchor{};
    const bool have_anchor = activated && ::GetWindowRect(activated, &anchor);
    const RECT* anchor_ptr = have_anchor ? &anchor : nullptr;

    std::vector<au::Point> path_pts;
    const char* mode = nullptr;
    if (block.contains("arc") && block["arc"].is_object()) {
        const auto& a = block["arc"];
        int cx = 0, cy = 0;
        if (!resolve_xy(a, anchor_ptr, "cx", "cy", "cxw", "cyw", cx, cy, err))
            return app_err(io, "appDrag",
                           err.empty() ? std::string("appDrag: arc requires center (cx/cy or cxw/cyw)")
                                       : "appDrag: " + err, id);
        const double radius = static_cast<double>(jint(a, "radius"));
        if (radius < 1.0)
            return app_err(io, "appDrag", "appDrag: arc 'radius' must be > 0", id);
        const double start_deg = static_cast<double>(jint(a, "startDeg", jint(a, "start_deg", 0)));
        const double end_deg = static_cast<double>(jint(a, "endDeg", jint(a, "end_deg", 360)));
        const double sweep = end_deg - start_deg;
        if (std::abs(sweep) < 0.5)
            return app_err(io, "appDrag", "appDrag: arc sweep is zero", id);
        int segments = jint(a, "segments");
        if (segments <= 0)
            segments = std::max(8, std::min(120, static_cast<int>(std::abs(sweep) / 8.0)));
        if (segments > 360)
            segments = 360;
        constexpr double kPi = 3.14159265358979323846;
        path_pts.reserve(static_cast<std::size_t>(segments + 1));
        for (int i = 0; i <= segments; ++i) {
            const double t = start_deg + (sweep * i) / segments;
            const double rad = (t * kPi) / 180.0;
            path_pts.push_back({cx + static_cast<int>(std::cos(rad) * radius),
                                cy + static_cast<int>(std::sin(rad) * radius)});
        }
        mode = "arc";
    } else if (block.contains("path") && block["path"].is_array()) {
        for (const auto& pt : block["path"]) {
            if (!pt.is_object())
                continue;
            int px = 0, py = 0;
            if (!resolve_xy(pt, anchor_ptr, "x", "y", "xw", "yw", px, py, err))
                return app_err(io, "appDrag",
                               "appDrag: path point requires x/y or xw/yw" + (err.empty() ? "" : " (" + err + ")"), id);
            path_pts.push_back({px, py});
        }
        if (path_pts.size() < 2)
            return app_err(io, "appDrag", "appDrag: 'path' needs >= 2 points", id);
        mode = "path";
    } else {
        int x1 = 0, y1 = 0;
        if (!resolve_xy(block, anchor_ptr, "x", "y", "xw", "yw", x1, y1, err))
            return app_err(io, "appDrag",
                           "appDrag: start point required (x,y or xw,yw)" + (err.empty() ? "" : " (" + err + ")"), id);
        int x2 = std::numeric_limits<int>::min();
        int y2 = std::numeric_limits<int>::min();
        if (block.contains("toX") || block.contains("toY") || block.contains("toXw") || block.contains("toYw") ||
            block.contains("to_x") || block.contains("to_y")) {
            nlohmann::json end_obj = nlohmann::json::object();
            if (block.contains("toX"))   end_obj["x"]  = block["toX"];
            if (block.contains("toY"))   end_obj["y"]  = block["toY"];
            if (block.contains("to_x"))  end_obj["x"]  = block["to_x"];
            if (block.contains("to_y"))  end_obj["y"]  = block["to_y"];
            if (block.contains("toXw"))  end_obj["xw"] = block["toXw"];
            if (block.contains("toYw"))  end_obj["yw"] = block["toYw"];
            if (!resolve_xy(end_obj, anchor_ptr, "x", "y", "xw", "yw", x2, y2, err))
                return app_err(io, "appDrag", "appDrag: end point invalid", id);
        } else if (block.contains("dx") || block.contains("dy")) {
            x2 = x1 + jint(block, "dx");
            y2 = y1 + jint(block, "dy");
        } else {
            return app_err(io, "appDrag",
                           "appDrag: end point required (toX/toY, toXw/toYw, dx/dy, or arc/path)", id);
        }
        if (x2 == x1 && y2 == y1)
            return app_err(io, "appDrag", "appDrag: end point equals start", id);
        path_pts = {{x1, y1}, {x2, y2}};
        mode = "linear";
    }

    const std::string button = jstr(block, "button", "left");
    const int duration_ms = jint(block, "durationMs", jint(block, "duration_ms", 250));
    if (duration_ms < 0 || duration_ms > 30000)
        return app_err(io, "appDrag", "appDrag: 'durationMs' must be in [0,30000]", id);
    if (!au::drag_path(path_pts, u2w(button), duration_ms, err))
        return app_err(io, "appDrag", "appDrag: " + err, id);

    nlohmann::json from_pt{{"x", path_pts.front().x}, {"y", path_pts.front().y}};
    nlohmann::json to_pt  {{"x", path_pts.back().x},  {"y", path_pts.back().y}};
    nlohmann::json payload{
        {"tool", "app_drag"},
        {"mode", mode},
        {"from", from_pt},
        {"to", to_pt},
        {"points", static_cast<int>(path_pts.size())},
        {"button", button},
        {"durationMs", duration_ms},
    };
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appDrag", "drag complete", std::move(payload), id,
                  nlohmann::json{{"from", from_pt}, {"to", to_pt}, {"mode", mode}});
}

BlockResult app_scroll_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    HWND activated = nullptr;
    std::string err;
    if (!activate_target_if_specified(io, activated, err))
        return app_err(io, "appScroll", "appScroll: " + err, id);

    RECT anchor{};
    const bool have_anchor = activated && ::GetWindowRect(activated, &anchor);
    // If caller provides target coords, move the cursor there first  -  wheel
    // events go to whichever window is under the cursor.
    int tx = 0, ty = 0;
    if (resolve_xy(block, have_anchor ? &anchor : nullptr, "x", "y", "xw", "yw", tx, ty, err)) {
        if (!au::smooth_move(tx, ty, jint(block, "moveDurationMs", -1), err))
            return app_err(io, "appScroll", "appScroll: " + err, id);
    } else if (!err.empty()) {
        return app_err(io, "appScroll", "appScroll: " + err, id);
    }
    const int clicks = jint(block, "clicks", 0);
    if (clicks == 0)
        return app_err(io, "appScroll", "appScroll: 'clicks' must be non-zero", id);
    const std::string axis = jstr(block, "axis", "vertical");
    const bool horizontal = axis == "horizontal" || axis == "h" || axis == "x";
    if (!au::scroll_wheel(clicks, horizontal, err))
        return app_err(io, "appScroll", "appScroll: " + err, id);

    nlohmann::json payload{{"tool", "app_scroll"}, {"clicks", clicks},
                           {"axis", horizontal ? "horizontal" : "vertical"}};
    if (activated)
        payload["activated"] = window_meta_json(activated);
    return app_ok(io, "appScroll", "scroll complete", std::move(payload), id,
                  nlohmann::json{{"clicks", clicks}, {"axis", horizontal ? "horizontal" : "vertical"}});
}

BlockResult app_batch_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    nlohmann::json doc;
    if (block.contains("steps") && block["steps"].is_array()) {
        doc = nlohmann::json{{"steps", block["steps"]}};
    } else if (block.contains("actions") && block["actions"].is_array()) {
        doc = nlohmann::json{{"steps", block["actions"]}};
    } else {
        return app_err(io, "appBatch", "appBatch: 'steps' (or 'actions') array is required", id);
    }
    for (auto& step : doc["steps"]) {
        if (step.is_object() && !step.contains("action") && step.contains("type") && step["type"].is_string())
            step["action"] = step["type"];
    }
    ab::RunOptions opts;
    opts.default_delay_ms = jint(block, "defaultDelayMs", jint(block, "default_delay_ms", 50));
    opts.default_wait_timeout_ms = jint(block, "defaultWaitTimeoutMs",
                                        jint(block, "default_wait_timeout_ms", 5000));
    opts.default_wait_interval_ms = jint(block, "defaultWaitIntervalMs",
                                         jint(block, "default_wait_interval_ms", 100));
    opts.stop_on_error = !jbool(block, "continueOnError", jbool(block, "continue_on_error", false));

    nlohmann::json report;
    std::string err;
    const bool ok = ab::run_json_batch(doc, opts, report, err);
    auto steps = report.value("steps", nlohmann::json::array());
    nlohmann::json payload{
        {"tool", "app_batch"},
        {"steps", steps},
        {"allOk", ok},
    };
    if (!err.empty())
        payload["error"] = err;
    if (!ok) {
        for (const auto& s : payload["steps"]) {
            if (!s.value("ok", true)) { payload["firstFailure"] = s; break; }
        }
        return app_err(io, "appBatch", err.empty() ? "batch had failures" : err, id, std::move(payload));
    }
    return app_ok(io, "appBatch", "batch complete", std::move(payload), id,
                  nlohmann::json{{"allOk", ok}, {"steps", std::move(steps)}});
}

// Parsed representation of a picker / screen spec string.
// Format:  screen:<monitor>[:<tag>=<val>]*
// Tags:  hwnd=<uint>  wintitle=<str>  rrect=<x,y,w,h>
struct ScreenSpecInfo {
    HWND        hwnd     = nullptr;
    std::wstring wintitle;
    bool has_rrect       = false;
    int  rrect_x = 0, rrect_y = 0, rrect_w = 0, rrect_h = 0;
};

static ScreenSpecInfo parse_screen_spec(const std::string& spec)
{
    ScreenSpecInfo out;
    if (spec.rfind("screen:", 0) != 0) return out;
    std::string rem = spec.substr(7);          // drop "screen:"
    auto pos = rem.find(':');
    if (pos == std::string::npos) return out;
    rem = rem.substr(pos + 1);                 // drop monitor index

    while (!rem.empty()) {
        const std::size_t colon = rem.find(':');
        const std::string tok   = colon == std::string::npos ? rem : rem.substr(0, colon);
        if (tok.rfind("hwnd=", 0) == 0 && !out.hwnd) {
            try {
                const auto v = std::stoull(tok.substr(5));
                if (v) out.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(v));
            } catch (...) {}
        } else if (tok.rfind("wintitle=", 0) == 0 && out.wintitle.empty()) {
            out.wintitle = u2w(tok.substr(9));
        } else if (tok.rfind("rrect=", 0) == 0 && !out.has_rrect) {
            std::string vals = tok.substr(6);
            int* fields[4] = {&out.rrect_x, &out.rrect_y, &out.rrect_w, &out.rrect_h};
            int fi = 0;
            while (fi < 4 && !vals.empty()) {
                const auto comma = vals.find(',');
                try { *fields[fi] = std::stoi(comma == std::string::npos ? vals : vals.substr(0, comma)); }
                catch (...) {}
                ++fi;
                if (comma == std::string::npos) break;
                vals = vals.substr(comma + 1);
            }
            if (fi == 4 && out.rrect_w > 0 && out.rrect_h > 0) out.has_rrect = true;
        }
        if (colon == std::string::npos) break;
        rem = rem.substr(colon + 1);
    }
    return out;
}

static void apply_spec_to_query(const ScreenSpecInfo& si, ai::Query& q)
{
    if (si.hwnd && !q.hwnd)             q.hwnd = si.hwnd;
    if (!si.wintitle.empty() && q.title_contains.empty()) q.title_contains = si.wintitle;
}

BlockResult app_inspect_dump_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    ai::Query q = query_from_block(block);
    const std::string input_spec = io.str("input");
    ScreenSpecInfo spec_info;
    if (!input_spec.empty() && input_spec.rfind("screen:", 0) == 0) {
        spec_info = parse_screen_spec(input_spec);
        apply_spec_to_query(spec_info, q);
    }
    std::vector<ai::WindowDump> windows;
    std::string err;
    if (!ai::dump_windows(q, windows, err))
        return app_err(io, "appInspectDump", "appInspectDump: " + err, id);

    // If the spec contains rrect= and filterToSelection is explicitly on,
    // narrow the element list to those whose center falls inside that rect.
    const bool filter_to_sel = io.flag("filterToSelection", false);
    if (filter_to_sel && spec_info.has_rrect && spec_info.hwnd) {
        RECT wr{};
        if (media::win::visible_window_bounds(spec_info.hwnd, wr)) {
            const int ax = wr.left + spec_info.rrect_x;
            const int ay = wr.top  + spec_info.rrect_y;
            const int ar = ax + spec_info.rrect_w;
            const int ab = ay + spec_info.rrect_h;
            for (auto& w : windows) {
                w.elements.erase(
                    std::remove_if(w.elements.begin(), w.elements.end(),
                        [&](const ai::ElementInfo& e) {
                            const int cx = e.rect.x + e.rect.w / 2;
                            const int cy = e.rect.y + e.rect.h / 2;
                            return cx < ax || cx >= ar || cy < ay || cy >= ab;
                        }),
                    w.elements.end());
            }
        }
    }

    const std::string fmt = jstr(block, "format", "md");
    nlohmann::json payload{{"tool", "app_inspect_dump"}, {"windowCount", static_cast<int>(windows.size())}};
    if (fmt == "json") {
        nlohmann::json windows_json = nlohmann::json::array();
        for (const auto& w : windows) {
            nlohmann::json jw = {{"pid", w.pid},
                                 {"hwnd", reinterpret_cast<std::uintptr_t>(w.hwnd)},
                                 {"process", w2u(w.process)},
                                 {"title", w2u(w.title)},
                                 {"rect", rect_json(w.rect)},
                                 {"elements", nlohmann::json::array()}};
            for (const auto& e : w.elements)
                jw["elements"].push_back(element_json(e));
            windows_json.push_back(std::move(jw));
        }
        payload["windows"] = windows_json;
        return app_ok(io, "appInspectDump", "dump complete", std::move(payload), id, std::move(windows_json));
    }
    ai::MarkdownOptions mo;
    mo.control_types_csv = u2w(jstr(block, "controls", "all"));
    mo.text_max_chars = jint(block, "textMaxChars", jint(block, "text_max_chars", 1200));
    std::string md = ai::dump_windows_markdown(windows, mo);
    payload["markdown"] = md;
    return app_ok(io, "appInspectDump", "dump complete", std::move(payload), id, std::move(md));
}

BlockResult app_inspect_find_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    std::vector<ai::WindowDump> windows;
    std::string err;
    if (!ai::dump_windows(query_from_block(block), windows, err))
        return app_err(io, "appInspectFind", "appInspectFind: " + err, id);

    const std::wstring name = u2w(jstr(block, "name"));
    const std::wstring value = u2w(jstr(block, "value"));
    const std::wstring automation_id = u2w(jstr(block, "automationId"));
    const std::wstring class_name = u2w(jstr(block, "className"));

    nlohmann::json matches_json = nlohmann::json::array();
    int matched = 0;
    for (const auto& w : windows) {
        for (const auto& e : w.elements) {
            if (!contains_ci(e.name, name))
                continue;
            if (!contains_ci(e.value, value))
                continue;
            if (!contains_ci(e.automation_id, automation_id))
                continue;
            if (!contains_ci(e.class_name, class_name))
                continue;
            matches_json.push_back(element_json(e));
            ++matched;
        }
    }
    nlohmann::json payload{{"tool", "app_inspect_find"}, {"count", matched}, {"matches", matches_json}};
    const int nth = jint(block, "nth", -1);
    nlohmann::json result;
    if (nth >= 0 && nth < matched) {
        payload["selected"] = matches_json[static_cast<std::size_t>(nth)];
        result = matches_json[static_cast<std::size_t>(nth)];
    } else {
        result = std::move(matches_json);
    }
    return app_ok(io, "appInspectFind", "find complete", std::move(payload), id, std::move(result));
}

// ── picker ────────────────────────────────────────────────────────────────────
// Runs an interactive window/element pick session (Spy++-style highlight overlay).
// The user moves the cursor over any window or web element and left-clicks to
// commit. The result is a screen spec string identical to the videoDetect /
// videoScreenCapture / imageScreenshot `input` format:
//
//   screen:0                              → primary monitor (custom rect: drag, or Shift/Ctrl+drag)
//   screen:0:layer=monitor                → that display including open apps (default for desktop picks)
//   screen:0:hwnd=…:layer=desktop         → wallpaper only (includeOpenWindows=false)
//   screen:0:hwnd=12345:wintitle=Chrome*  → root window
//   screen:0:hwnd=12345:wintitle=Chrome*:rrect=10,50,640,480
//                                         → UIA sub-element (web node, video player…)
//
// Modes: click = window/UIA element (current stack entry); Alt+wheel or [ ]
// cycles leaf → siblings → ∪siblings → parent with HUD; drag (or Shift/Ctrl+drag)
// = free rectangle; Z = 8× magnifier (optional), C = copy sampled colour; ESC cancels
// (block returns Error / continueOnError).
//
// The payload is always a superset of `format`: colorHex/rgb whenever a pixel was
// sampled, and selectors/cssSelector/xpath whenever the Chrome bridge answered.
// PREVIOUS / storeAs ← screen spec string (screen_input kind).

BlockResult picker_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();

    media::win::inspect::PickOptions opts;
    const std::string format = jstr(block, "format", "screen");
    const bool color_pick = (format == "color");
    const bool measure_pick = (format == "measure");
    opts.highlight       = color_pick ? false
                          : measure_pick ? true
                                         : jbool(block, "highlight",   true);
    opts.hud             = color_pick ? false : jbool(block, "hud", true);
    opts.resolve_uia     = color_pick ? false : jbool(block, "resolveUia",  true);
    opts.show_hidden     = jbool(block, "showHidden",  true);
    opts.siblings        = color_pick ? false : jbool(block, "siblings",    false);
    opts.eyedropper      = color_pick;
    opts.measure         = measure_pick;
    opts.sample_color    = color_pick ? true : jbool(block, "sampleColor", false);
    opts.magnifier       = color_pick
                               ? jbool(block, "colorMagnifier", true)
                               : jbool(block, "magnifier", false);
    opts.resolve_cmdline = false;

    const int timeout_ms = static_cast<int>(
        jint(block, "timeoutMs", 60000));
    const bool chrome_dom = color_pick ? false : jbool(block, "chromeDom", true);
    opts.chrome_dom = chrome_dom;
    opts.chrome_preview = chrome_dom && jbool(block, "chromePreview", false);

    if (format == "context") {
        auto& store = media::win::context::ContextStore::instance();
        store.start_foreground_tracking();
        const auto inv = store.CaptureInvocationContext();
        media::win::context::ContextQuery q;
        q.capabilities = media::win::context::ContextCapability::Files
            | media::win::context::ContextCapability::Folders
            | media::win::context::ContextCapability::Text
            | media::win::context::ContextCapability::URL;
        q.invocationId = inv.id;
        q.allowMru = jbool(block, "allowMru", false);
        q.allowOpenApps = jbool(block, "allowOpenApps", false);
        q.allowAppRecent = jbool(block, "allowAppRecent", false);
        if (q.allowOpenApps)
            store.GetOpenApps();
        if (q.allowAppRecent)
            store.GetAppRecent({});
        const auto elig = store.EligibleSources(q);
        const auto snap = store.Snapshot();
        const auto resolved = store.ResolveContext(q);
        nlohmann::json items = nlohmann::json::array();
        const auto& src = resolved.resolved ? resolved.source
            : (elig.empty() ? media::win::context::ContextSource{} : elig.front());
        for (const auto& it : src.items) {
            nlohmann::json ji{{"name", w2u(it.displayName)}, {"kind",
                it.kind == media::win::context::ContextItem::Kind::Directory ? "directory" : "file"}};
            if (it.path)
                ji["path"] = w2u(*it.path);
            items.push_back(std::move(ji));
        }
        const auto js = store.ToJson(snap, false);
        nlohmann::json detail{
            {"format", "context"},
            {"invocationId", inv.id},
            {"resolved", resolved.resolved},
            {"reason", resolved.reason},
            {"type", media::win::context::source_type_name(src.type)},
            {"items", items},
            {"markdown", store.ToMarkdown(snap, elig, q)},
            {"sources", js.contains("sources") ? js["sources"] : nlohmann::json::array()},
        };
        if (q.allowMru && js.contains("mru"))
            detail["mru"] = js["mru"];
        if (q.allowOpenApps && js.contains("open_apps"))
            detail["open_apps"] = js["open_apps"];
        if (q.allowAppRecent && js.contains("app_recent"))
            detail["app_recent"] = js["app_recent"];
        std::string first;
        if (!src.items.empty() && src.items[0].path)
            first = w2u(*src.items[0].path);
        io.ok("picker", first)
          .set("text", store.ToMarkdown(snap, elig, q))
          .set("source", std::string("context"))
          .payload(std::move(detail))
          .result(first, ParamKind::string_);
        return BlockResult::Ok;
    }

    if (format == "selection") {
        media::win::inspect::SelectedTextOptions sopts;
        sopts.chrome_dom = chrome_dom;
        sopts.timeout_ms = std::min(timeout_ms, 15000);
        sopts.max_chars = static_cast<int>(jint(block, "textMaxChars", 0));

        media::win::inspect::SelectedText sel;
        std::string sel_err;
        const bool got = insp::get_selected_text(sopts, sel, sel_err);
        if (!got || sel.text.empty() || sel.password) {
            io.ok("picker", "")
              .set("text", "")
              .set("source", std::string("none"))
              .set("hwnd", 0LL)
              .set("process", "")
              .payload({
                  {"format", "selection"},
                  {"text", ""},
                  {"source", "none"},
                  {"inspectSource", "none"},
              })
              .result("", ParamKind::string_);
            return BlockResult::Ok;
        }

        const std::string text = w2u(sel.text);
        const char* source = "none";
        switch (sel.source) {
        case media::win::inspect::SelectedText::Source::UIAutomation:
            source = "uia";
            break;
        case media::win::inspect::SelectedText::Source::ChromeCDP:
            source = "chrome";
            break;
        default:
            break;
        }
        const auto hwnd_val = reinterpret_cast<std::uintptr_t>(sel.hwnd);
        nlohmann::json detail{
            {"format", "selection"},
            {"text", text},
            {"source", source},
            {"inspectSource", source},
            {"hwnd", static_cast<unsigned long long>(hwnd_val)},
            {"process", w2u(sel.process)},
            {"controlType", w2u(sel.control_type)},
            {"frameworkId", w2u(sel.framework_id)},
        };
        io.ok("picker", text)
          .set("text", text)
          .set("source", std::string(source))
          .set("hwnd", static_cast<long long>(hwnd_val))
          .set("process", w2u(sel.process))
          .payload(std::move(detail))
          .result(text, ParamKind::string_);
        return BlockResult::Ok;
    }

    if (format == "foreground") {
        media::win::inspect::ForegroundWindow fg;
        std::string fg_err;
        if (!insp::get_foreground_window(fg, fg_err)) {
            io.error("picker",
                     "picker: " + (fg_err.empty() ? std::string("no foreground window") : fg_err),
                     1);
            return BlockResult::Error;
        }
        const auto hwnd_u = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(fg.hwnd));
        const auto root_u = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(fg.root));
        const auto cur_u = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(fg.cursor_hwnd));
        const auto parent_u = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(fg.cursor_parent));
        const auto cur_root_u = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(fg.cursor_root));
        auto hwnd_hex = [](unsigned long long v) {
            char buf[32] = {};
            std::snprintf(buf, sizeof(buf), "0x%llX", v);
            return std::string(buf);
        };
        nlohmann::json detail{
            {"format", "foreground"},
            {"title", w2u(fg.title)},
            {"hwnd", hwnd_u},
            {"hwndHex", hwnd_hex(hwnd_u)},
            {"root", root_u},
            {"rootHex", hwnd_hex(root_u)},
            {"class", w2u(fg.class_name)},
            {"pid", fg.pid},
            {"process", w2u(fg.process)},
            {"source", fg.source},
            {"cursor", nlohmann::json::array({fg.cursor.x, fg.cursor.y})},
            {"cursorHwnd", cur_u},
            {"cursorHwndHex", hwnd_hex(cur_u)},
            {"cursorParent", parent_u},
            {"cursorParentHex", hwnd_hex(parent_u)},
            {"cursorRoot", cur_root_u},
            {"cursorRootHex", hwnd_hex(cur_root_u)},
        };

        std::string css_selector;
        if (chrome_dom) {
            media::win::inspect::PickResult stub;
            stub.hwnd = fg.cursor_hwnd ? fg.cursor_hwnd : fg.hwnd;
            stub.root = fg.root ? fg.root : fg.hwnd;
            stub.title = fg.title;
            stub.class_name = fg.class_name;
            stub.pid = fg.pid;
            stub.point = fg.cursor;
            RECT wr{};
            if (stub.root && ::GetWindowRect(stub.root, &wr)) {
                stub.root_rect = wr;
                if (!::PtInRect(&wr, stub.point)) {
                    stub.point.x = wr.left + (wr.right - wr.left) / 2;
                    stub.point.y = wr.top + (wr.bottom - wr.top) / 2;
                    stub.hwnd = fg.hwnd;
                }
            }
            if (fg.pid) {
                HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fg.pid);
                if (proc) {
                    wchar_t path[MAX_PATH * 2] = {};
                    DWORD n = ARRAYSIZE(path);
                    if (::QueryFullProcessImageNameW(proc, 0, path, &n) && n > 0)
                        stub.exe.assign(path, n);
                    ::CloseHandle(proc);
                }
            }
            const bool enrichable =
                ci::is_dom_enrichable_pick(stub)
                || ci::is_dom_enrichable_at(stub.hwnd, stub.point, stub.root);
            if (enrichable) {
                ci::DomInspectOpts dom_opts = ci::normalize_dom_inspect_opts({
                    jstr(block, "scope", "auto"),
                    jstr(block, "documentSelector", "html"),
                    static_cast<int>(jint(block, "textMaxChars", 0)),
                    static_cast<int>(jint(block, "htmlMaxChars", 8 * 1024 * 1024)),
                    static_cast<int>(jint(block, "markdownMaxChars", 2 * 1024 * 1024)),
                });
                ci::ChromePickEnrichResult dom;
                std::string dom_err;
                if (ci::enrich_pick_with_chrome_dom(
                        stub, std::min(timeout_ms, 15000), dom, dom_err, dom_opts)) {
                    detail["inspectSource"] = dom.source;
                    if (!dom.selectors.empty()) {
                        detail["selectors"] = dom.selectors;
                        css_selector = dom.css_selector;
                        if (!css_selector.empty())
                            detail["cssSelector"] = css_selector;
                        if (!dom.xpath.empty())
                            detail["xpath"] = dom.xpath;
                    }
                    if (!dom.element.empty())
                        detail["dom"] = dom.element;
                } else if (!dom_err.empty()) {
                    detail["domError"] = dom_err;
                }
            }
        }

        io.ok("picker", detail.dump())
          .set("title", w2u(fg.title))
          .set("hwnd", static_cast<long long>(hwnd_u))
          .set("cursorParent", static_cast<long long>(parent_u))
          .set("cssSelector", css_selector)
          .set("source", fg.source)
          .payload(nlohmann::json(detail))
          .result(Value::from_json(std::move(detail)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    media::win::inspect::PickResult pick;
    std::string err;

    // Run the blocking picker on a worker thread so we can enforce a timeout.
    // If the deadline expires, synthesise an ESC keypress so the picker exits
    // cleanly (sets pick.cancelled = true) before we join the thread.
    auto fut = std::async(std::launch::async, [&]() -> bool {
#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        return insp::pick_window(opts, pick, err);
    });

    const auto status =
        fut.wait_for(std::chrono::milliseconds(timeout_ms));

    if (status == std::future_status::timeout) {
        logger::warn("[picker-block] timed out after " + std::to_string(timeout_ms) + "ms");
        // Cancel the picker by injecting ESC into the input stream.
        INPUT esc[2]{};
        esc[0].type     = INPUT_KEYBOARD;
        esc[0].ki.wVk   = VK_ESCAPE;
        esc[1]          = esc[0];
        esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(2, esc, sizeof(INPUT));
        fut.wait();  // let the picker thread process the ESC and exit

        io.error("picker",
                 "picker: timed out after " +
                 std::to_string(timeout_ms / 1000) + "s", 1);
        return BlockResult::Error;
    }

    if (!fut.get()) {
        logger::warn("[picker-block] failed: " + err);
        io.error("picker", "picker: " + err, 1);
        return BlockResult::Error;
    }
    if (pick.cancelled) {
        io.error("picker", "picker: cancelled", 1);
        return BlockResult::Error;
    }

    if (format == "measure") {
        if (!pick.measure_ok) {
            io.error("picker", "picker: measure incomplete (need points A and B)", 1);
            return BlockResult::Error;
        }
        const int dx = pick.measure_b.x - pick.measure_a.x;
        const int dy = pick.measure_b.y - pick.measure_a.y;
        const double dist = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
        const RECT rect_a = (pick.measure_rect_a.right > pick.measure_rect_a.left &&
                             pick.measure_rect_a.bottom > pick.measure_rect_a.top)
                                ? pick.measure_rect_a
                                : media::win::inspect::point_as_rect(pick.measure_a);
        const RECT rect_b = (pick.measure_rect_b.right > pick.measure_rect_b.left &&
                             pick.measure_rect_b.bottom > pick.measure_rect_b.top)
                                ? pick.measure_rect_b
                                : media::win::inspect::point_as_rect(pick.measure_b);
        const int rect_a_w = rect_a.right - rect_a.left;
        const int rect_a_h = rect_a.bottom - rect_a.top;
        const int rect_b_w = rect_b.right - rect_b.left;
        const int rect_b_h = rect_b.bottom - rect_b.top;
        const int span_w = pick.measure_span.right - pick.measure_span.left;
        const int span_h = pick.measure_span.bottom - pick.measure_span.top;
        const auto mk_rect_spec = [&](int x, int y, int w, int h) {
            return "screen:" + std::to_string(pick.monitor_idx) + ":rect=" +
                   std::to_string(x) + "," + std::to_string(y) + "," +
                   std::to_string(w) + "," + std::to_string(h);
        };

        const std::string result_value = insp::measure_to_string(pick);
        nlohmann::json detail{
            {"format", "measure"},
            {"pointA", nlohmann::json::array({pick.measure_a.x, pick.measure_a.y})},
            {"pointB", nlohmann::json::array({pick.measure_b.x, pick.measure_b.y})},
            {"rectA", nlohmann::json::array(
                {rect_a.left, rect_a.top, rect_a_w, rect_a_h})},
            {"rectB", nlohmann::json::array(
                {rect_b.left, rect_b.top, rect_b_w, rect_b_h})},
            {"spanRect", nlohmann::json::array(
                {pick.measure_span.left, pick.measure_span.top, span_w, span_h})},
            {"specA", mk_rect_spec(rect_a.left, rect_a.top, rect_a_w, rect_a_h)},
            {"specB", mk_rect_spec(rect_b.left, rect_b.top, rect_b_w, rect_b_h)},
            {"specSpan", mk_rect_spec(pick.measure_span.left, pick.measure_span.top,
                                      span_w, span_h)},
            {"dx", dx},
            {"dy", dy},
            {"distance", std::llround(dist)},
            {"monitor", pick.monitor_idx},
        };
        io.ok("picker", result_value)
          .set("specA", detail["specA"].get<std::string>())
          .set("specB", detail["specB"].get<std::string>())
          .set("specSpan", detail["specSpan"].get<std::string>())
          .set("dx", static_cast<long long>(dx))
          .set("dy", static_cast<long long>(dy))
          .set("distance", static_cast<long long>(std::llround(dist)))
          .payload(std::move(detail))
          .result(result_value, ParamKind::string_);
        return BlockResult::Ok;
    }

    // Build a WGC-safe screen spec. The shared helper clips UIA/custom
    // selections to the capturable root so rrect never starts negative.
    const auto hwnd_val = reinterpret_cast<std::uintptr_t>(pick.root ? pick.root : pick.hwnd);
    const std::string title = w2u(pick.title);
    const std::string spec =
        pm::video::video_input_spec_from_pick(
            pick, title, jbool(block, "includeOpenWindows", true));
    const RECT clippedTarget = pm::video::clipped_picker_target(pick);
    const long long pickedWidth =
        clippedTarget.right - clippedTarget.left;
    const long long pickedHeight =
        clippedTarget.bottom - clippedTarget.top;

    nlohmann::json detail{
        {"spec",    spec},
        {"hwnd",    static_cast<unsigned long long>(hwnd_val)},
        {"title",   title},
        {"monitor", pick.monitor_idx},
        {"width", pickedWidth},
        {"height", pickedHeight},
        {"includeOpenWindows", jbool(block, "includeOpenWindows", true)},
    };
    if (pick.uia_ok) {
        detail["uiaName"]         = w2u(pick.uia_name);
        detail["uiaAutomationId"] = w2u(pick.uia_automation_id);
        detail["uiaControlType"]  = w2u(pick.uia_control_type);
        detail["uiaFrameworkId"]  = w2u(pick.uia_framework_id);
    }

    std::string result_value = spec;
    ParamKind result_kind = ParamKind::screen_input;
    detail["format"] = format;
    detail["inspectSource"] = "uia";

    // Sampled pixel colour: primary result for format=color, payload side-channel otherwise.
    const std::string color_hex =
        insp::color_to_hex(pick.color, pick.color_ok);
    if (!color_hex.empty()) {
        detail["colorHex"] = color_hex;
        detail["rgb"] = nlohmann::json::array(
            {GetRValue(pick.color), GetGValue(pick.color), GetBValue(pick.color)});
    }
    if (format == "color") {
        if (color_hex.empty()) {
            io.error("picker", "picker: no color sampled", 1);
            return BlockResult::Error;
        }
        result_value = color_hex;
        result_kind = ParamKind::color_;
        detail["point"] = nlohmann::json::array({pick.point.x, pick.point.y});
    }

    const bool wants_dom_result = (format == "md" || format == "html");
    const bool wants_selector = (format == "selector");
    const bool wants_text = (format == "text");
    const int text_max_chars = static_cast<int>(jint(block, "textMaxChars", 0));
    const int html_max_chars = static_cast<int>(jint(block, "htmlMaxChars", 8 * 1024 * 1024));
    const int markdown_max_chars = static_cast<int>(
        jint(block, "markdownMaxChars", 2 * 1024 * 1024));
    const std::string on_dom_fail = jstr(block, "onDomFail", "error");
    ci::DomInspectOpts dom_opts = ci::normalize_dom_inspect_opts({
        jstr(block, "scope", "auto"),
        jstr(block, "documentSelector", "html"),
        text_max_chars,
        html_max_chars,
        markdown_max_chars,
    });
    std::string css_selector;
    std::string picked_text;
    // DOM selectors are useful for Chrome / in-app WebView2 picks (app_click /
    // app-use), so enrich whenever the target is DOM-capable — not only md/html.
    if (chrome_dom &&
        (wants_dom_result || wants_selector || wants_text || ci::is_dom_enrichable_pick(pick))) {
        logger::trace(std::string("[picker-block] chromeDom enrich format=") + format
                      + " scope=" + dom_opts.scope
                      + " documentSelector=" + dom_opts.document_selector
                      + " onDomFail=" + on_dom_fail
                      + " hwnd=0x" + std::to_string(reinterpret_cast<uintptr_t>(pick.hwnd))
                      + " class=" + w2u(pick.class_name)
                      + " exe=" + w2u(pick.exe)
                      + " enrichable=" + (ci::is_dom_enrichable_pick(pick) ? "1" : "0"));
        ci::ChromePickEnrichResult dom;
        std::string dom_err;
        if (ci::enrich_pick_with_chrome_dom(
                pick, std::min(timeout_ms, 15000), dom, dom_err, dom_opts)) {
            detail["inspectSource"] = dom.source;
            detail["scope"] = dom_opts.scope;
            detail["documentSelector"] = dom_opts.document_selector;
            detail["dom"] = dom.element;
            if (!dom.selectors.empty()) {
                detail["selectors"] = dom.selectors;
                css_selector = dom.css_selector;
                if (!dom.xpath.empty()) detail["xpath"] = dom.xpath;
            }
            if (wants_text) {
                picked_text = ci::text_from_dom_result(dom);
                if (!picked_text.empty()) {
                    detail["speakText"] = picked_text;
                    detail["innerText"] = dom.element.value("innerText", std::string{});
                }
            }
            if (wants_dom_result) {
                if (format == "html") {
                    if (html_max_chars > 0
                        && dom.html.size() > static_cast<size_t>(html_max_chars)) {
                        dom.html.resize(static_cast<size_t>(html_max_chars));
                        dom.html += "\xE2\x80\xA6";
                    }
                    result_value = dom.html;
                    result_kind = ParamKind::string_;
                    detail["html"] = dom.html;
                } else {
                    result_value = ci::markdown_from_dom_result(dom, markdown_max_chars);
                    result_kind = ParamKind::string_;
                    detail["markdown"] = result_value;
                }
            } else if (wants_selector) {
                if (css_selector.empty()) {
                    io.error("picker", "picker: no CSS selector for target", 1);
                    return BlockResult::Error;
                }
                result_value = css_selector;
                result_kind = ParamKind::string_;
            }
        } else {
            detail["domError"] = dom_err;
            logger::warn("[picker-block] chrome dom enrich failed: " + dom_err);
            // md/html/selector default to hard-fail. onDomFail=text|screen is for
            // saved automations that should still produce a result.
            if (wants_selector || wants_dom_result) {
                if (on_dom_fail == "text") {
                    std::wstring wtext;
                    std::string uia_err;
                    if (insp::spoken_text_from_pick(
                            pick, wtext, uia_err, text_max_chars)) {
                        picked_text = w2u(wtext);
                        result_value = picked_text;
                        result_kind = ParamKind::string_;
                        detail["inspectSource"] = "uia";
                        detail["text"] = picked_text;
                    } else {
                        io.error("picker", "picker: " + (dom_err.empty()
                            ? (uia_err.empty() ? std::string("DOM inspection failed") : uia_err)
                            : dom_err), 1);
                        return BlockResult::Error;
                    }
                } else if (on_dom_fail == "screen") {
                    detail["inspectSource"] = "screen";
                    result_value = spec;
                    result_kind = ParamKind::screen_input;
                } else {
                    io.error("picker", "picker: " + (dom_err.empty()
                        ? std::string("DOM inspection failed")
                        : dom_err), 1);
                    return BlockResult::Error;
                }
            }
        }
    } else if (wants_selector) {
        io.error("picker", "picker: selector format requires Chrome/in-app browser + chromeDom", 1);
        return BlockResult::Error;
    }

    if (wants_text) {
        if (picked_text.empty()) {
            std::wstring wtext;
            std::string uia_err;
            if (insp::spoken_text_from_pick(
                    pick, wtext, uia_err, text_max_chars)) {
                picked_text = w2u(wtext);
                if (detail.value("inspectSource", std::string{}) != "dom")
                    detail["inspectSource"] = "uia";
            } else if (!uia_err.empty()) {
                detail["textError"] = uia_err;
            }
        }
        if (picked_text.empty()) {
            io.error("picker", "picker: no text at target", 1);
            return BlockResult::Error;
        }
        result_value = picked_text;
        result_kind = ParamKind::string_;
        detail["text"] = picked_text;
    }

    io.ok("picker", result_value)
      .set("spec",    spec)
      .set("hwnd",    static_cast<long long>(hwnd_val))
      .set("title",   title)
      .set("monitor", static_cast<long long>(pick.monitor_idx))
      .set("width", pickedWidth)
      .set("height", pickedHeight)
      .set("colorHex", color_hex)
      .set("cssSelector", css_selector)
      .set("text", wants_text ? picked_text : std::string{})
      .payload(std::move(detail))
      .result(result_value, result_kind);
    return BlockResult::Ok;
}

nlohmann::json picker_default()
{
    return {{"kind","picker"}, {"highlight",true}, {"resolveUia",true},
            {"hud",true}, {"showHidden",true}, {"siblings",true}, {"chromeDom",true},
            {"chromePreview",false}, {"sampleColor",false}, {"magnifier",false},
            {"colorMagnifier",true},
            {"scope","auto"}, {"documentSelector","html"},
            {"onDomFail","error"},
            {"textMaxChars",0},
            {"htmlMaxChars", 8 * 1024 * 1024},
            {"markdownMaxChars", 2 * 1024 * 1024},
            {"format","screen"}, {"timeoutMs",60000},
            {"includeOpenWindows", true}};
}

std::vector<ParamDef> picker_params()
{
    return {
        pd_enum("format", {"screen", "md", "html", "text", "color", "selector", "measure",
                           "selection", "foreground", "context"})
            .opt().dflt("screen")
            .desc("Result format. screen = capture spec; md/html = in-app or Chrome DOM; "
                  "text = plain speakable string (DOM innerText / UIA); "
                  "selection = current focused selection (UIA TextPattern, then Chrome extension); "
                  "foreground = last focused window JSON (title, hwnd, cursor parent, "
                  "Chrome css/xpath when chromeDom); "
                  "context = OS context sources (Explorer/Desktop/drag/invocation) as JSON+markdown; "
                  "color = eyedropper; selector = CSS; "
                  "measure = boundary-snapped point-to-point line."),
        pd_bool("includeOpenWindows").grp("capture_options").dflt(true)
            .desc("When the pick is a monitor/desktop: capture open apps on that "
                  "display (screen:N:layer=monitor). Off = wallpaper only "
                  "(hwnd=Progman/WorkerW PrintWindow)."),
        pd_bool("allowMru").grp("capture_options").dflt(false)
            .desc("format=context: include session MRU as eligible candidates. "
                  "Never auto-resolves file operations."),
        pd_bool("allowOpenApps").grp("capture_options").dflt(false)
            .desc("format=context: include the bounded open-app / taskbar-candidate snapshot."),
        pd_bool("allowAppRecent").grp("capture_options").dflt(false)
            .desc("format=context: lazy per-app Shell Recent/Frequent (IApplicationDocumentLists). "
                  "Never auto-resolves file operations."),
        pd_bool("chromeDom").opt().dflt(true)
            .desc("Chrome / in-app browser: resolve DOM/selectors in the committed payload, "
                  "and for format=selection fall back to the extension after UIA. "
                  "Off = UIA/HWND only."),
        pd_enum("scope", {"auto", "element", "document"}).opt().dflt("auto")
            .desc("DOM extract target. element = only the leaf (fail on miss). "
                  "document = always dump documentSelector. "
                  "auto = leaf, then document on miss / page-edge / ancestor pick."),
        pd_enum("documentSelector", {"html", "body", "main"}).opt().dflt("html")
            .desc("Root used for scope=document and auto fallback. "
                  "body/main omit leftover <head> in format=html."),
        pd_enum("onDomFail", {"error", "text", "screen"}).adv().dflt("error")
            .desc("When format is md/html/selector and DOM inspect fails: "
                  "error = exit 1 (default); text = UIA spoken text; "
                  "screen = capture spec."),
        pd_bool("chromePreview").adv().dflt(false)
            .desc("Show live Chrome CSS details in the HUD after the cursor settles. "
                  "Uses blocking extension IPC and may feel slow on some machines."),
        pd_bool("highlight").opt().dflt(true)
            .desc("Show the translucent highlight overlay while picking."),
        pd_bool("hud").opt().dflt(true)
            .desc("Show the informational picker HUD. Off keeps rectangle highlighting."),
        pd_bool("resolveUia").opt().dflt(true)
            .desc("Resolve UIA after the cursor settles and on commit (gives sub-window "
                  "precision for web content, media players, etc.)."),
        pd_bool("showHidden").opt().dflt(true)
            .desc("Consider non-visible child windows (needed to reach "
                  "Chrome_RenderWidgetHostHWND etc.)."),
        pd_bool("siblings").opt().dflt(true)
            .desc("Include sibling panes and ∪siblings union in the Alt+wheel / [ ] "
                  "stack (leaf → siblings → parent). Off = ancestors only."),
        pd_bool("sampleColor").opt().dflt(false)
            .desc("Region picks only: add colorHex/rgb to the payload (sampled once at "
                  "click). For eyedropper use format=color instead."),
        pd_bool("magnifier").opt().dflt(false)
            .desc("Region/measure picks: open the 8× loupe (Z toggles)."),
        pd_bool("colorMagnifier").adv().dflt(true)
            .desc("Color picks: show the live 8× loupe. Disable on slow software "
                  "GDI/DWM machines; pixel sampling still works."),
        pd_int("textMaxChars").adv().dflt(0).range(0, 256 * 1024)
            .desc("Chrome DOM plain-text cap (innerText/speakText). 0 = unlimited (default). "
                  "Set e.g. 8192 to truncate long articles."),
        pd_int("htmlMaxChars").adv().dflt(8 * 1024 * 1024).range(0, 32 * 1024 * 1024)
            .desc("Cap cleaned HTML (format=html and the html field used for md). "
                  "0 = unlimited. Default 8 MiB (probe default)."),
        pd_int("markdownMaxChars").adv().dflt(2 * 1024 * 1024).range(0, 32 * 1024 * 1024)
            .desc("Cap markdown after conversion (format=md). 0 = unlimited. "
                  "Default 2 MiB (probe default)."),
        pd_int("timeoutMs").adv().grp("capture_options").dflt(60000).range(1000, 600000)
            .desc("Abort the pick and return an error after this many milliseconds "
                  "of inactivity. Default 60 s."),
        pd_store_as(),
    };
}

// ── CF_HDROP file-clipboard helpers ──────────────────────────────────────────

bool set_clipboard_files(const std::vector<std::wstring>& paths, std::string& err)
{
    size_t total = 1;  // trailing double-NUL
    for (const auto& p : paths) total += p.size() + 1;
    const size_t mem_size = sizeof(DROPFILES) + total * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GHND, mem_size);
    if (!mem) { err = "GlobalAlloc failed"; return false; }
    auto* df = static_cast<DROPFILES*>(::GlobalLock(mem));
    if (!df) { ::GlobalFree(mem); err = "GlobalLock failed"; return false; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;
    df->pt     = {};
    df->fNC    = FALSE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dst, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    *dst = L'\0';
    ::GlobalUnlock(mem);
    if (!::OpenClipboard(nullptr)) { ::GlobalFree(mem); err = "OpenClipboard failed"; return false; }
    struct ClipGuard { ~ClipGuard() { ::CloseClipboard(); } } cg;
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_HDROP, mem)) { ::GlobalFree(mem); err = "SetClipboardData(CF_HDROP) failed"; return false; }
    return true;
}

bool get_clipboard_files(std::vector<std::wstring>& out, std::string& err)
{
    if (!::OpenClipboard(nullptr)) { err = "OpenClipboard failed"; return false; }
    struct ClipGuard { ~ClipGuard() { ::CloseClipboard(); } } cg;
    HANDLE h = ::GetClipboardData(CF_HDROP);
    if (!h) { err = "clipboard does not contain files (CF_HDROP)"; return false; }
    const auto* df = static_cast<const DROPFILES*>(::GlobalLock(h));
    if (!df) { err = "GlobalLock failed"; return false; }
    struct LockGuard { HANDLE h; ~LockGuard() { ::GlobalUnlock(h); } } lg{h};
    if (df->fWide) {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(df) + df->pFiles);
        while (*p) {
            std::wstring path;
            while (*p) path.push_back(*p++);
            out.push_back(std::move(path));
            ++p;
        }
    } else {
        const char* p = reinterpret_cast<const char*>(
            reinterpret_cast<const BYTE*>(df) + df->pFiles);
        while (*p) {
            const char* end = p;
            while (*end) ++end;
            const int n = ::MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(end - p), nullptr, 0);
            if (n > 0) {
                std::wstring wide(static_cast<std::size_t>(n), L'\0');
                ::MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(end - p), &wide[0], n);
                out.push_back(std::move(wide));
            }
            p = end + 1;
        }
    }
    return true;
}

// ── clipboardCopy ─────────────────────────────────────────────────────────────
// Optionally sends Ctrl+C to a target window (or the focused element when no
// selector is given), waits delayMs, then reads and returns the clipboard content.
// type=text  → CF_UNICODETEXT → result string
// type=files → CF_HDROP      → result JSON array of paths

BlockResult clipboard_copy_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id       = jstr(block, "id");
    const std::string type     = jstr(block, "type", "text");
    const bool trigger         = jbool(block, "trigger", true);
    const int  delay_ms        = std::clamp(jint(block, "delayMs", 80), 0, 10000);

    if (trigger) {
        HWND activated = nullptr;
        std::string err;
        if (!activate_target_if_specified(io, activated, err))
            return app_err(io, "clipboardCopy", "clipboardCopy: " + err, id);
        std::string hotkey_err;
        if (!au::send_hotkey({L"ctrl", L"c"}, hotkey_err))
            return app_err(io, "clipboardCopy", "clipboardCopy: Ctrl+C failed: " + hotkey_err, id);
        if (delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    std::string err;
    if (type == "files") {
        std::vector<std::wstring> files;
        if (!get_clipboard_files(files, err))
            return app_err(io, "clipboardCopy", "clipboardCopy: " + err, id);
        nlohmann::json result = nlohmann::json::array();
        for (const auto& f : files) result.push_back(w2u(f));
        nlohmann::json payload{{"tool","clipboard_copy"}, {"type","files"},
                               {"count", static_cast<int>(files.size())}, {"files", result}};
        return app_ok(io, "clipboardCopy",
                      std::to_string(files.size()) + " file(s) on clipboard",
                      std::move(payload), id, std::move(result));
    } else {
        std::wstring text;
        if (!au::get_clipboard_text(text, err))
            return app_err(io, "clipboardCopy", "clipboardCopy: " + err, id);
        const std::string utf8 = w2u(text);
        nlohmann::json payload{{"tool","clipboard_copy"}, {"type","text"},
                               {"length", static_cast<int>(utf8.size())}};
        return app_ok(io, "clipboardCopy",
                      "copied " + std::to_string(utf8.size()) + " char(s)",
                      std::move(payload), id, utf8);
    }
}

// ── clipboardPaste ────────────────────────────────────────────────────────────
// Optionally writes text or file paths to the clipboard, then sends Ctrl+V to a
// target window (or the focused element when no selector is given).
// If no content param is supplied, the current clipboard content is pasted as-is
// and returned as the result (useful for "paste whatever is on clipboard").

BlockResult clipboard_paste_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id    = jstr(block, "id");
    const std::string type  = jstr(block, "type", "text");
    const bool trigger      = jbool(block, "trigger", true);
    std::string err;

    bool has_content = false;
    nlohmann::json result_value;

    if (type == "files") {
        // io.json() goes through the resolver; falls back to raw if unresolved
        const nlohmann::json& files_param = io.json("files");
        if (files_param.is_array() && !files_param.empty()) {
            std::vector<std::wstring> paths;
            for (const auto& f : files_param)
                if (f.is_string() && !f.get<std::string>().empty())
                    paths.push_back(u2w(f.get<std::string>()));
            if (!paths.empty()) {
                if (!set_clipboard_files(paths, err))
                    return app_err(io, "clipboardPaste", "clipboardPaste: " + err, id);
                result_value = nlohmann::json::array();
                for (const auto& p : paths) result_value.push_back(w2u(p));
                has_content = true;
            }
        }
    } else {
        // io.str() respects the .prev() hint — picks up PREVIOUS when text is blank
        const std::string text = io.str("text");
        if (!text.empty()) {
            if (!au::set_clipboard_text(u2w(text), err))
                return app_err(io, "clipboardPaste", "clipboardPaste: " + err, id);
            result_value = text;
            has_content = true;
        }
    }

    if (trigger) {
        HWND activated = nullptr;
        if (!activate_target_if_specified(io, activated, err))
            return app_err(io, "clipboardPaste", "clipboardPaste: " + err, id);
        std::string hotkey_err;
        if (!au::send_hotkey({L"ctrl", L"v"}, hotkey_err))
            return app_err(io, "clipboardPaste", "clipboardPaste: Ctrl+V failed: " + hotkey_err, id);
    }

    // When no content param was provided, report back what's currently on the clipboard
    // (the content that was just pasted into the target).
    if (!has_content) {
        if (type == "files") {
            std::vector<std::wstring> files;
            if (!get_clipboard_files(files, err))
                return app_err(io, "clipboardPaste", "clipboardPaste: " + err, id);
            result_value = nlohmann::json::array();
            for (const auto& f : files) result_value.push_back(w2u(f));
        } else {
            std::wstring text;
            if (!au::get_clipboard_text(text, err))
                return app_err(io, "clipboardPaste", "clipboardPaste: " + err, id);
            result_value = w2u(text);
        }
    }

    const std::string msg = has_content
        ? (type == "files" ? std::to_string(result_value.size()) + " file(s) pasted" : "text pasted")
        : (trigger ? "Ctrl+V sent" : "clipboard read");
    nlohmann::json payload{{"tool","clipboard_paste"}, {"type",type}, {"trigger",trigger}};
    return app_ok(io, "clipboardPaste", msg, std::move(payload), id, std::move(result_value));
}

#else // !(_WIN32 && FEATURE_ASSISTANT)

// Stub handler: emits a clear error so cross-platform fixtures fail fast.
BlockResult app_unavailable_block(BlockIO& io)
{
    const nlohmann::json& block = io.raw();
    const std::string id = jstr(block, "id");
    const std::string kind = jstr(block, "kind");
    return app_err(io, kind.empty() ? std::string("app") : kind,
                   "app.* blocks require Windows + FEATURE_ASSISTANT", id);
}

#endif // _WIN32 && FEATURE_ASSISTANT

struct AppBlockSpec {
    const char* kind;
    const char* label;
    const char* description;
    BlockHandler handler;
    nlohmann::json (*default_block)();
    std::vector<ParamDef> (*params)();
};

// Window selectors shared by most app blocks.
std::vector<ParamDef> selector_params()
{
    return {
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_int("pid").in().desc("Exact process id."),
        pd_int("hwnd").in().desc("Exact window handle."),
        pd_bool("foreground").in().dflt(false).desc("Target the current foreground window."),
    };
}

void append(std::vector<ParamDef>& dst, std::vector<ParamDef> more)
{
    for (auto& p : more)
        dst.push_back(std::move(p));
}

#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT

nlohmann::json app_activate_default()      { return {{"kind", "appActivate"}, {"title", "Notepad"}}; }
std::vector<ParamDef> app_activate_params(){ auto p = selector_params(); p.push_back(pd_store_as()); return p; }

nlohmann::json app_open_default()          { return {{"kind", "appOpen"}, {"exe", "notepad.exe"}, {"waitMs", 3000}}; }
std::vector<ParamDef> app_open_params()
{
    return {
        pd_str("exe").in().req().dflt("notepad.exe").desc("Executable to launch."),
        pd_str("args").in().dflt("").desc("Command-line arguments."),
        pd_str("cwd").in().dflt("").desc("Working directory."),
        pd_int("x").opt().desc("Initial window X."),
        pd_int("y").opt().desc("Initial window Y."),
        pd_int("width").opt().desc("Initial window width."),
        pd_int("height").opt().desc("Initial window height."),
        pd_int("waitMs").adv().dflt(3000).range(0, 120000).desc("Wait this long for the main window."),
        pd_store_as(),
    };
}

nlohmann::json app_close_default()         { return {{"kind", "appClose"}, {"title", "Notepad"}, {"force", false}}; }
std::vector<ParamDef> app_close_params()
{
    auto p = selector_params();
    append(p, {pd_bool("force").opt().dflt(false).desc("TerminateProcess instead of WM_CLOSE."), pd_store_as()});
    return p;
}

nlohmann::json app_screenshot_default()
{
    return {{"kind", "appScreenshot"}, {"title", "Notepad"}, {"outputPath", "out/shot.jpg"}, {"quality", 85}};
}
std::vector<ParamDef> app_screenshot_params()
{
    return {
        pd_outpath("outputPath").in().req().dflt("out/shot.jpg").desc("JPEG output path."),
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_bool("foreground").in().dflt(false).desc("Capture the foreground window."),
        pd_str("rect").opt().dflt("").desc("Optional 'x,y,w,h' screen rect."),
        pd_int("elementIndex").opt().desc("Capture a specific UIA element rect."),
        pd_int("quality").opt().dflt(85).range(1, 100).desc("JPEG quality."),
        pd_bool("activate").opt().dflt(true).desc("Activate the window before capture."),
        pd_store_as(),
    };
}

nlohmann::json app_click_default()
{
    return {{"kind", "appClick"}, {"title", "Notepad"}, {"xw", 100}, {"yw", 100}, {"button", "left"}, {"count", 1}};
}
std::vector<ParamDef> app_click_params()
{
    return {
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_int("x").in().desc("Absolute screen X."),
        pd_int("y").in().desc("Absolute screen Y."),
        pd_int("xw").in().desc("Window-relative X (preferred)."),
        pd_int("yw").in().desc("Window-relative Y."),
        pd_str("button").opt().dflt("left").desc("left | right | middle."),
        pd_int("count").opt().dflt(1).range(1, 10).desc("Click count (2 = double)."),
        pd_bool("virtual").adv().dflt(false).desc("PostMessage click; for background apps."),
        pd_store_as(),
    };
}

nlohmann::json app_mouse_move_default()
{
    return {{"kind", "appMouseMove"}, {"title", "Notepad"}, {"xw", 200}, {"yw", 200}, {"smooth", true}};
}
std::vector<ParamDef> app_mouse_move_params()
{
    return {
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_int("x").in().desc("Absolute screen X."),
        pd_int("y").in().desc("Absolute screen Y."),
        pd_int("xw").in().desc("Window-relative X."),
        pd_int("yw").in().desc("Window-relative Y."),
        pd_bool("smooth").opt().dflt(true).desc("Smooth (auto-paced) move."),
        pd_int("durationMs").opt().dflt(-1).desc("-1 = auto from distance, 0 = teleport."),
        pd_store_as(),
    };
}

nlohmann::json app_type_default()          { return {{"kind", "appType"}, {"title", "Notepad"}, {"text", "Hello"}}; }
std::vector<ParamDef> app_type_params()
{
    return {
        pd_str("text").in().req().dflt("Hello").desc("Text to type."),
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_bool("foreground").in().dflt(false).desc("Type into the foreground window."),
        pd_store_as(),
    };
}

nlohmann::json app_hotkey_default()        { return {{"kind", "appHotkey"}, {"title", "Notepad"}, {"keys", "ctrl+s"}}; }
std::vector<ParamDef> app_hotkey_params()
{
    return {
        pd_str("keys").in().req().dflt("ctrl+s").desc("Combo, e.g. ctrl+shift+t."),
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_bool("foreground").in().dflt(false).desc("Send to the foreground window."),
        pd_store_as(),
    };
}

nlohmann::json app_key_default()           { return {{"kind", "appKey"}, {"key", "a"}, {"holdMs", 100}}; }
std::vector<ParamDef> app_key_params()
{
    return {
        pd_str("key").in().req().dflt("a").desc("Single key to press."),
        pd_json("modifiers").in().desc("Modifier list: ctrl/shift/alt/win."),
        pd_int("holdMs").opt().dflt(0).range(0, 60000).desc("Note-sustain for piano-style apps."),
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_bool("foreground").in().dflt(false).desc("Send to the foreground window."),
        pd_store_as(),
    };
}

nlohmann::json app_drag_default()
{
    return {{"kind", "appDrag"}, {"title", "Notepad"}, {"xw", 100}, {"yw", 100},
            {"toXw", 300}, {"toYw", 200}, {"durationMs", 250}};
}
std::vector<ParamDef> app_drag_params()
{
    return {
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_int("x").in().desc("Start X (screen)."),
        pd_int("y").in().desc("Start Y (screen)."),
        pd_int("xw").in().desc("Start X (window-relative)."),
        pd_int("yw").in().desc("Start Y (window-relative)."),
        pd_int("toX").in().desc("End X (screen)."),
        pd_int("toY").in().desc("End Y (screen)."),
        pd_int("toXw").in().desc("End X (window-relative)."),
        pd_int("toYw").in().desc("End Y (window-relative)."),
        pd_int("dx").in().desc("Relative end X delta."),
        pd_int("dy").in().desc("Relative end Y delta."),
        pd_json("path").in().desc("Polyline: [{x,y}|{xw,yw}, ...]."),
        pd_json("arc").in().desc("{cx,cy,radius,startDeg,endDeg,segments}."),
        pd_str("button").opt().dflt("left").desc("left | right | middle."),
        pd_int("durationMs").opt().dflt(250).range(0, 30000).desc("Drag duration."),
        pd_store_as(),
    };
}

nlohmann::json app_scroll_default()        { return {{"kind", "appScroll"}, {"title", "Notepad"}, {"clicks", -3}}; }
std::vector<ParamDef> app_scroll_params()
{
    return {
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_int("clicks").in().req().dflt(-3).desc("Wheel clicks; positive = up/right."),
        pd_str("axis").opt().dflt("vertical").desc("vertical | horizontal."),
        pd_int("x").in().desc("Pre-target cursor X (optional)."),
        pd_int("y").in().desc("Pre-target cursor Y (optional)."),
        pd_int("xw").in().desc("Pre-target cursor X (window-relative)."),
        pd_int("yw").in().desc("Pre-target cursor Y (window-relative)."),
        pd_store_as(),
    };
}

nlohmann::json app_batch_default()
{
    return {{"kind", "appBatch"},
            {"defaultDelayMs", 50},
            {"steps", nlohmann::json::array({
                {{"action", "activate"}, {"title", "Notepad"}},
                {{"action", "type"}, {"text", "Hello"}},
            })}};
}
std::vector<ParamDef> app_batch_params()
{
    return {
        pd_json("steps").in().req().desc("Array of {action, ...} steps."),
        pd_int("defaultDelayMs").opt().dflt(50).range(0, 60000).desc("Delay between steps."),
        pd_int("defaultWaitTimeoutMs").adv().dflt(5000).range(0, 600000).desc("Default wait timeout."),
        pd_int("defaultWaitIntervalMs").adv().dflt(100).range(1, 60000).desc("Default wait poll interval."),
        pd_bool("continueOnError").opt().dflt(false).desc("Keep running steps after a failure."),
        pd_store_as(),
    };
}

nlohmann::json app_inspect_dump_default()  { return {{"kind", "appInspectDump"}, {"input", ""}, {"title", "Notepad"}, {"format", "md"}}; }
std::vector<ParamDef> app_inspect_dump_params()
{
    return {
        pd_screen_input("input").in().prev().dflt("")
            .desc("Picker / screen spec (e.g. from a Picker block). "
                  "Extracts hwnd and title; overrides the title/process fields when set."),
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_bool("foreground").in().dflt(false).desc("Dump the foreground window."),
        pd_enum("format", {"md","json"}).opt().dflt("md").desc("Output format."),
        pd_int("limit").opt().dflt(0).range(0, 500000).desc("Max elements per window. 0 = no limit."),
        pd_bool("filterToSelection").opt().dflt(false)
            .desc("When the input spec contains rrect= (sub-element pick), filter the output "
                  "to elements whose center falls inside that rect. Off by default — rrect is "
                  "used only to identify the window."),
        pd_str("controls").adv().dflt("all")
            .desc("Control-type filter for markdown output. "
                  "\"all\" = every element (full tree). "
                  "Empty = smart default (invokable + menus + inputs). "
                  "CSV of types to include, e.g. buttons,menus,editable."),
        pd_int("textMaxChars").adv().dflt(1200).range(0, 1000000).desc("Truncate element text in markdown output."),
        pd_bool("probeCells").adv().dflt(false)
            .desc("Probe grid / table cells (DataGrid, Table). Needed for spreadsheets like "
                  "LibreOffice Calc or Excel to enumerate individual cells."),
        pd_store_as(),
    };
}

nlohmann::json app_inspect_find_default()  { return {{"kind", "appInspectFind"}, {"title", "Notepad"}, {"name", "File"}}; }
std::vector<ParamDef> app_inspect_find_params()
{
    return {
        pd_str("title").in().dflt("").desc("Window-title substring."),
        pd_str("process").in().dflt("").desc("Process-name substring."),
        pd_bool("foreground").in().dflt(false).desc("Search the foreground window."),
        pd_str("name").in().dflt("").desc("Element name substring."),
        pd_str("value").in().dflt("").desc("Element value substring."),
        pd_str("automationId").in().dflt("").desc("AutomationId substring."),
        pd_str("className").in().dflt("").desc("ClassName substring."),
        pd_int("nth").opt().dflt(-1).desc("Select the nth match (-1 = all)."),
        pd_store_as(),
    };
}

nlohmann::json clipboard_copy_default()
{
    return {{"kind","clipboardCopy"}, {"type","text"}, {"trigger",true}, {"delayMs",80}};
}
std::vector<ParamDef> clipboard_copy_params()
{
    auto p = selector_params();
    append(p, {
        pd_enum("type", {"text","files"}).in().dflt("text")
            .desc("Clipboard format to read back: text (CF_UNICODETEXT) or files (CF_HDROP)."),
        pd_bool("trigger").in().dflt(true)
            .desc("Send Ctrl+C to the target (or focused element) before reading. "
                  "Set false to read clipboard without triggering a copy."),
        pd_int("delayMs").opt().dflt(80).range(0, 10000)
            .desc("Wait this long (ms) after Ctrl+C before reading the clipboard. "
                  "Increase for slow apps."),
        pd_store_as(),
    });
    return p;
}

nlohmann::json clipboard_paste_default()
{
    return {{"kind","clipboardPaste"}, {"type","text"}, {"text",""}, {"files", nlohmann::json::array()}, {"trigger",true}};
}
std::vector<ParamDef> clipboard_paste_params()
{
    auto p = selector_params();
    append(p, {
        pd_enum("type", {"text","files"}).in().dflt("text")
            .desc("Clipboard format to write: text (CF_UNICODETEXT) or files (CF_HDROP)."),
        pd_str("text").in().prev().dflt("")
            .desc("Text to put on the clipboard before pasting (type=text). "
                  "Uses PREVIOUS when blank. Leave empty to paste whatever is already on clipboard."),
        pd_json("files").in().dflt(nlohmann::json::array())
            .desc("File paths to put on the clipboard before pasting (type=files). "
                  "Leave empty to paste whatever is already on clipboard."),
        pd_bool("trigger").in().dflt(true)
            .desc("Send Ctrl+V to the target (or focused element) after writing. "
                  "Set false to only write to clipboard without triggering a paste."),
        pd_store_as(),
    });
    return p;
}

const std::vector<AppBlockSpec>& app_block_specs()
{
    static const std::vector<AppBlockSpec> specs = {
        {"appActivate",     "App Activate",      "Bring a target window to the foreground.",
            app_activate_block,     app_activate_default,     app_activate_params},
        {"appOpen",         "App Open",          "Launch an executable and (optionally) wait for its window.",
            app_open_block,         app_open_default,         app_open_params},
        {"appClose",        "App Close",         "Send WM_CLOSE (or TerminateProcess with force=true).",
            app_close_block,        app_close_default,        app_close_params},
        {"appScreenshot",   "App Screenshot",    "Capture a window / element / explicit rect to a JPEG file.",
            app_screenshot_block,   app_screenshot_default,   app_screenshot_params},
        {"appClick",        "App Click",         "Click at (x,y) or (xw,yw) on a target window.",
            app_click_block,        app_click_default,        app_click_params},
        {"appMouseMove",    "App Mouse Move",    "Smoothly move the cursor (auto-paced) or teleport.",
            app_mouse_move_block,   app_mouse_move_default,   app_mouse_move_params},
        {"appType",         "App Type",          "Type a string into the activated window.",
            app_type_block,         app_type_default,         app_type_params},
        {"appHotkey",       "App Hotkey",        "Send a key combination (e.g. ctrl+shift+t).",
            app_hotkey_block,       app_hotkey_default,       app_hotkey_params},
        {"appKey",          "App Key",           "Press one key with optional modifiers and hold duration.",
            app_key_block,          app_key_default,          app_key_params},
        {"appDrag",         "App Drag",          "Drag linear / polyline / arc with smooth pacing.",
            app_drag_block,         app_drag_default,         app_drag_params},
        {"appScroll",       "App Scroll",        "Wheel scroll (vertical or horizontal) at an optional point.",
            app_scroll_block,       app_scroll_default,       app_scroll_params},
        {"appBatch",        "App Batch",         "Run a sequence of app_batch actions in one block.",
            app_batch_block,        app_batch_default,        app_batch_params},
        {"appInspectDump",  "App Inspect Dump",  "Dump UI Automation tree of matching windows.",
            app_inspect_dump_block, app_inspect_dump_default, app_inspect_dump_params},
        {"appInspectFind",  "App Inspect Find",  "Filter UIA elements by name/value/automationId/className.",
            app_inspect_find_block, app_inspect_find_default, app_inspect_find_params},
        {"clipboardCopy",   "Clipboard Copy",
            "Send Ctrl+C to the focused or targeted element, then return the clipboard content "
            "(text or file paths). No target selector = operates on the currently focused element.",
            clipboard_copy_block,  clipboard_copy_default,  clipboard_copy_params},
        {"clipboardPaste",  "Clipboard Paste",
            "Write text or file paths to the clipboard, then send Ctrl+V to the focused or targeted "
            "element. No target selector = operates on the currently focused element.",
            clipboard_paste_block, clipboard_paste_default, clipboard_paste_params},
        {"picker",          "Picker",
            "Interactive window / element picker with live highlight + stack HUD. "
            "Click to pick the highlighted region; Alt+wheel or [ ] cycles leaf → "
            "siblings → ∪siblings → parent; drag (or Shift/Ctrl+drag) selects a free "
            "rectangle; Z opens an 8× magnifier, C copies the sampled colour; ESC cancels. "
            "Default output is a screen spec for video/screenshot blocks; set format=md or "
            "format=html for Chrome DOM via the Tanit extension. format=selection captures "
            "the current focused selection (UIA first, Chrome extension fallback) without "
            "an interactive click. format=foreground returns JSON for the last focused "
            "window (title, hwnd, cursor parent HWND) — remembers the previous app when "
            "Tanit is in front. format=context returns Explorer/Desktop/drag sources from "
            "the process-local context store (no click). Sampled colour and Chrome DOM selectors always ride along "
            "in the payload.",
            picker_block, picker_default, picker_params},
    };
    return specs;
}

#else

const std::vector<AppBlockSpec>& app_block_specs()
{
    static const std::vector<AppBlockSpec> specs; // empty
    return specs;
}

#endif

} // namespace

BlockHandler app_block_handler(const std::string& kind)
{
    for (const auto& spec : app_block_specs()) {
        if (kind == spec.kind)
            return spec.handler;
    }
    return nullptr;
}

void register_app_blocks(BlockRegistry& registry)
{
    for (const auto& spec : app_block_specs()) {
        auto descriptor = bdb(
            spec.kind,
            spec.handler,
            spec.label,
            "App",
            spec.description,
            spec.default_block ? spec.default_block() : nlohmann::json::object())
            .params(spec.params ? spec.params() : std::vector<ParamDef>{})
            .exec({false, false, true});
        if (spec.kind == "picker")
            descriptor.primary_output(ParamKind::screen_input, "Screen spec");
        else
            descriptor.primary_output(ParamKind::json_value, "Result");
        registry[spec.kind] = std::move(descriptor);
    }
}

} // namespace media::xblox::blocks
