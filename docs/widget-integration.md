# XBlox Fenced Widget Integration

This document is the checklist for adding the XBlox fenced markdown widget to another React app. The immediate later target is `pm-pics/src/modules/pages/markdown/MarkdownRenderer.tsx`, but the steps are written so any app with a `react-markdown` renderer can follow them.

The integration must stay web-only. It uses the static palette and the TypeScript web runtime. It must not depend on the native WebView backend, `hostBridge`, or native XBlox RPC.

## What The Fence Does

An XBlox fence is a markdown code block with language `xblox` or `xblox-json`.

````markdown
```xblox
{
  "options": {
    "hasToolbar": true,
    "showToolbar": true,
    "hasPalette": true,
    "showPalette": false,
    "hasProps": true,
    "showProps": true,
    "hasRunLog": true,
    "showRunLog": true,
    "editable": true,
    "autoHeight": true
  },
  "document": {
    "version": 1,
    "context": {
      "score": 7,
      "user": { "name": "kim" }
    },
    "roots": [
      { "kind": "stdout", "message": "hello ${user.name}" },
      { "kind": "log", "level": "info", "message": "score=${score}" }
    ]
  }
}
```
````

The wrapper form is preferred:

- `options` controls the embedded widget surface.
- `document` is the XBlox document payload.

Plain document JSON is also accepted if the app does not need per-fence options.

## Option Semantics

Use `has*` for capability and `show*` for initial visibility.

- `hasPalette: false` means there is no palette panel and the user cannot open it.
- `showPalette: false` means the palette exists but starts collapsed.
- `hasProps: false` means there is no properties panel or properties dialog.
- `showProps: false` or `showProperties: false` means properties exist but start collapsed.
- `hasRunLog: false` or `hasLog: false` means there is no run log panel.
- `showRunLog: false` or `showLog: false` means the run log exists but starts collapsed.
- `hasToolbar: false` disables toolbar-backed actions like run, run chain, clipboard, delete, move, navigation, and search.
- `showToolbar: false` hides the toolbar chrome but does not mean the feature is unavailable unless `hasToolbar` is also false.

Default behavior for docs widgets should usually be:

```json
{
  "hasToolbar": true,
  "showToolbar": true,
  "hasPalette": true,
  "showPalette": false,
  "hasProps": true,
  "showProps": true,
  "hasRunLog": true,
  "showRunLog": true,
  "hasHelp": true,
  "showHelp": false,
  "editable": true,
  "autoHeight": true
}
```

## Runtime Boundary

The fenced widget runs through `apps/xblox/src/xblox/runtime-web`.

Supported web-runtime blocks currently include:

- `stdout`
- `log`
- `setVariable`
- `getVariable`
- `if`, `elseIf`, `else`
- `switch`, `case`, `switchDefault`
- `while`, `for`, `break`
- `iterator`
- `wait` when enabled by runtime options

Unsupported native/system blocks are not executed. They should produce skipped events rather than reaching for native RPC.

Run output is delivered as XBlox run events. `stdout` events include `stdout`; `log` events include the rendered message and level in event data. Apps should show both in the run log panel.

## Integration Paths

There are two valid integration paths.

### Path A: Source-Level React Integration

Use this when the target app builds together with the PixlWiz source tree or can alias into it. This is what `viewer-next` currently does.

You import the React fence widget and shared XBlox builder code directly, then let the target bundler compile it.

Required source pieces:

- `apps/viewer-next/src/viewers/markdown/MarkdownRenderer_Extensions.tsx`
- `apps/viewer-next/src/viewers/markdown/XbloxFenceWidget.tsx`
- `apps/xblox/src/xblox/react/*`
- `apps/xblox/src/xblox/runtime-web/*`
- `apps/xblox/src/palette.json`
- `apps/xblox/src/styles.css`
- `apps/shared/xblox/*`
- `apps/shared/web/hostBridge.ts` types only

This path gives the target app the full builder surface: tree, palette, props, run log, help, and web runtime.

### Path B: Built Widget Bundle

Use this when the target app should not import PixlWiz source. Build a standalone widget bundle from `apps/xblox`.

```bash
cd apps/xblox
npm run build:widget
```

Output:

- `apps/xblox/dist/widget/xblox-widget.js`
- `apps/xblox/dist/widget/xblox-widget.css`
- source map files if enabled

The bundle exports:

- `XbloxWidget`
- `renderXbloxWidget(element, props)`
- `staticNativePalette`

The bundle is best for non-React or loosely coupled host apps. The current markdown fence work in `viewer-next` uses source-level integration because it needs the full builder and run log inside React.

## React Markdown Integration

The target renderer needs to do three things:

