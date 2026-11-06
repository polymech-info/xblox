# Back to Win32: Architecture Examples

These excerpts show the core pattern from the codebase: a native library target first, then CLI and other surfaces layered on top.

Source files:

- `CMakeLists.txt`
- `src/core/resize.hpp`
- `src/cli/pm_image_entry.cpp`

## Core Library Target

```cmake
# -- pm-media library (resize + transform API)
option(POLYMECH_MEDIA_SHARED "Build pm-media as a shared library (DLL/so)" OFF)

set(_pm_media_sources
    src/core/cli_cancel.cpp
    src/core/batch_queue.cpp
    src/core/cache.cpp
    src/core/compress.cpp
    src/core/find.cpp
    src/core/duplicates.cpp
    src/core/glob_paths.cpp
    src/core/input_selection.cpp
    src/core/path_sanitizer.cpp
    src/core/mustache_render.cpp
    src/core/string_sanitizers.cpp
    src/core/llm_jpeg_from_path.cpp
    src/core/meta.cpp
    src/core/output_path.cpp
    src/core/resize.cpp
    src/core/transform.cpp
    src/core/url_fetch.cpp
    src/core/app_image_provider.cpp
    src/core/settings_portable.cpp
    src/core/settings_runtime.cpp
    src/core/app_exe_directory.cpp
    src/llm/agent.cpp
    src/llm/tool_catalog.cpp
    src/llm/tool_executor.cpp
    src/llm/llm_fs_guard.cpp
    src/llm/path_tool_catalog.cpp
    src/llm/path_tool_executor.cpp
)

if(POLYMECH_MEDIA_SHARED)
    add_library(pm-media SHARED ${_pm_media_sources})
    target_compile_definitions(pm-media PRIVATE POLYMECH_BUILDING_LIBRARY)
else()
    add_library(pm-media STATIC ${_pm_media_sources})
    target_compile_definitions(pm-media PRIVATE POLYMECH_STATIC_BUILD=1)
    target_compile_definitions(pm-media INTERFACE POLYMECH_STATIC_BUILD=1)
endif()

target_include_directories(pm-media PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(pm-media PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
    Vips::vips
    pranav_glob
)
```

## Plain Core API

```cpp
POLYMECH_API bool resize_file(const std::string& input_path, const std::string& output_path, const ResizeOptions& opt,
                              std::string& err_out);

struct ResizeBufferResult {
    bool        ok = false;
    std::string error;
    std::string mime;
    std::string bytes;
};

POLYMECH_API ResizeBufferResult resize_buffer(const void* in_data, std::size_t in_size,
                                              const ResizeOptions& opts);

struct ResizeBatchResult {
    int count = 0;
    std::vector<std::string> outputs;
};

POLYMECH_API bool resize_batch(const std::string& input_spec, const std::string& output_spec, const ResizeOptions& opt,
                               std::string& err_out, ResizeBatchResult* out_stats = nullptr,
                               const std::function<void(std::size_t, std::size_t)>& progress = {});

POLYMECH_API void apply_resize_options_from_json(const nlohmann::json& j, ResizeOptions& opt);
```

## Windows Entry, Normal CLI Core

```cpp
#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR /*lpCmdLine*/, int nShowCmd) {
    pmui::win32_register_startup_show_cmd(nShowCmd);
    int          w_argc   = 0;
    LPWSTR*      w        = ::CommandLineToArgvW(GetCommandLineW(), &w_argc);
    if (!w) return 1;
    std::vector<std::string> u8;
    u8.reserve(static_cast<size_t>(w_argc));
    for (int i = 0; i < w_argc; i++) {
        if (!w[i] || w[i][0] == 0) {
            u8.emplace_back();
        } else {
            const int    n  = ::WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string  s  = n > 1 ? std::string(static_cast<size_t>(n - 1), '\0') : std::string{};
            if (n > 1)
                (void)::WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), n, nullptr, nullptr);
            u8.push_back(std::move(s));
        }
    }
    ::LocalFree(w);

    std::vector<char*> a;
    a.reserve(static_cast<size_t>(w_argc) + 1u);
    for (int i = 0; i < w_argc; i++)
        a.push_back(const_cast<char*>(u8[static_cast<size_t>(i)].c_str()));
    a.push_back(nullptr);
    return pm_image_run(w_argc, a.data());
}
#else
int main(int argc, char** argv) { return pm_image_run(argc, argv); }
#endif
```
