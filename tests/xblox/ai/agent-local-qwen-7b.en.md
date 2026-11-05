# pm-image (C++)

CMake-based **`pm-image`** binary: **CLI**, **HTTP REST** (`serve`), and **line-delimited JSON IPC** (`ipc`). Image processing uses **libvips** — the same engine as **[Sharp](https://sharp.pixelplumbing.com/)** (Node.js), exposed with a similar option model. Configure API keys and URLs via **environment variables**, **app `settings.json`**, or CLI flags — see [docs/settings.md](docs/settings.md) and the sections below.

The tool is aimed at **large batch operations**: path **globs**, **per-file output templates**, **optional disk cache**, and **server / IPC** endpoints are all built around processing **many images per run** (folders, catalogs, CI, sidecar services). Single-file use is supported, but the design favors throughput and repeatability over one-off editing.

- **API reference (underlying library):** [libvips `VipsImage`](https://www.libvips.org/API/current/class.Image.html)

**Documentation**

| Topic | Where |
|-------|--------|
| **Product overview** (batch workflows, RAW) | [docs/product.md](docs/product.md) |
| **Duplicates** (size / fingerprint / meta, optional JSON+LLM) | [docs/duplicates.md](docs/duplicates.md) |
| **CLI** (`resize`, `compress`, `transform`, `serve`, `ipc`, …) | This file — [CLI examples](#cli-examples), subcommand sections below |
| **`--ui-next` ribbon UI** (Windows) | [User guide](docs/win32xx-ui.md#user-guide-ui-next) · [architecture](docs/win32xx-ui.md#1-component-overview) |
| **macOS native UI** (planned AppKit shell, env, signing) | [docs/osx.md](docs/osx.md) |
| **REST API** (`serve` — pass-through binary / JSON) | [docs/api-rest.md](docs/api-rest.md) |
| **IPC API** (`ipc` — TCP / Unix socket, line-JSON) | [docs/api-uds.md](docs/api-uds.md) |
| **API developer notes** (extending `serve` / `ipc`) | [docs/api-dev.md](docs/api-dev.md) |
| **Adding a new operation** (LLM/dev playbook) | [docs/llm.md](docs/llm.md) |
| **`serve` / `ipc` as a service** (systemd / NSSM) | [docs/integration.md](docs/integration.md) |

## Prerequisites

| Requirement | Notes |
|-------------|--------|
| CMake | ≥ 3.20 |
| C++ compiler | C++17 |
| **libvips** | **Required** — pkg-config (Unix) or `third_party/vips-dev-*` / `VIPS_ROOT` (Windows) |
| Git | For `FetchContent` (CLI11, Asio, httplib, json, **libcurl**, [p-ranav/glob](https://github.com/p-ranav/glob), [PicoSHA2](https://github.com/okdshin/PicoSHA2) for cache keys) |
| Node.js | Optional — `npm run test:media` (Node 18+) |

### Installing libvips

**Debian / Ubuntu**

```bash
sudo apt install libvips-dev pkg-config
```

**macOS (Homebrew)**

```bash
brew install vips pkg-config
```

**Windows (official dev bundle — recommended)**

```powershell
npm run setup:vips
```

Downloads [build-win64-mxe `vips-dev-x64-all`](https://github.com/libvips/build-win64-mxe/releases) into `third_party/vips-dev-*`. CMake adds that path automatically; DLLs are copied to `dist/` on link.

Pin the version with **`MEDIA_VIPS_VERSION`** (default `8.18.2`) if needed.

Alternatively set **`VIPS_ROOT`** or **`CMAKE_PREFIX_PATH`** to a tree with `include/vips/vips.h` and `lib/libvips.lib`.

## Build

```bash
cd packages/media/cpp
cmake --preset release
cmake --build --preset release
```

Binary: **`dist/pm-image`** (`.exe` on Windows).

**macOS:** Install **libvips** (e.g. `brew install vips pkg-config`), **CMake** (`brew install cmake`), and **Apple Command Line Tools** (`xcode-select --install`) for the compiler — **not** the full Xcode app. Build from **`packages/media/cpp`** with the same `cmake --preset` / `cmake --build` commands above; optional `-G Ninja` after `brew install ninja`. Native AppKit UI plans, signing, and toolchain detail: [docs/osx.md](docs/osx.md).

### Windows installer (NSIS)

From `packages/media/cpp`, after a release build:

```bash
npm run build:installer
```

Produces **`dist/pm-image-Setup.exe`**, installs `pm-image.exe`, libvips DLLs, `vips-modules-8.18`, and `scripts/explorer-resize.ps1`, and prepends the install directory to the **user** `PATH`. Uninstaller is registered under Add/Remove Programs.

### Explorer context menu (`pm-image register-explorer`)

Implemented in **C++** (Windows registry). The NSIS installer runs `pm-image.exe register-explorer` after copying files; uninstall runs `register-explorer --unregister --no-refresh-shell`.

```bash
pm-image register-explorer
pm-image register-explorer --dry
pm-image register-explorer --unregister
```

Registers a **PM-Media** cascading menu on image extensions (including **`.avif`**, **`.arw`**, **`.webp`**, TIFF, etc.), on **folders**, and on the **folder background** (empty area). **Resize** presets (default widths **1980, 1200, 800, 400**): **in place** or **copy** as `${SRC_NAME}_${width}${SRC_EXT}`. **Convert to JPG** writes `${SRC_NAME}_converted.jpg` through the same **IExecute** / **`pm-image`** path as other verbs. Override widths with `--widths 1920,800`.

Under `SystemFileAssociations`, each extension is registered as **`.ext`**, **`.EXT`**, and **`.Ext`** so Explorer picks up the menu regardless of filename casing.

Defaults: **`--media-bin`** is this executable; **`--widths`** defaults to **`1980,1200`** (see **`pm-image register-explorer --help`**). Optional **`scripts/explorer-*.ps1`** files ship for manual or legacy automation; they are **not** used for the standard context-menu verbs.

`pm-media register-explorer` forwards argv to `pm-image.exe` for convenience.

Standard verbs invoke **`pm-image`** via **DelegateExecute** / **IExecute** (no PowerShell middleman). Re-run `pm-image register-explorer` after updates so registry paths stay current.

## Formats — same idea as Sharp / libvips

Sharp wraps libvips: **decode → process → encode**. We do the same with `vips_image_new_from_file` and format-specific savers.

| | Supported |
|---|-----------|
| **Resize / geometry** | `fit`, dimensions, crop (`cover` + `position`), letterbox (`contain` + `background`), rotate, flip, flop, EXIF `autorotate` |
| **Output (first-class in code)** | **JPEG** (`Q`, strip), **PNG** (`compression`), **WebP** (`Q`, strip), **TIFF** |
| **AVIF / HEIC** | Via `format` / file extension and `quality` (libvips HEIF/AVIF saver — needs libheif in your libvips build) |
| **Anything else libvips knows** | Fallback: `vips_image_write_to_file` from extension (e.g. **GIF**, **JP2K**, … depending on how libvips was built) |

**Input** types match **whatever your libvips build can load** (the Windows `vips-dev-x64-all` bundle includes broad loader support). Set output format with the **`output` path extension** or JSON / CLI **`format`** (`webp`, `avif`, `jpg`, …).

### Camera RAW (dedicated support)

**RAW** camera files (e.g. **`.arw`**, **`.cr2`**, **`.nef`**, **`.dng`**, and other extensions your libvips build exposes via **LibRaw**) are first-class **inputs** for **`resize`**, **`compress`**, **`serve`**, **`ipc`**, and Explorer flows the same as TIFF/JPEG: decode happens inside libvips, so you can run **batch jobs** over recursive globs (e.g. `` `**/*.arw` ``) or mixed folders without a separate RAW converter.

On **Windows `--ui-next`**, optional compile-time flags add a **dedicated preview path** for RAWs:

| CMake flag | Behavior |
|------------|----------|
| **`FEATURE_RAW_PREVIEW`** (default ON) | Fast preview using the **embedded JPEG** in the RAW (via libvips thumbnail path). |
| **`FEATURE_RAW_VIEW`** (default ON; requires preview) | After a short delay, replaces the preview with a **full demosaiced** decode (`vips_image_new_from_file`). Turn either flag OFF to disable that tier. |

See **[docs/win32xx-ui.md](docs/win32xx-ui.md)** for how the UI loads RAW previews; [src/win/ui_next/features.h](src/win/ui_next/features.h) lists all flags.

## Sharp-like options (`resize` / JSON)

| Sharp concept | `pm-image` / JSON field | Notes |
|---------------|---------------------------|--------|
| `resize.fit` | `fit` | `inside`, `cover`, `contain`, `fill`, `outside` |
| `resize.position` | `position` | `centre`, `attention`, `entropy`, … → libvips *interesting* |
| `resize.kernel` | `kernel` | `nearest`, `cubic`, `mitchell`, `lanczos2`, `lanczos3` (default) |
| `jpeg|webp|… quality` | `quality` | 1–100 |
| `png compression` | `png_compression` | 0–9 |
| `withoutEnlargement` | `without_enlargement` | Default **true**; CLI `--allow-enlargement` flips |
| EXIF orientation | `autorotate` | Default **true**; CLI `--no-autorotate` |
| Strip metadata | `strip_metadata` | Default **true**; CLI `--no-strip` |
| `rotate` | `rotate` | 0, 90, 180, 270 (after autorotate) |
| `flip` / `flop` | `flip` / `flop` | |
| Letterbox | `background` | `#rrggbb` for `contain` |

**Windows:** Native UI entry points are documented under [Windows UI](#windows-ui) below.

**REST** `POST /v1/resize` accepts **`application/json`** (same keys as the table, plus **“Batch paths & cache”** below) **or** **`multipart/form-data`** (upload a file; response is the **image bytes** — see **`serve`**). **IPC** uses the same JSON keys as the table, plus the batch/cache fields.

---

## Batch paths & cache: globs, variables, caching

Use this when **one** `resize` invocation (CLI, or a single REST/IPC request) should match **many** inputs or build **per-file** output paths. See also [docs/Examples.md](../docs/Examples.md) (TypeScript / Sharp reference).

### Summary

| Topic | What it does |
|-------|----------------|
| **Input glob** | `*` / `?` / `**` in `input` expand to a list of files ([p-ranav/glob](https://github.com/p-ranav/glob)). |
| **HTTP(S) URL** | `input` may be `http://` or `https://` — image is fetched with **libcurl** (follows redirects; `--url-timeout` default **5** s; `--url-max-redirects` default **20**). Cache key is URL + options (no local mtime). |
| **Omit `output` (CLI)** | If the input resolves to **exactly one** file or URL, you may omit the second positional argument: the file is written to the **current working directory** using a **sanitized basename** (same rules as `sanitizeFilename` in [`packages/acl`](../../acl/src/vfs/path-sanitizer.ts) — illegal/control chars, Windows reserved names, trailing dots/spaces, 255-byte UTF-8 cap). URLs without a path extension default to **`.jpg`**. `--format` overrides the output extension when set. |
| **Destination variables** | `output` may contain `${SRC_DIR}`, `${SRC_NAME}`, `${SRC_FILE_EXT}` (or `&{…}`) — expanded **per matched input**. |
| **expand_glob** | JSON `false`: treat `input` / `output` as literal paths (no glob expansion). Templates still apply if `output` contains `${SRC_` / `&{SRC_`. |
| **Output cache** | SHA-256 key from input path + size + mtime + options; default dir `<cwd>/cache/images/`. |

**Not supported in C++:** Bash **extglob** (e.g. `*.+(jpg)`). Use `*.jpg`, `**/*.jpg`, or separate runs. **Bare** `{SRC_NAME}` without `$` / `&` is not a placeholder — use `${SRC_NAME}` or `&{SRC_NAME}`.

### Input globs

- **Syntax:** `*`, `?`, and `**` for recursion. Paths are resolved from the current working directory unless absolute.
- **Multiple files → directory output:** `output` must be an **existing** directory, **or** a **new** directory given with a **trailing** `/` or `\` (parent dirs are created).
- **Single file** from a literal path or a glob that matches one file: `output` can be a full file path, or a directory (trailing sep) to keep the original filename.

**CLI:** use positional `input`/`output` or `--src` / `--dst` together. You can omit `output` only when `input` is a single path or URL (not a multi-match glob).

```bash
pm-image resize './photos/**/*.jpg' ./out/
pm-image resize --src './shots/*.png' --dst ./thumbs/
# URL → ./200.jpg under cwd (picsum path segment "200", default extension .jpg)
pm-image resize 'https://picsum.photos/200' --max-width 400 --url-timeout 30
```

**JSON:** same strings in `"input"` and `"output"`. When `expand_glob` is true (default), glob expansion runs when the pattern contains `*`, `?`, or `**`.

### Destination variables (`${SRC_*}` / `&{SRC_*}`)

Placeholders are expanded **after** inputs are resolved (glob or single file). Each output path is built from the **absolute** input file for that row.

| Placeholder | Meaning |
|-------------|---------|
| **SRC_DIR** | Parent directory of the current input (generic path, `/` separators). |
| **SRC_NAME** | Filename **stem** without extension (`photo` for `photo.JPG`). |
| **SRC_FILE_EXT** | Extension **with** leading dot (e.g. `.jpg`), or empty if none. |

Use cases: write beside each source (`${SRC_DIR}/out/${SRC_NAME}.webp`), suffix stems (`${SRC_NAME}_thumb.jpg`), or change extension via `format` / path.

```bash
pm-image resize --src ./photo.jpg --dst '${SRC_DIR}/${SRC_NAME}_medium.jpg' --max-width 800
pm-image resize --src './shots/*.jpg' --dst '${SRC_DIR}/${SRC_NAME}.webp' --max-width 1920
```

**REST / IPC** — same strings in JSON (escape quotes in shell as needed):

```bash
curl -s -X POST http://127.0.0.1:8080/v1/resize \
  -H 'Content-Type: application/json' \
  -d '{"input":"/data/in.png","output":"/out/${SRC_NAME}_thumb.webp","max_width":256}'
```

**Responses:** `{"ok":true}` for a single output; if more than one file is produced, `{"ok":true,"count":N,"outputs":["..."]}`.

### JSON reference (batch + cache)

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `input` | string | required | Source path or glob. |
| `output` | string | required | File path, directory, or template with `${SRC_*}` / `&{SRC_*}`. |
| `expand_glob` | bool | `true` | If `false`, no glob expansion; paths are literal. |
| `cache` | bool | `true` (or server default from `serve` / `ipc` flags) | Enable/disable cache for this request. |
| `cache_dir` | string | empty → `<cwd>/cache/images` (or server `--cache-dir`) | Root directory for cached blobs. |
| `url_timeout_sec` | int | `5` | Total + connect timeout for HTTP(S) fetch (seconds; `0` = libcurl default). |
| `url_max_redirects` | int | `20` | Max redirects when fetching URLs. |

All **resize** options (`max_width`, `fit`, …) participate in the same JSON body.

**REST / IPC** JSON always require `"output"` in JSON (no automatic path). Multipart upload does not use `input`/`output` paths.

### Output cache

- **Default:** caching is **on**; root dir **`cache/images`** under the process **current working directory** (override with `--cache-dir` or JSON `cache_dir`).
- **Key:** **SHA-256** (PicoSHA2) over canonical input path, file size, modification time, and a stable encoding of **all** resize options — change any of these and you get a miss.
- **Storage:** `<cache_dir>/XX/<hex>` (two-letter shard).
- **Hit:** copy cached bytes to `output`; **libvips is not initialized** for that job.
- **Miss:** run resize, then **best-effort** store into cache (failure to store does not fail the request).

**CLI (`resize`):** `--no-cache`, `--cache-dir <path>`.

**`serve` / `ipc`:** same flags set **defaults** for requests that omit `cache` / `cache_dir`. Per-request JSON can still set `"cache": true` or `"cache_dir": "/path"` to override.

**Batch + cache:** glob batches run **sequentially** (one file after another); each file may hit or miss the cache independently.

### `serve` example with cache and glob

```bash
pm-image serve --host 127.0.0.1 -p 8080 --cache-dir /var/cache/pm-image
```

```bash
curl -s -X POST http://127.0.0.1:8080/v1/resize \
  -H 'Content-Type: application/json' \
  -d '{"input":"/data/in/*.jpg","output":"/data/out/","max_width":400,"cache":true,"cache_dir":"/var/cache/pm-image"}'
```

## Concurrency

- **HTTP `serve`**: cpp-httplib default thread pool (`CPPHTTPLIB_THREAD_POOL_COUNT` — see upstream `httplib.h`).
- **libvips**: processing is thread-safe per image; configure process-wide concurrency with `VIPS_CONCURRENCY` (or `vips_concurrency_set` in code later if needed).

## CLI examples

Paths below use Unix style; on Windows run `pm-image.exe` and use `.\` or full paths as needed.

### Help and version

```bash
pm-image --help
pm-image resize --help
pm-image compress --help
pm-image meta --help
pm-image -v
```

### Windows UI

**`--ui`** (Win32 dialog) — `pm-image resize --ui` opens a native path/options dialog. Other `resize` flags seed defaults; cancel exits without processing.

**`--ui-next`** (Win32++ ribbon) — `pm-image resize --ui-next` starts the full workbench:

| Area | Role |
|------|------|
| **Home** ribbon | **Resize**, **Compress**, **Meta**, **AI Transform**, **Find**, and **Duplicates** switch the right-hand **Settings** panel mode and run the matching pipeline on the **queue** (or on the Explorer selection — files are enqueued first). |
| **Queue** | Drag/drop or **Add files**; shows per-file status for resize / compress / meta / transform. |
| **Settings** | Resize presets and geometry; **Compress** (MozJPEG vs PNG, quality, PNG level, optional quantize/zopfli when enabled at build time); **Meta** (LLM description: outputs `.md` / `.json` / EXIF, in-memory pre-resize 256–1024 px, provider/model/prompt presets); **Transform** prompt and model options; **Find** (name or LLM search); **Duplicates** (file size, fingerprint, or sidecar+EXIF; optional **JSON+LLM** pair compare). Default destination presets prefer **next to source** with a **`_resized` / `_compressed` stem suffix** so outputs do not overwrite sources. |
| **Log** | Same detail as CLI-style messages (paths, errors). **Duplicates** in LLM mode logs pair progress and scores. |
| **Preview** | Image preview with pan, zoom, **Fit**, **Full** (fullscreen); **F11** / **Alt+F** toggle fullscreen, **Esc** exits. Camera **RAW** files (e.g. `.arw`) may show a fast embedded preview first, then a higher-quality decode when `FEATURE_RAW_VIEW` is enabled. |
| **Find results** / **Duplicate groups** | **Find results**: table of search matches. **Duplicate groups**: tree of groups and member files; right-click to save/open **session** JSON. |
| **File tree** (optional, `FEATURE_FILETREE`) | Explorer-style browser; selecting an image loads the preview and sets **implicit selection** for commands without using the queue first. |

Layout, panel visibility, window placement, last Explorer folder, AI presets, and **appearance preferences (theme + font size)** persist under **`%APPDATA%\\PolyMech\\pm-image\\settings.json`** (encrypted or JSON, depending on build). Dock splitter geometry is stored in the **registry** (`Polymech\\pm-image-ui`).

#### App Settings (theme + font size)

The **View → App Settings** ribbon button opens a modal:

- **Theme**: `System` (follow Windows `AppsUseLightTheme` registry — auto-switches when you toggle Windows dark mode), `Light`, `Dark`. Dark mode applies `DWMWA_USE_IMMERSIVE_DARK_MODE` to the title bar, `UseDarkMenu` to the menu, and themed colours to every dock panel (Settings, Find Results, Duplicate groups, Log, File Info, Queue, File Tree).
- **Font size**: `Default` (system message font, typically Segoe UI 9pt) or `+1 / +2 / +3 / +4 pt`. **Default is `+2 pt`** — noticeably more comfortable on high-DPI displays without being huge. Applies live to every panel and dialog (no restart).

Both settings are stored under `appearance` in `settings.json` (next to `providers`, `window`, and `prompt_presets`). The ribbon strip itself is rendered by the Windows Ribbon Framework and stays in the OS chrome scheme — that's a platform constraint, not a PM-Image limit.

Full behavior, feature flags, and Mermaid diagrams: **[docs/win32xx-ui.md](docs/win32xx-ui.md)**.

### `resize` — fit inside a box (default), write WebP / AVIF by extension

```bash
# Max 800×600, stay inside the box, Lanczos3 (default), write JPEG quality 85 (default)
pm-image resize photo.jpg out.jpg --max-width 800 --max-height 600

# Same, explicit quality
pm-image resize photo.jpg out.jpg --max-width 800 --max-height 600 -q 90

# WebP output (quality applies)
pm-image resize photo.jpg thumb.webp --max-width 400 --max-height 400 -q 82

# AVIF output (quality applies; needs HEIF/AVIF support in your libvips build)
pm-image resize photo.png out.avif --max-width 1200 --max-height 1200 -q 50

# Force output format when the path has no extension you trust
pm-image resize in.tif /tmp/out --format webp --max-width 512
```

### `resize` — square images (1:1)

Use the **same** `--max-width` and `--max-height` (that value is the square side in pixels). Pick **`--fit`**:

| `fit` | Result |
|-------|--------|
| **`cover`** | Fills the square; crops overflow (default crop: `--position centre`, or `attention` / `entropy` for smart crop). |
| **`contain`** | Full image inside the square; **letterboxing** on two sides if needed (`--background`). |
| **`fill`** | Stretches to the square (ignores aspect ratio). |

```bash
# 512×512 crop-to-square (avatars, thumbnails)
pm-image resize portrait.jpg avatar.jpg --fit cover --max-width 512 --max-height 512

# 1080×1080 WebP, smart crop on subject
pm-image resize product.png grid.webp --fit cover --max-width 1080 --max-height 1080 --position attention -q 85

# Square canvas, no crop — padded bands with a colour
pm-image resize panoramic.jpg square.jpg --fit contain --max-width 800 --max-height 800 --background '#111111'

# Exact square by stretching (rare)
pm-image resize any.jpg out.jpg --fit fill --max-width 256 --max-height 256
```

**REST / IPC JSON:** e.g. `"max_width": 512, "max_height": 512, "fit": "cover", "position": "attention"`.

### `resize` — cover (crop), contain (letterbox), rotate / flip

```bash
# Cover: fill 1200×630, crop centre (use --position attention for smart crop)
pm-image resize wide.jpg social.jpg --fit cover --max-width 1200 --max-height 630

# Contain: fit inside 800×600 canvas, letterbox with a background
pm-image resize logo.png padded.png --fit contain --max-width 800 --max-height 600 --background '#1a1a1a'

# EXIF autorotate (default), then rotate 90° CCW, vertical flip
pm-image resize img.jpg rotated.jpg --max-width 1024 --rotate 90 --flip
```

### `compress` — MozJPEG or optimised PNG

Re-encodes images for smaller files without changing geometry. **Compressor** defaults from the **output path extension** (e.g. `.jpg` → MozJPEG, `.png` → PNG); use **`--compressor mozjpeg|png`** to force. Omitted output path: writes beside the source with a **`_compressed`** stem suffix when the format stays the same, or an appropriate new extension (e.g. PNG → JPEG drops the suffix when the extension changes).

| Flags | Notes |
|-------|--------|
| `-q`, `--quality` | MozJPEG quality **1–100** (default **85**). |
| `--no-progressive` | Disable progressive JPEG. |
| `--optimize-scans`, `--trellis-quant` | MozJPEG tuning (smaller / slower). |
| `--level` | PNG DEFLATE **1–9** (default **9**). |
| `--quantize`, `--colors`, `--quant-quality` | Lossy PNG palette (**requires** CMake `FEATURE_PNG_COMPRESSOR`). |
| `--zopfli`, `--zopfli-iter` | Lossless ultra-DEFLATE (**requires** `FEATURE_PNG_ZOPFLI`). |
| `--no-strip` | Keep metadata (default strips). |
| `--suffix` | Custom stem suffix for auto output names (default `_compressed` when format unchanged). |

```bash
# Auto output: e.g. photo_compressed.jpg next to photo.jpg
pm-image compress photo.jpg

# Explicit MozJPEG output
pm-image compress in.png out.jpg --compressor=mozjpeg -q 82 --no-progressive

# PNG re-encode
pm-image compress graph.png graph-smaller.png --compressor=png --level 9

# Batch: many inputs → one directory
pm-image compress --src a.jpg b.png c.webp --dst ./out/
```

```bash
npm run test:media:compress
```

### `serve` — HTTP REST

```bash
# Default: http://127.0.0.1:8080 — GET /health, POST /v1/resize
pm-image serve --host 127.0.0.1 -p 8080
```

| Mode | `Content-Type` | Request | Response |
|------|----------------|---------|----------|
| **Upload** | `multipart/form-data` | Image part **`file`**, **`image`**, or **`upload`**; optional fields: `max_width`, `max_height`, `format`, `fit`, `quality`, … | **Binary image** (`Content-Type` matches format; default **JPEG** if `format` omitted). `Content-Disposition: inline; filename="resized…"`. |
| **Paths** | `application/json` | **`input`** and **`output`** (server-visible paths), plus resize / batch / cache fields | **JSON** — `{"ok":true}` or batch `count` / `outputs` |

**Upload (multipart):** the response body is the **processed image**, not JSON. The on-disk **output cache is not used** for uploads (each request uses temporary files).

```bash
curl -s http://127.0.0.1:8080/health
curl -s -o thumb.jpg -X POST http://127.0.0.1:8080/v1/resize \
  -F "file=@/path/in.png" \
  -F "max_width=400" \
  -F "quality=85"
```

**Paths (JSON):** same resize options as the CLI, with `input` and `output` paths (must be readable/writable by the server process).

```bash
curl -s -X POST http://127.0.0.1:8080/v1/resize \
  -H 'Content-Type: application/json' \
  -d '{"input":"/path/in.png","output":"/path/out.webp","max_width":400,"quality":80}'
```

Optional JSON: `"cache":false`, `"expand_glob":false`, `"cache_dir":"..."` — see **Batch paths & cache** above.

### `meta` — AI description / EXIF / JSON

Generates structured metadata for an image using an LLM (currently **Google Gemini**, default **`gemini-2.5-flash`**). Pipeline per file: **(1) optional in-memory resize** to ~512 px, **(2) read EXIF basics** via libvips, **(3) Gemini call** with `responseMimeType=application/json`, **(4) write any combination** of `<stem>.md`, `<stem>.json`, and an in-place EXIF `ImageDescription` update.

Default JSON shape (model is asked for these keys): `title`, `alt` (≤125 chars, SEO), `description`, `scene`, `estimated_location`, `characters[]`, `objects[]`, `tags[]`, `confidence`. The on-disk JSON also includes `source`, `model`, `provider`, and an `exif` object copied from the file.

```bash
# Plan check (no API call, no writes) — prints the resolved options + dimensions
pm-image meta photo.jpg --dry-run

# Real run: writes photo.md and photo.json next to the source
pm-image meta photo.jpg

# Multiple files into a separate folder, JSON only
pm-image meta photo1.jpg photo2.jpg shot.png --out-dir ./meta --no-md

# Update the source file's EXIF ImageDescription (rewrites the file)
pm-image meta product.jpg --no-md --no-json --update-exif
```

| Flag | Default | Notes |
|------|---------|-------|
| `input` (positional, repeatable) | **required** | One or more image paths |
| `--out-dir` | next to source | Folder for `.md` / `.json` |
| `--provider` | `google` | AI provider |
| `--model` | `gemini-2.5-flash` | e.g. `gemini-2.5-pro` for higher quality |
| `--api-key` | env `IMAGE_TRANSFORM_GOOGLE_API_KEY` | API key |
| `-p, --prompt` | built-in cataloguer prompt | Override the prompt entirely |
| `--no-resize` | (resize on) | Skip the in-memory resize |
| `--resize-width` | `512` | Longest-edge target before sending |
| `--no-md` | (md on) | Skip `<stem>.md` |
| `--no-json` | (json on) | Skip `<stem>.json` |
| `--update-exif` | off | Rewrite the source file with new EXIF `ImageDescription` |
| `--dry-run` | off | Validate args, run resize, print plan; no API call, no writes |

`--dry-run` is the recommended way to verify your options and the actual bytes that would be sent to the API.

### `find` — search images by name, by prompt, or by reference image

Two modes for surfacing matching images out of a folder, glob, or list of paths.

- **Name mode (default, free, no API)** — substring match on filename and parent folder names.
- **LLM mode (`--llm`)** — for each image build a *metadata corpus* (sidecar `<stem>.md`, `<stem>.json`, libvips EXIF) and ask an LLM judge "does this match?". Missing sidecars are **generated on the fly** (re-uses the `meta` worker), so the second `find` over the same folder is fast (cache hit) and cheap. The judge accepts an optional **prompt**, one or more **reference images** (`-r`), or both — references are streamed as multimodal parts so the model can match by visual similarity, not just by text.

#### Concrete examples

```bash
# 1. Substring search (instant, free, offline)
pm-image find ~/photos -p passport
pm-image find ~/Downloads ./client-imports -p invoice

# 2. Folder-name match — anything under any folder called "kids" or "trip-2024"
pm-image find ~/photos -p kids
pm-image find ~/photos -p trip-2024

# 3. Glob inputs (one shot across many directories)
pm-image find "D:/shoots/2026-*/raw" "D:/shoots/2026-*/edits" -p "raw"

# 4. Machine-readable output for scripts / pipelines
pm-image find ~/photos -p invoice --json | jq '.[] | .path'

# 5. Semantic LLM search — "find the dog photos"
pm-image find ~/photos -p "a dog (any breed) outdoors" --llm

# 6. Semantic LLM with a strong model + cache bypass
#    (re-describes every image — useful after switching models)
pm-image find ~/photos -p "outdoor portrait" --llm \
  --bypass-cache --model gemini-2.5-pro

# 7. Cache-only mode — no API calls if metadata is missing, fastest
pm-image find ~/photos -p "products on white background" --llm --no-generate

# 8. "Find more like this" — visual similarity by reference image
pm-image find ~/photos -p "more shots like this product photo" --llm \
  -r ~/photos/products/hero-shot.jpg

# 9. Brand search — find every photo that matches the brand mood/colours
pm-image find ~/website/assets -p "matches our brand style" --llm \
  -r ~/brand/logo.png -r ~/brand/style-sheet.png

# 10. Pure visual search (no text query, references only)
pm-image find ~/photos --llm \
  -r ~/photos/screenshots/error-dialog.png \
  -p "" --max 20
```

| Flag | Default | Notes |
|------|---------|-------|
| `input` (positional, repeatable) | **required** | Files, folders, or globs — recurses by default |
| `-p, --prompt` | required (Name) / optional in LLM if `-r` is set | The user search query |
| `--llm` | off | Switch to semantic LLM judge mode |
| `-r, --reference` | _(none)_ | LLM: reference image(s) the candidate should resemble. **Repeatable.** Sent as multimodal parts in every judge call. |
| `--case-sensitive` | off | Name mode: case-sensitive substring |
| `--no-folders` | off | Name mode: don't match parent folder names |
| `--no-recursive` | off | Don't recurse into directory inputs |
| `--bypass-cache` | off | LLM: ignore existing `.md`/`.json`/EXIF, always re-generate |
| `--no-generate` | off | LLM: skip images without cached `.md`/`.json` (no `meta_extract` call) |
| `--no-md` / `--no-json` / `--no-exif` | off | Skip individual cache sources |
| `--max N` | 0 (∞) | Truncate to N best matches (LLM mode is sorted by score) |
| `--dry-run` | off | Resolve candidates + corpus only; no LLM calls / no writes |
| `--json` | off | Print a JSON array with `{path, score, source, reason}` |
| `--provider` / `--model` / `--api-key` | google / gemini-2.5-flash / env | Same plumbing as `meta` |
| `--judge-prompt` | built-in | Override the yes/no decision prompt sent to the model |
| `--meta-prompt` | built-in | Override the cataloguer prompt used when **generating** missing cache |
| `--resize-width N` / `--no-resize` | 512 / off | Generation-only resize controls |

`pm-image find` is the CLI surface. The same logic is exposed as a library via `media::find_images(...)` and `media::match_metadata_against_prompt(...)` — the second is the seed of the future "compare image-or-corpus against prompt" primitive used for testing and end-user filtering.

#### In the UI (`--ui-next`)

1. Pick a folder in the **Explorer** dock, **or** select a few images, **or** drop files into the **Queue**.
2. Click **Find** on the Home ribbon → the **Settings** panel switches to Find mode.
3. Type your query, tick **Use LLM** if needed, and (LLM mode) **+ Add reference…** to attach example images.
4. Click **Run**. The **Find Results** panel pops to the front and fills as matches stream in.
5. Click a row to preview, right-click → **Reveal in Explorer panel** to jump to its folder.

#### Duplicates (`--ui-next`)

1. Pick a folder, selection, or queue (same as Find).
2. Click **Duplicates** on Home → **Settings** shows how to group (**file size** / **similar look (fingerprint)** / **text & EXIF (sidecar hash)** or **Use AI to compare JSON** for Meta `.json` fields).
3. Click **Run** → **Duplicate groups** shows a **tree** (group → files). Click a file to preview; **File info** lists **peers** in the group. Right-click for **open**, **copy paths**, **save session** / **open session**.

### `duplicates` — group same-size, similar, or same-meta images

Scans one or more paths and outputs **groups** of files that match under `--by` (`size` | `fingerprint` | `meta`). For **meta**, sidecars are `<stem>.md` and `<stem>.json` next to each image, plus EXIF; optional `--meta-compare-json-llm` uses the **chat LLM** (OpenRouter from settings) to score whether two JSON corpora describe the same thing before linking. Full flag table: [`docs/duplicates.md`](docs/duplicates.md).

#### Concrete examples

```bash
# Same file size (fast, many false positives on small files)
pm-image duplicates ./imports --by size

# Perceptual hash: exact same “look” (default dHash, strict)
pm-image duplicates ./photos/exports --by fingerprint

# Near-duplicates: re-saved JPEGs, slightly different dimensions
pm-image duplicates ./recompress-test --by fingerprint --max-hamming 4

# After `meta` has written .md/.json: group by same hashed sidecar+EXIF
pm-image duplicates ./catalog --by meta

# AI: link only when pair JSON is similar (7 = default; higher = stricter)
pm-image duplicates ./catalog --by meta --meta-compare-json-llm --meta-json-min-sim 7

# Machine-readable + a markdown report for review
pm-image duplicates ./event --by fingerprint --json --report-md ./event-dupes.md

# Session file: reopen in the app (View → Duplicate groups → open) or:
pm-image duplicates --load-session ./event-dupes.pm-duplicates.json
```

| Flag | Default | Notes |
|------|---------|-------|
| `input` (positional) | **required** unless `--load-session` | Files, folders, globs; `--no-recursive` to limit depth |
| `--by` | `fingerprint` | `size` \| `fingerprint` \| `meta` |
| `--min-group` | `2` | Only list groups with at least this many files |
| `--max-hamming` | `0` | `fingerprint` only: 0 = exact hash, up to 64 for looser match |
| `--fingerprint-same-size-only` | off | `fingerprint`: only compare files with equal byte size (faster with `--max-hamming` > 0) |
| `--no-md` / `--no-json` / `--no-exif` | off | `meta`: skip that part of the corpus before hashing |
| `--meta-compare-json-llm` | off | `meta`: LLM pair-compare of JSON fields, not byte hash |
| `--meta-json-min-sim` | `7` | `meta+LLM`: min pairwise score 0..10 to link |
| `--llm-router` / `--llm-model` / … | from settings | Override OpenAI-compatible API for pair compare |
| `--json` | off | Print groups to stdout |
| `--report-md` / `--report-json` | — | Full diagnostic report (markdown or JSON) |
| `--save-session` / `--load-session` | — | v2 session JSON: save a scan or validate/reload one |

### `transform` — AI image editing

Uses a generative-AI model (currently **Google Gemini**) to edit an image based on a text prompt. Reads the input, sends image + prompt (and optional **reference images** — logo / brand sheet / style swatch) to the API, and writes the result.

```bash
pm-image transform photo.jpg -p "remove the background"
pm-image transform photo.jpg out.png -p "make it black and white" --model gemini-3-pro-image-preview

# Apply a brand reference (e.g. add a logo, match a colour palette)
pm-image transform product.jpg -p "place the brand logo in the lower-right corner" \
  -r assets/brand/logo.png

# Multiple references (logo + style sheet)
pm-image transform product.jpg out.png \
  -r assets/brand/logo.png \
  -r assets/brand/style-sheet.png \
  -p "match the colour palette and add the logo subtly to the bottom-right"
```

| Flag | Default | Notes |
|------|---------|-------|
| `input` (positional) | **required** | Input image path |
| `output` (positional) | auto from input + prompt | Output path |
| `-p, --prompt` | **required** | Editing prompt |
| `--provider` | `google` | AI provider |
| `--model` | `gemini-3-pro-image-preview` | Model name |
| `--api-key` | env `IMAGE_TRANSFORM_GOOGLE_API_KEY` | API key |
| `--aspect-ratio` | auto | `1:1`, `16:9`, `4:3`, `3:4`, `9:16`, `21:9` |
| `--image-size` | `1K` | `512`, `1K`, `2K`, `4K` |
| `-r, --reference` | _(none)_ | Reference image(s) — logo, brand sheet, style swatch. **Repeatable**: `-r logo.png -r palette.jpg`. Sent as additional multimodal parts. |

If `--api-key` is omitted, the value of the **`IMAGE_TRANSFORM_GOOGLE_API_KEY`** environment variable is used.

### `ipc` — one JSON line per connection (TCP; Unix socket on Linux/macOS)

```bash
pm-image ipc --host 127.0.0.1 -p 9333 --cache-dir ./cache/images
# elsewhere: send a single line, read one line back, e.g.
# {"input":"/tmp/a.jpg","output":"/tmp/b.webp","max_width":320,"format":"webp","cache":true}
```

Same JSON fields as REST (`input`, `output`, globs, `expand_glob`, `cache`, `cache_dir`, resize options).

### `llm` — pm-image ops as agent-callable LLM tools

The same workers (`resize`, `compress`, `transform`, `meta`, `find`) are exposed as JSON-Schema-described tools. Same surface across CLI, REST (`/v1/llm/*`), and IPC (`op:"llm.tools/*"`). See [docs/llm-tools.md](docs/llm-tools.md) for the design + envelope contract.

```bash
# List the catalog (one entry per op)
pm-image llm tools-list | jq '.tools[].name'
# → "image_resize" "image_compress" "image_transform" "image_meta" "image_find"

# Invoke a tool — image bytes are passed inline as base64
echo '{ "options": {"compressor":"mozjpeg","quality":70} }' > /tmp/args.json
pm-image llm tools-call --name image_compress \
                        --args /tmp/args.json \
                        --image-file in.png
# → {"ok":true,"mime":"image/jpeg","bytes":884,"b64":"/9j/2wBDAA..."}
```

### `llm agent` — in-process chat agent (path tools)

The `polymech::kbot::LLMClient` library (`packages/kbot/`) powers **`pm-image llm agent`**: one user prompt, optional file paths, and the model may call path tools (`image_resize`, `image_meta`, …) in a loop. Routers include **openrouter / openai / deepseek / gemini / ollama / fireworks / xai / huggingface** (see `--router` and app Chat settings).

```bash
pm-image llm agent -p "compress these with MozJPEG 80" --paths a.png b.jpg
pm-image llm agent -p "list images in this folder" --folder . --dry-run
```

See [docs/llm-tools.md](docs/llm-tools.md) for the tool catalog and REST/IPC `llm` surface.

### `app` — talk to the running pm-image UI (Windows only)

`pm-image app <verb>` sends a named command to the running UI's `WM_COPYDATA` bridge and exits. Use it from shells, scheduled tasks, or hotkey daemons to drive the UI from outside. Returns non-zero if no pm-image UI is running. The same verbs are also bound to in-app keyboard shortcuts.

| Verb | Shortcut | Effect |
|------|----------|--------|
| `takescreenshot` | **Alt+P** | Captures the main window's on-screen pixels and writes `<cwd>/screenshots/pm-image-YYYYMMDD-HHMMSS.png`. (Will move to `%APPDATA%\Polymech\pm-image\screenshots\` once the AppData layout is finalised.) |

```bash
pm-image                 # launch the UI (no subcommand)
pm-image app takescreenshot   # tell it to drop a PNG under .\screenshots\
```

See [`docs/win32xx-ui.md` § App commands](docs/win32xx-ui.md) for the bridge protocol and the one-page checklist for adding a new verb.


## Tests

From `packages/media/cpp`, after **`cmake --build --preset release`** (or `npm run build:release`):

```bash
npm run test:media                  # full suite
npm run test:media:rest             # REST only (JSON + multipart)
npm run test:media:multipart        # multipart upload → image body only
npm run test:media:templates        # `${SRC_*}` REST + IPC + CLI
npm run test:media:glob             # recursive glob + templates (PNG under glob-in/)
npm run test:media:glob:raw         # same for tests/assets/raw/**/*.arw (skips if none)
npm run test:media:ipc              # IPC only
npm run test:media:url              # HTTP(S) URL inputs — needs network
npm run test:media:compress         # compress CLI (MozJPEG + PNG)
npm run test:media:meta             # meta CLI: dry-run + live Gemini (needs API key in env or settings)
npm run test:media:llm              # LLM tools: catalog + image_compress over CLI/REST/IPC
```

Requires a built **`dist/pm-image`** linked against **libvips** and fixture PNGs (`npm run generate:assets` if missing).

Each run overwrites **`tests/test-report-last.md`** with Markdown from **`orchestrator/reports.js`**: **host** (CPU, RAM, load), **Node process** (CPU/memory deltas, RSS), timing, plus **image rows** (fixture and response **byte sizes**, PNG **pixel dimensions** when known, and multipart/JSON **output file** sizes).

The suite covers **REST** (JSON paths and **multipart upload**), **IPC (TCP)**, optional **Unix socket** on non-Windows, **destination templates**, and **recursive glob + `${SRC_DIR}` / `${SRC_NAME}`** (`test:media:glob` — outputs under `tests/assets/glob-in/**/out/`, gitignored, for manual inspection). **`test:media:glob:raw`** runs the same glob flow against user-supplied **`.arw`** files under `tests/assets/raw/` (skipped when the folder is missing or has no ARW files).

**HTTP URL smoke tests** (downloads from **picsum.photos**, needs network):

```bash
npm run test:media:url
```

## License

See [LICENSE](LICENSE) in this directory when present.

## Testing - UI

- https://github.com/microsoft/winappdriver?tab=readme-ov-file
- https://github.com/pywinauto/pywinauto


