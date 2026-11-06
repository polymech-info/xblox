# Back to Win32: Shell Integration Examples

These excerpts show the shell path: Explorer invokes a COM shell extension, the extension collects selected paths, then spawns the main app with normal CLI-style arguments.

Source files:

- `src/win/shell/pm_image_iexecute.cpp`
- `src/win/register_explorer.cpp`

## Shell Extension Intent

```cpp
/**
 * In-proc shell extension: IExecuteCommand + IObjectWithSelection.
 * https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iexecutecommand
 * CLSID -> payload: HKCU\...\IExecuteMap\<GUID>\ (see pm::brand; written by register-explorer).
 *
 * Explorer "Chat..." / "Open-with PM Viewer":
 *   spawns `pm-image.exe --ui-preset=viewer --src "<;joined paths>"`
 * with no PowerShell middleman. Shell verbs use DelegateExecute to this DLL;
 * install with `pm-image register-explorer`, not ad-hoc registry edits.
 */
```

## Locate the Main Executable

```cpp
static std::wstring get_pm_image_exe() {
    if (!g_hThis) return {};
    wchar_t mod[MAX_PATH]{};
    if (!GetModuleFileNameW(g_hThis, mod, MAX_PATH)) return {};
    if (!PathRemoveFileSpecW(mod)) return {};
    std::wstring p = mod;
    p += L"\\";
    p += pm::brand::k_exe_basename_w;
    if (PathFileExistsW(p.c_str())) return p;
    p = mod;
    p += L"\\..\\";
    p += pm::brand::k_exe_basename_w;
    wchar_t full[MAX_PATH]{};
    if (GetFullPathNameW(p.c_str(), MAX_PATH, full, nullptr) && PathFileExistsW(full)) return full;
    return std::wstring(mod) + L"\\" + std::wstring(pm::brand::k_exe_basename_w);
}
```

## Preserve Folder as One Logical Selection

```cpp
/** "Open in app" / Chat: pass directories as a single path (app expands in-queue). Do not
 *  enumerate the tree in the shell - huge lists exceed CreateProcess cmdline limits and
 *  the UI seed must stay one logical folder. */
static void add_open_ui_path(const std::wstring& path, std::vector<std::wstring>* out) {
    std::error_code ec;
    if (path.empty()) return;
    const fs::path p = path;
    if (!fs::exists(p, ec) || ec) return;
    if (fs::is_directory(p, ec) && !ec) {
        out->push_back(p.lexically_normal().wstring());
        return;
    }
    if (fs::is_regular_file(p, ec) && !ec) {
        if (is_image_ext(p.extension().wstring())) out->push_back(p.lexically_normal().wstring());
    }
}
```

## Explorer Registration Logging

```cpp
void reg_explorer_log_line(const std::string& msg) {
    const std::string full = "[register-explorer] " + msg;
    logger::info(full);
    std::cerr << full << "\n";
    try {
        std::error_code ec;
        const fs::path dir  = media::settings::get_config_dir();
        fs::create_directories(dir, ec);
        const fs::path logf = dir / "register-explorer.log";
        std::ofstream     out(logf, std::ios::app | std::ios::binary);
        if (!out)
            return;
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        char ts[48]{};
        sprintf_s(ts, "%04u-%02u-%02u %02u:%02u:%02u  ", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
        out << ts << msg << "\n";
    } catch (...) {
    }
}
```
