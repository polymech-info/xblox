# Back to Win32: Win32 UI Examples

These excerpts show the practical dark-mode and control-styling work that sits behind a native Win32 UI.

Source files:

- `src/win/ui_next/Mainfrm.cpp`
- `src/win/ui_next/Mainfrm_layout.cpp`
- `src/win/ui_next/settings_controls.hpp`
- `docs/lazy-layout.md`
- `docs/dockers-layout.md`

## Appearance Pass

```cpp
void CMainFrame::ApplyAppearance(bool reread_settings)
{
    if (reread_settings) {
        pmui::ui_log_file_event("ApplyAppearance enter (re-reads font+theme, full panel refresh)");
        pmui::ui_font_init_from_settings();
        pmui::theme_init_from_settings();
    } else {
        pmui::ui_log_file_event("ApplyAppearance enter (cached font+theme, full panel refresh)");
    }
    const auto& pal = pmui::theme_palette();

    StatusBarTheme sbt{};
    sbt.UseThemes = TRUE;
    sbt.clrBkgnd1 = sbt.clrBkgnd2 = pal.window_bg;
    sbt.clrText   = pal.window_fg;
    SetStatusBarTheme(sbt);

    pmui::apply_dark_titlebar(GetHwnd(), pal.dark);
    UseDarkMenu(pal.dark ? TRUE : FALSE);

    pmui::enable_app_dark_mode(true);
    pmui::allow_dark_mode_for_window_tree(GetHwnd(), pal.dark);
}
```

## Avoiding White Flashes

```cpp
// Re-apply the UI font + force a full repaint to every panel hierarchy.
// In dark mode, skip RDW_ERASE to reduce a white/flash erase before themed paint.
const UINT k_refresh_rw = (UINT)(RDW_INVALIDATE | RDW_ALLCHILDREN
                                 | (pal.dark ? 0U : (UINT)RDW_ERASE));
auto refreshPanel = [&](CDocker* d) {
    if (!d) return;
    pmui::apply_font_to_tree(d->GetHwnd());
    ::RedrawWindow(d->GetHwnd(), nullptr, nullptr, k_refresh_rw);
};
```

## Theming Standard Controls

```cpp
// Push the right uxtheme parts to every standard child control so
// BUTTON/EDIT/COMBOBOX/LISTVIEW/etc. actually paint dark.
auto themePanel = [&](CDocker* d) {
    if (!d) return;
    pmui::apply_window_theme_recursive(d->GetHwnd(), pal.dark);
};
themePanel(m_pDockQueue);
themePanel(m_pDockLog);
themePanel(m_pDockSettings);
themePanel(m_pDockFindResults);
themePanel(m_pDockDuplicateResults);
```

## Custom Menu Background

```cpp
void CMainFrame::DrawMenuItemBkgnd(LPDRAWITEMSTRUCT pDrawItem)
{
    const auto& pal = pmui::theme_palette();
    if (pal.dark && IsUsingDarkMenu()) {
        const bool isDisabled = (pDrawItem->itemState & ODS_GRAYED) != 0;
        const bool isSelected = (pDrawItem->itemState & ODS_SELECTED) != 0;
        CRect       drawRect  = pDrawItem->rcItem;
        Win32xx::CDC drawDC(pDrawItem->hDC);

        if (isSelected && !isDisabled) {
            const Win32xx::MenuTheme& mbt = GetMenuBarTheme();
            drawDC.CreateSolidBrush(mbt.clrHot1);
            drawDC.CreatePen(PS_SOLID, 1, mbt.clrOutline);
            drawDC.Rectangle(drawRect.left, drawRect.top, drawRect.right, drawRect.bottom);
        } else {
            drawRect.left = GetMenuMetrics().GetGutterRect(pDrawItem->rcItem).Width();
            drawDC.SolidFill(pal.control_bg, drawRect);
        }
        return;
    }

    Win32xx::CFrameT<Win32xx::CDocker>::DrawMenuItemBkgnd(pDrawItem);
}
```

## DPI-Aware Control Factory

```cpp
inline int dpi_scale(HWND hwnd, int value)
{
    UINT dpi = 96;
    if (hwnd && ::IsWindow(hwnd))
        dpi = ::GetDpiForWindow(hwnd);
    return ::MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

inline HWND create_preset_combo(const PresetComboParams& p)
{
    // No WS_EX_CLIENTEDGE: dark combo styling supplies control_bg for the
    // listbox/static/edit paths and avoids light field fill.
    return ::CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, p.x,
        p.y - dpi_scale(p.parent, 1), p.combo_w, p.drop_height, p.parent, (HMENU)(UINT_PTR)p.id, p.inst, nullptr);
}
```

## Lazy Dock Layout Goal

```text
Hidden panels should not be created during startup. Panels that are visible,
tabbed, resized, or re-anchored by the user must round-trip exactly for the
active workbench. Default anchors belong to the workbench implementation, not
to generic CMainFrame cleanup code.
```

## App-Owned Layout Model