1. Detect `xblox` and `xblox-json` code fences.
2. Render the fence with an XBlox widget instead of Prism/plain code.
3. Unwrap the surrounding `<pre>` so the widget is not nested inside code block styling.

### Add The Lazy Component

Create a small extension module next to the markdown renderer in the target app.

```tsx
import React from "react";

export type XbloxBlockOptions = {
  theme?: "dark" | "light";
  hasToolbar?: boolean;
  showToolbar?: boolean;
  hasPalette?: boolean;
  showPalette?: boolean;
  hasProps?: boolean;
  showProps?: boolean;
  showProperties?: boolean;
  hasProperties?: boolean;
  hasRunLog?: boolean;
  showRunLog?: boolean;
  hasLog?: boolean;
  showLog?: boolean;
  hasHelp?: boolean;
  showHelp?: boolean;
  editable?: boolean;
  autoHeight?: boolean;
  hasAutoHeight?: boolean;
  maxHeight?: number | string;
};

export function isXbloxLanguage(language: string): boolean {
  return language === "xblox" || language === "xblox-json";
}

export function isXbloxCodeClass(className: string): boolean {
  return /\blanguage-(xblox|xblox-json)\b/.test(className);
}

const LazyXbloxFenceWidget = React.lazy(() =>
  import("./XbloxFenceWidget").then((mod) => ({ default: mod.XbloxFenceWidget })),
);

export function XbloxBlock({ document, ...options }: { document: string } & XbloxBlockOptions) {
  return (
    <React.Suspense fallback={<pre className="pm-xblox-loading">Loading XBlox...</pre>}>
      <LazyXbloxFenceWidget source={document} {...options} />
    </React.Suspense>
  );
}
```

For `pm-pics`, this mirrors the existing lazy `MermaidWidget` pattern in `src/modules/pages/markdown/MarkdownRenderer.tsx`.

### Update The `code` Renderer

In the target `ReactMarkdown` `code` component, detect XBlox before Prism highlighting.

```tsx
code: ({ className, children, ...props }) => {
  const match = /language-([\w-]+)/.exec(className || "");
  const language = match ? match[1] : "";

  if (!match) {
    return <code className={className} {...props}>{children}</code>;
  }

  const text = String(children).replace(/\n$/, "");

  if (isXbloxLanguage(language)) {
    return <XbloxBlock document={text} {...xbloxOptions} />;
  }

  // Existing Mermaid, gallery, Prism, and fallback code paths stay below.
}
```

`pm-pics` currently checks `language-mermaid` and `language-custom-gallery` before Prism. Add XBlox beside those custom widget checks.

### Update The `pre` Renderer

If the markdown renderer wraps code blocks in a custom `<pre>`, unwrap XBlox fences the same way Mermaid and gallery are unwrapped.

```tsx
pre: ({ node, children, ...props }) => {
  const firstChild = node?.children?.[0];
  if (firstChild?.type === "element" && firstChild?.tagName === "code") {
    const classNames = firstChild.properties?.className;
    const cn = Array.isArray(classNames) ? classNames.join(" ") : String(classNames || "");

    if (isXbloxCodeClass(cn)) {
      return <>{children}</>;
    }
  }

  return <pre {...props}>{children}</pre>;
}
```

In `pm-pics`, extend the existing `isGallery || isMermaid` branch to include `isXblox`.

## Theme Wiring

Pass the app's current theme into the widget if the app already knows it.

```tsx
const xbloxOptions = useMemo(
  () => ({
    theme,
    hasToolbar: true,
    showToolbar: true,
    hasPalette: true,
    showPalette: false,
    hasProps: true,
    showProps: true,
    hasRunLog: true,
    showRunLog: true,
    editable: true,
    autoHeight: true,
  }),
  [theme],
);
```

If the target app does not pass a theme, `XbloxFenceWidget` detects light/dark from the document classes. Passing the theme is still better because it avoids a first-render mismatch.

## Palette Loading

The source-level widget loads `apps/xblox/src/palette.json` as an asset URL and decodes UTF-8 or UTF-16LE. A target app must make this file reachable by the bundler.

For Vite-style source integration, add aliases so imports resolve:

```ts
resolve: {
  alias: {
    "@pm/shared": path.resolve(repoRoot, "apps/shared"),
    "@pm/viewer": path.resolve(repoRoot, "apps/viewer-next/src"),
    "@/components/logs": path.resolve(repoRoot, "apps/xblox/src/components/logs"),
    "@/schema": path.resolve(repoRoot, "apps/xblox/src/schema"),
    "@/xblox": path.resolve(repoRoot, "apps/xblox/src/xblox"),
  },
}
```

Exact aliases depend on the target app's existing `@` meaning. In `pm-pics`, `@` already points at `pm-pics/src`, so do not blindly copy `viewer-next` imports. Either:

