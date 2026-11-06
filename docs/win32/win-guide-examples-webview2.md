# Back to Win32: WebView2 Examples

These excerpts show WebView2 used as a focused rich panel, with native code still owning files, theme, drag-and-drop, and the host bridge.

Source files:

- `src/win/ui_next/ChatWebPanel.cpp`
- `src/win/viewers/text/ViewerWebPanel.cpp`

## Dedicated User Data Folder

```cpp
std::wstring webview2_user_data_folder() {
    try {
        fs::path p = media::settings::get_config_dir() / "web";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p.wstring();
    } catch (...) {
        return L"";
    }
}
```

## Stable Origin for Local Storage

```cpp
// NavigateToString() loads a document with an opaque / null origin. Serving the
// same bundle from a stable https virtual host gives it durable localStorage.
static bool install_chat_web_virtual_host(ICoreWebView2* webview, const std::string& utf8_html) {
    std::wstring werr;
    if (!write_chat_html_file(utf8_html, werr)) {
        logger::warn(std::string("[chat-web] bundle write failed: ") + pmui::wide_to_utf8(werr)
                     + " - localStorage will not persist (NavigateToString fallback).");
        return false;
    }
    ComPtr<ICoreWebView2>   w0(webview);
    ComPtr<ICoreWebView2_3> v3;
    if (FAILED(w0.As(&v3)) || !v3)
        return false;
    const std::wstring folder = chat_web_content_folder();
    const HRESULT hrMap
        = v3->SetVirtualHostNameToFolderMapping(pm::brand::k_chat_web_vhost_w, folder.c_str(),
                                                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    return SUCCEEDED(hrMap);
}
```

## Match Native Theme Background

```cpp
static void apply_webview2_match_theme_background(ICoreWebView2Controller* ctrl)
{
    if (!ctrl) return;
    ComPtr<ICoreWebView2Controller2> c2;
    if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&c2))) || !c2)
        return;
    const auto& pal = pmui::theme_palette();
    COREWEBVIEW2_COLOR col{};
    col.A = 255;
    col.R = GetRValue(pal.web_surface_bg);
    col.G = GetGValue(pal.web_surface_bg);
    col.B = GetBValue(pal.web_surface_bg);
    c2->put_DefaultBackgroundColor(col);
}

static void apply_webview2_preferred_color_scheme(ICoreWebView2* webview)
{
    if (!webview) return;
    ComPtr<ICoreWebView2_13>        w13;
    ComPtr<ICoreWebView2Profile>    prof;
    if (FAILED(webview->QueryInterface(IID_PPV_ARGS(&w13))) || !w13) return;
    if (FAILED(w13->get_Profile(&prof)) || !prof) return;
    const bool dark = pmui::theme_palette().dark;
    const COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme = dark
        ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
        : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT;
    (void)prof->put_PreferredColorScheme(scheme);
}
```

## Native Drop Target for Explorer Files

```cpp
void CChatWebView::Impl::EnsureShellFileDropTarget()
{
    if (!m_hostHwnd || !::IsWindow(m_hostHwnd) || !controller)
        return;

    HWND dropHwnd = find_largest_chromium_render_widget_under(m_hostHwnd);
    if (!dropHwnd)
        return;

    if (dropHwnd == shell_drop_hwnd && shell_drop_target)
        return;

    if (shell_drop_hwnd && ::IsWindow(shell_drop_hwnd)) {
        (void)::RevokeDragDrop(shell_drop_hwnd);
        shell_drop_hwnd = nullptr;
    }

    if (!shell_drop_target)
        shell_drop_target = new ChatShellDropTarget(this);

    const HRESULT hr = ::RegisterDragDrop(dropHwnd, shell_drop_target);
    if (FAILED(hr)) {
        logger::warn(std::string("[chat-web] RegisterDragDrop (shell HDROP) failed: HRESULT 0x")
                     + std::to_string(static_cast<uint32_t>(hr)));
        return;
    }
    shell_drop_hwnd = dropHwnd;
}
```

## Viewer State Bridge

```cpp
void FlushContextToWeb()
{
    nlohmann::json j;
    j["selection"] = nlohmann::json::array();
    for (const auto& w : selection)
        j["selection"].push_back(pmui::wide_to_utf8(w));
    j["folder"]      = pmui::wide_to_utf8(folder);
    j["viewerKind"]  = viewer_kind_utf8;
    j["features"]    = nlohmann::json::object();

    const auto& pal = pmui::theme_palette();
    j["features"]["theme"] = pal.dark ? "dark" : "light";

    std::wstring js = L"if(window.pmViewer){";
    js += L"window.pmViewer.setStatus(" + pmui::utf8_to_wide(j.dump()) + L");}";

    if (ready) RunJsNow(js);
    else pendingScripts.push_back(js);
}
```