```cpp
struct DockPanelState {
    int id;
    bool visible;
    bool floating;
    RECT floating_rect;
    UINT dock_side;
    int dock_size;
    int parent_id;
    int tab_group_id;
    int tab_order;
};

struct WorkbenchDockLayout {
    std::string workbench_id;
    std::vector<DockPanelState> panels;
};
```

## Panel Factory for Dock Restore

```cpp
// Called by Win32++ CDocker::LoadDockLayout() to recreate each docker by ID.
// Do not store the raw pointer here: Win32++ may call CloseAllDockers() on
// failure, leaving any stored pointer dangling. Member pointers are assigned
// safely after a confirmed successful JSON restore.
DockPtr CMainFrame::NewDockerFromID(int id)
{
    switch (id) {
    case DOCK_ID_QUEUE:       return std::make_unique<CDockQueue>();
    case DOCK_ID_LOG:         return std::make_unique<CDockLog>();
    case DOCK_ID_SETTINGS:    return std::make_unique<CDockSettings>();
    case DOCK_ID_FINDRESULTS: return std::make_unique<CDockFindResults>();
    case DOCK_ID_DUPLICATERESULTS: return std::make_unique<CDockDuplicateResults>();
    case DOCK_ID_CHAT: {
#ifdef FEATURE_CHAT_WEB
        if (pmui::chat_web_available()) return std::make_unique<CDockChatWeb>();
#endif
        return std::make_unique<CDockChat>();
    }
    case DOCK_ID_FILETREE:    return std::make_unique<CDockFileTree>();
#ifdef FEATURE_NODES
    case DOCK_ID_NODES:      return std::make_unique<CDockNodes>();
#endif
    }
    return nullptr;
}
```

## Startup Draw Suppression

```cpp
// Suppress painting while dock restore + LoadLayout + ApplyAppearance run; the
// main HWND can be visible for hundreds of ms before theme/font land. Without
// this, users see a light/default frame flash, then a second pass.
struct InitialUpdateDrawLock {
    explicit InitialUpdateDrawLock(HWND h)
    {
        if (h)
            (void)::LockWindowUpdate(h);
    }
    ~InitialUpdateDrawLock() { (void)::LockWindowUpdate(nullptr); }
} const draw_lock{GetHwnd()};
```

## Restore or Build Workbench Defaults

```cpp
try {
    dock_topology_loaded = LoadDockLayout();
    pmui::ui_log_file_event("OnInitialUpdate: after LoadDockLayout");

    if (dock_topology_loaded) {
        m_workbench->BindDockPointers(*this);
        std::string reject_reason;
        if (!m_workbench->AcceptRestoredDockLayout(*this, reject_reason)) {
            CString msg(L"[Layout] Saved dock layout rejected by workbench policy");
            if (!reject_reason.empty())
                msg += CString(L": ") + pmui::utf8_to_wide(reject_reason).c_str();
            msg += L" - using default layout.";
            LogMessage(msg);
            dock_topology_loaded = false;
        }
    }
}
catch (...) {
    LogMessage(L"[Layout] LoadDockLayout threw unknown exception.");
    dock_topology_loaded = false;
}

if (!dock_topology_loaded) {
    pm::win32_dock::erase_from_settings(m_workbench->workbenchSettingsId());
    m_workbench->BuildInitialDockLayout(*this);
}
```

## Defer Heavy Post-Layout Work

```cpp
// Defer session replay, queue seeding, and IExplorerBrowser folder sync
// until the dock layout from settings has a chance to finish sizing.
::SetTimer(GetHwnd(), kPostLayoutInitTimerId, pm::ui::k_post_layout_init_delay_ms, nullptr);
pmui::ui_log_file_event("OnInitialUpdate leave; posted 150ms kPostLayoutInitTimerId");
```

## Future Work: Mini App as Visual Sandbox

The main app should stay conservative, but `apps/win32-mini` is useful for exploring more modern visuals without dragging the whole product with it. It tests borderless chrome, DWM hints, per-pixel alpha via `UpdateLayeredWindow`, custom caption buttons, rounded surfaces, and a Direct2D/WIC filmstrip with room for glow, effects, and animation.

Source files:

- `apps/win32-mini/README.md`
- `apps/win32-mini/main.cpp`
- `apps/win32-mini/filmstrip/FilmStrip.cpp`
- `apps/win32-mini/CMakeLists.txt`

```cmake
add_executable(pm-win32-mini WIN32
    main.cpp
    AppDefaults.cpp
    filmstrip/FilmStrip.cpp
    filmstrip/FilmStripThumb.cpp
)

target_link_libraries(pm-win32-mini PRIVATE
    user32
    gdi32
    dwmapi
    gdiplus
    d2d1
    windowscodecs
    dwrite
    ole32
    advapi32
)
```

Important findings from the sandbox:

- A layered top-level window driven by `UpdateLayeredWindow` is effectively one composited bitmap.
- Standard child controls on that same layered HWND are not a reliable visual layer.
- A separate owned, non-layered popup is a better host for real controls when needed.
- Transparent rounded corners affect hit-testing; resize and caption logic still need geometry-based handling.
- GDI text on a 32 bpp layered DIB can leave alpha holes; use GDI+ or DirectWrite-style rendering paths for visible text.