- copy/adapt the fence widget into `pm-pics` with explicit relative imports to PixlWiz packages, or
- create a small package/export from `apps/xblox` so `pm-pics` can import `@polymech/xblox/widget-react` without alias conflicts.

Prefer the package/export route for long-term maintenance.

## CSS Requirements

The full builder needs the XBlox styles plus small markdown-host overrides.

At minimum import:

```ts
import "path-to-pixlwiz/apps/xblox/src/styles.css";
```

Then add host-scoped markdown overrides equivalent to `viewer-next`'s `.pm-xblox-fence` and `.pm-xblox-fence--builder` rules:

- prevent markdown `pre` and `code` styles from applying inside the builder
- set widget min-height and max-height behavior
- constrain embedded property/log panels
- ensure light/dark variables match the host theme

Keep the rules scoped under `.pm-xblox-fence` so the widget does not restyle normal markdown code blocks.

## `pm-pics` Integration Plan

Do this later in `C:\Users\zx\Desktop\polymech\pm-pics`.

1. Add an XBlox fence component next to `src/modules/pages/markdown/MermaidWidget`.

   Suggested files:

   - `src/modules/pages/markdown/XbloxFenceWidget.tsx`
   - `src/modules/pages/markdown/MarkdownRenderer_Extensions.tsx` or local helpers inside `MarkdownRenderer.tsx`

2. Decide the dependency strategy.

   Short-term:

   - import/copy the current `viewer-next` fence widget and wire aliases to PixlWiz source

   Long-term:

   - expose a clean `apps/xblox` React widget entry that does not depend on `viewer-next` aliases
   - import that from `pm-pics`

3. Extend `MarkdownRenderer.tsx`.

   Current custom fences:

   - `language-mermaid`
   - `language-custom-gallery`

   Add:

   - `language-xblox`
   - `language-xblox-json`

4. Update the `pre` unwrap logic.

   Existing branch unwraps gallery and Mermaid. Include XBlox so the builder is rendered as a block widget, not as code.

5. Pass theme and default options.

   If `pm-pics` has a theme context, pass it. If not, rely on DOM class detection first, then wire explicit theme later.

   Recommended defaults for `pm-pics`:

   ```tsx
   const xbloxOptions = {
     hasToolbar: true,
     showToolbar: true,
     hasPalette: true,
     showPalette: false,
     hasProps: true,
     showProps: true,
     hasRunLog: true,
     showRunLog: true,
     hasHelp: true,
     showHelp: false,
     editable: true,
     autoHeight: true,
   };
   ```

6. Add scoped CSS.

   Put it in the markdown/page CSS layer, not global app chrome. Scope everything with `.pm-xblox-fence`.

7. Verify with `docs/xblox/widget-test.md`.

   The expected behavior:

   - the `xblox` fence renders as the full builder
   - palette exists but starts collapsed when `showPalette: false`
   - props panel is visible when `showProps: true`
   - run log is visible when `showRunLog: true`
   - Run Chain executes through the web runtime
   - `stdout` and `log` block messages appear in the run log
   - unsupported native blocks are skipped, not sent to native RPC

## Build Config Checklist

For a Vite app:

- ensure `react`, `react-dom`, and `@tanstack/react-query` are available
- ensure the target can import or bundle XBlox CSS
- ensure `palette.json` is emitted as an asset or virtual module
- define XBlox build flags for the intended embedded surface
- avoid native runtime flags for markdown widgets

### Sample Vite Extras

This is a starting point for a Vite host app that source-imports the widget. Adjust paths for the target repo layout.

