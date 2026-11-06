# Back to Win32: CLI and Logging Examples

These excerpts show two practical support layers: CLI registration with `CLI11`, and UI/startup logging routed through the process logger with `spdlog`.

Source files:

- `src/cli/pm_image_register_cli.cpp`
- `src/win/ui_next/ui_log_file.hpp`
- `src/win/ui_next/ui_log_file.cpp`

## Global CLI Options

```cpp
void pm_image_register_cli(CLI::App& app, PmImageCliState& s) {
    app.add_option(
            "--log-level", s.log_level,
            "Global stderr log level for this process (spdlog): trace, debug, info, warn, error, critical, off. "
            "Applies to all subcommands (resize, serve, llm agent, ...). Default: info.")
        ->default_val("info")
        ->check(CLI::IsMember(
            {"trace", "debug", "info", "warn", "warning", "error", "err", "critical", "off", "none"},
            CLI::ignore_case));
    app.add_flag(
        "--no-gui", s.g_no_gui,
        "Windows: do not open the list-style job window for batch (use console; for scripts, tests, CI). "
        "Place before the subcommand (e.g. `pm-image --no-gui meta a.jpg b.jpg`).");
    app.add_flag(
        "--mcp,--no-mcp", s.mcp_enabled,
        "Enable the embedded MCP HTTP listener for this process (default: on). "
        "Bind host defaults to 0.0.0.0 on Windows and 127.0.0.1 elsewhere; set --mcp-bind to override. "
        "Env PM_IMAGE_MCP=0|false|off|no or =1|true|on|yes overrides this flag for CI.");
}
```

## Windows UI Launch Flags

```cpp
#if defined(_WIN32)
app.add_flag(
    "--console", s.win_console,
    "Windows: attach or allocate a console for stdout/stderr (default: off - GUI launches from Explorer, "
    "Open with, shell verbs, etc. do not open a console window).");
app.add_flag(
    "--ui-chat", s.ui_chat,
    "Open stand-alone native chat (minimal window; use with top-level --src for context). No subcommand.");
app.add_option(
        "--src", s.ui_chat_src_list,
        "With --ui-chat, or with --ui-preset and no subcommand: input path(s), repeat; `;` in a quoted value for several")
    ->expected(-1);
app.add_option(
        "--ui-preset", s.ui_preset,
        "No subcommand: workbench `main` (full tools), `chat` (chat-first), or `viewer` (fast image preview + menu/status, optional Explorer). "
        "Pair with top-level --src to seed; overrides `ui.workbench` for this run.")
    ->check(CLI::IsMember({"main", "chat", "viewer"}));
#endif
```

## CLI Subcommand Registration

```cpp
s.resize_cmd = app.add_subcommand("resize", "Resize / transform an image (libvips, Sharp-like options)");
s.resize_cmd->add_option("input", s.in_path, "Input path, glob (*, ?, **), or http(s):// URL")->required(false);
s.resize_cmd->add_option(
    "output", s.out_path,
    "Output file/dir, or omit when there is exactly one input -> write under cwd (sanitized name)");
s.resize_cmd->add_option("--src", s.src_list, "Input (repeat for multiple); use with --dst; Explorer passes several files")
    ->expected(-1);
s.resize_cmd->add_option("--dst", s.dst_flag, "Same as positional output; directory if multiple inputs");
s.resize_cmd->add_option("--max-width", s.max_w, "Target / max width (0 = no limit)");
s.resize_cmd->add_option("--max-height", s.max_h, "Target / max height (0 = no limit)");
s.resize_cmd->add_option("--format", s.format, "Output format (default: from extension)");
s.resize_cmd
    ->add_option("--fit", s.fit,
                 "inside|cover|contain|fill|outside - see Sharp resize.fit")
    ->default_val("inside");
```

## UI Log Session API

```cpp
namespace pmui {

/// Startup / layout / selection trace: logs to the default spdlog logger with channel
/// [startup]. Nested UiLogFileSession increments a depth counter; only the outermost
/// session registers tracing and emits the closing line.
void ui_log_file_begin_session();
void ui_log_file_end_session();
void ui_log_file_event(const char* message);
void ui_log_file_eventf(const char* fmt, ...);

struct UiLogFileSession {
    UiLogFileSession() { ui_log_file_begin_session(); }
    ~UiLogFileSession() { ui_log_file_end_session(); }
    UiLogFileSession(const UiLogFileSession&)            = delete;
    UiLogFileSession& operator=(const UiLogFileSession&) = delete;
};

} // namespace pmui
```

## Lazy Startup Logging

```cpp
void ui_log_file_event(const char* message)
{
    if (!message)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session_depth <= 0)
        return;
    const uint64_t ms = elapsed_ms_unlocked();
    pm::log::info_lazy("startup", [&] {
        char b[800];
        (void)std::snprintf(b, sizeof b, "%5llu ms  %s", static_cast<unsigned long long>(ms), message);
        return std::string(b);
    });
}
```

## Session End Flush

```cpp
void ui_log_file_end_session()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session_depth <= 0)
        return;
    --g_session_depth;
    if (g_session_depth > 0)
        return;

    media::settings::clear_settings_load_tracing();
    const int misses = g_load_count;
    const uint64_t ms = elapsed_ms_unlocked();
    pm::log::info_lazy("startup", [ms, misses] {
        return std::to_string(ms) + " ms  end session (load_settings cache-misses logged: " + std::to_string(misses)
               + ")";
    });
    logger::flush();
}
```
