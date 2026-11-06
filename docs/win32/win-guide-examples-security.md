# Back to Win32: Security Examples

These excerpts show the filesystem guard pattern used for WebView2, LLM tools, shell dispatch, and internal preview paths.

Source files:

- `docs/web-shell.md`
- `src/llm/llm_fs_guard.hpp`
- `src/llm/llm_fs_guard.cpp`

## Web to Shell Boundary

```text
LLM response
   |
   v
ChatTranscript.tsx  (click handler)
   |  postHost({ kind, path/url })
   v
ChatWebPanel.cpp    (host message router)
   |  guard #1: llm_fs_guard_deny_reason  (paths)
   |  guard #1: path shape / list filtering
   v
pmui::shell::*      (default_shell.cpp)   <- guard #2 (always runs regardless of call site)
   |
   v
::ShellExecuteW / CFileViewer::OpenFile
```

## Shared Guard API

```cpp
namespace media::llm {

/// Returns a non-empty human-readable reason if LLM tools must not touch this path
/// (read, write, list root, or emit outputs here). Empty string means allowed.
/// Explorer -> dock preview (`CFileViewer::OpenFile`) uses the same check so the WebView
/// host does not map or read paths the tools are denied.
/// When the path exists, it is normalized with weakly_canonical first.
POLYMECH_API std::string llm_fs_guard_deny_reason(const std::filesystem::path& path);

/// Write targets only (`write_file`, image tool output paths). Blocks disallowed
/// extensions (scripts, installers, PDF/Office, archives, ...) and system prefixes;
/// image / video / audio extensions from the allowlist are permitted.
POLYMECH_API std::string llm_fs_guard_write_deny_reason(const std::filesystem::path& path);

} // namespace media::llm
```

## Write Guard

```cpp
std::string llm_fs_guard_write_deny_reason(const fs::path& path) {
    fs::path p = path;
    std::error_code ec;
    if (fs::exists(p, ec)) {
        const fs::path c = fs::weakly_canonical(p, ec);
        if (!ec && !c.empty()) p = c;
    } else {
        p = p.lexically_normal();
    }

    const std::string norm = norm_path_for_sensitive(p);
    if (write_abs_prefix_blocked_for_write(norm)) return "refusing write to system or protected location";

    std::string ext = p.extension().string();
    tolower_inplace(ext);
    if (write_ext_is_allowed_media(ext)) return {};
    if (write_tarball_suffix_blocked(p, ext)) return "refusing disallowed archive type for write";
    if (write_extension_in_blocklist(ext)) return "refusing disallowed file type for write";
    return {};
}
```

## Read / Touch Guard

```cpp
std::string llm_fs_guard_deny_reason(const fs::path& path) {
    fs::path p = path;
    std::error_code ec;
    if (fs::exists(p, ec)) {
        const fs::path c = fs::weakly_canonical(p, ec);
        if (!ec && !c.empty()) p = c;
    } else {
        p = p.lexically_normal();
    }

    if (filename_is_dotfile(p)) return "refusing dot-named path";
    if (path_blocked_by_prefix_list(p)) return "refusing sensitive path or filename";
    if (fs::exists(p, ec) && file_has_platform_hidden(p)) return "refusing hidden file";
    return {};
}
```

## Rule for New Web Host Messages

```text
1. Add the handler in ChatWebPanel.cpp.
2. For paths: call llm_fs_guard_deny_reason(path) and return early if non-empty.
3. For URLs: route through pmui::shell::open_url.
4. For file reads: guard again in the sub-loader before opening the file.
5. Never call ShellExecuteW directly from a handler.
```