```ts
import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

const here = path.dirname(fileURLToPath(import.meta.url));
const pixlwizRoot = path.resolve(here, "../pixlwiz");

const xbloxWidgetDefines = {
  "import.meta.env.VITE_XBLOX_RUNTIME": JSON.stringify("web"),

  __XBLOX_PANEL_COMMAND_STRIP__: JSON.stringify(false),
  __XBLOX_PANEL_DEBUG__: JSON.stringify(false),
  __XBLOX_PANEL_PALETTE__: JSON.stringify(true),
  __XBLOX_PANEL_PROPERTIES__: JSON.stringify(true),
  __XBLOX_PANEL_RUN_LOG__: JSON.stringify(true),
  __XBLOX_PANEL_HELP__: JSON.stringify(true),
  __XBLOX_PANEL_MERMAID__: JSON.stringify(false),

  __XBLOX_ACTION_SAVE__: JSON.stringify(false),
  __XBLOX_ACTION_RUN__: JSON.stringify(true),
  __XBLOX_ACTION_RUN_CHAIN__: JSON.stringify(true),
  __XBLOX_ACTION_STOP__: JSON.stringify(false),
  __XBLOX_ACTION_CLEAR_RUN_STATE__: JSON.stringify(true),
  __XBLOX_ACTION_LOOP__: JSON.stringify(false),
  __XBLOX_ACTION_UNDO_REDO__: JSON.stringify(false),
  __XBLOX_ACTION_CLIPBOARD__: JSON.stringify(true),
  __XBLOX_ACTION_DELETE__: JSON.stringify(true),
  __XBLOX_ACTION_MOVE__: JSON.stringify(true),
  __XBLOX_ACTION_NAVIGATE__: JSON.stringify(true),
  __XBLOX_ACTION_SEARCH__: JSON.stringify(true),
  __XBLOX_ACTION_THEME__: JSON.stringify(false),
  __XBLOX_ACTION_MERMAID_FULLSCREEN__: JSON.stringify(false),
  __XBLOX_ACTION_PROPERTIES_DIALOG__: JSON.stringify(true),
};

export default defineConfig({
  plugins: [react()],
  define: xbloxWidgetDefines,
  resolve: {
    alias: [
      { find: "@pm/shared", replacement: path.resolve(pixlwizRoot, "apps/shared") },
      { find: "@pm/viewer", replacement: path.resolve(pixlwizRoot, "apps/viewer-next/src") },

      // Only use these if your copied/adapted fence widget still imports the xblox app's "@/..." paths.
      // If the host app already owns "@", prefer explicit package exports instead of these aliases.
      { find: "@/components/logs", replacement: path.resolve(pixlwizRoot, "apps/xblox/src/components/logs") },
      { find: "@/schema", replacement: path.resolve(pixlwizRoot, "apps/xblox/src/schema") },
      { find: "@/xblox", replacement: path.resolve(pixlwizRoot, "apps/xblox/src/xblox") },
    ],
  },
  optimizeDeps: {
    include: [
      "@tanstack/react-query",
      "@radix-ui/react-select",
      "lucide-react",
      "use-sync-external-store/shim/with-selector",
    ],
  },
  assetsInclude: [
    "**/apps/xblox/src/palette.json",
  ],
});
```

For `pm-pics`, be careful: its `@` alias already means `pm-pics/src`. Do not add a broad `{ find: "@", replacement: ... }` alias for PixlWiz. Either keep imports explicit in the copied fence widget, or create a stable package/export from `apps/xblox` and import that package.

If the target app uses Vite but should consume the prebuilt widget instead of source imports, keep the Vite config mostly unchanged. Copy or publish `apps/xblox/dist/widget`, then lazy-load the ESM bundle:

```ts
const mod = await import("/widgets/xblox-widget.js");
mod.renderXbloxWidget(element, { document, ...options });
```

For a Webpack app:

- add TS/TSX handling for the imported XBlox source
- include `apps/xblox/src` and `apps/shared` in transpilation if needed
- add aliases matching the import strategy
- add asset handling for `palette.json`
- define the same compile-time XBlox feature flags

For a pure external widget:

- run `npm run build:widget`
- publish/copy `dist/widget/xblox-widget.js` and CSS
- mount with `renderXbloxWidget(element, { document, ...options })`

## Verification Commands

In `apps/xblox`:

```bash
npm run tests:web-runtime
npm run build:widget
npm run build:embed
```

In the target app:

```bash
npm run build
```

If the target app has a visual markdown preview, open a document containing the fence from `docs/xblox/widget-test.md` and use **Run Chain**.

## Troubleshooting

If the fence renders as a code block:

- check `language-xblox` detection
- check the `pre` unwrap branch
- check that `ReactMarkdown` is receiving the custom `components`

If the widget renders but palette fails:

- check the `palette.json` asset URL
- check UTF-16LE decoding if loading the raw file
- check aliases if importing from PixlWiz source

If Run Chain does nothing:

- check `hasToolbar`
- check `features.actions.runChain`
- check that `onRunChain` is wired to `runWebXbloxDocument`

If the log panel is missing:

- check `hasRunLog` or `hasLog`
- check `showRunLog` only controls initial collapsed state
- check that `features.panels.runLog` is true

If CSS is broken:

- verify `apps/xblox/src/styles.css` is imported once
- verify markdown `pre/code` rules are neutralized under `.pm-xblox-fence`
- verify the host app is not globally styling all nested `button`, `pre`, or `code` elements inside prose

## Non-Goals

- Do not integrate native WebView RPC into external markdown widgets.
- Do not run shell/network/filesystem native blocks in the browser widget.
- Do not make `pm-pics` depend on `viewer-next` internals permanently.
- Do not let widget CSS leak into normal markdown rendering.
