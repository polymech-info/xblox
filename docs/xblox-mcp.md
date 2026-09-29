# xBlox MCP Blocks — Implementation Plan

Goal: let xBlox documents call MCP servers directly (an `mcpCall` block), reusing
the existing MCP client layer in `src/llm/`, with per-tool dynamic parameter
schemas (the Replicate/Whisper `.dyn()` pattern). Roadmap item: "mcp client :
via cli & xblox" (`docs/releases/v1.0.md`).

Legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` open decision

**Status:** Phases 0–1, 3, 7, Phase 8 Tiers 1–2, and Phase 2 UI resolvers (C++)
are shipped (orchestrator green). **Next:** Phase 2c property-panel flow
(server → tool catalog → tool dropdown → variable-capable tool params).

---

## Phase 0 — Shared MCP config/session module (dedupe first) ✓

The config-load + session-factory logic is currently copy-pasted in three
places (`mcp_probe.cpp`, `llm/tools/mcp/McpBridge.cpp`,
`win/ui_next/CSettingsWebView.cpp::settingsMcpToolCall`). Extract once so the
new block, settings, probe, and bridge all share it.

- [x] Create `src/llm/mcp_config.hpp` / `src/llm/mcp_config.cpp` (gate `FEATURE_MCP_CLIENT`).
- [x] `load_mcp_config(const std::filesystem::path& dir, nlohmann::json& out, std::string& err)`
  - [x] Secure path first (`pm://config/mcp.json` via `load_or_import_seed_document`) when `FEATURE_SECURE_STORAGE`.
  - [x] Plain `mcp.json` fallback otherwise. Match `mcp_probe.cpp` exactly.
- [x] `list_mcp_servers(const nlohmann::json& cfg) -> std::vector<std::string>` (skip `enabled:false`).
- [x] `create_mcp_session(const nlohmann::json& server_cfg, const std::string& server_name, std::string& err) -> std::unique_ptr<IMcpSession>`
  - [x] Parse `type` / `url` / `command` / `args` / `env` / `headers`.
  - [x] Bare-URL-in-`command` → HTTP (all three current copies do this).
  - [x] stdio → `StdioMcpClient` + `parse_stdio_launch_options()` (sandbox!).
  - [x] http → `StreamHttpClient`.
- [x] Move `parse_stdio_launch_options()` + `parse_header_map()` here (currently duplicated in `mcp_probe.cpp` and `McpBridge.cpp`).
- [x] Move `parameters_from_mcp_tool()` here (currently `McpBridge.cpp:505`) — used to turn a tool's `inputSchema` into a params schema.
- [x] Share `resolve_json_templates()` (currently static in `network_blocks.cpp`)
  so both `fetch` and `mcpCall` use one `${var}`/`${USER:}`/`${ENV:}` resolver.
  Put it in a small shared header rather than duplicating (see Phase 2 parity).
  → `src/xblox/blocks/json_templates.{hpp,cpp}`; mechanical parsing via `string_utils::substitute_variables`.
- [x] `mcp_server_policy_allows()` / `require_registered_mcp()` — expose or keep in bridge; the block must enforce policy too (see Phase 4).
- [x] Add `mcp_config.cpp` to `CMakeLists.txt` under the `if(FEATURE_MCP_CLIENT)` block (~line 1751).

### Move the existing tools cache here too

There is already a disk-backed MCP tools cache — but it lives **privately** inside
`src/llm/llm_info_compact.cpp` (anonymous namespace), coupled to the planner
"compressed markdown" builder. Details:

- Cache file: `<config_dir>/mcp-tools-cache.json` (env override `POLYMECH_MCP_TOOLS_CACHE`).
- `version:1`, TTL `k_mcp_tools_cache_ttl_seconds = 8h`.
- Invalidated when `mcp.json` mtime **or** path changes (`mcp_cache_is_fresh`).
- Stores the whole `probe_mcp_config()` result → **includes per-tool `inputSchema`** (exactly what the block UI needs).
- Already-public accessors in `llm_info_compact.hpp`:
  - `nlohmann::json mcp_probe_cached(bool allow_probe, std::string* note)`
  - `std::vector<McpToolInventoryEntry> mcp_tool_inventory(bool allow_probe)`

- [x] Extract the cache primitives (`mcp_json_path`, `mcp_tools_cache_path`,
  `mcp_cache_is_fresh`, `load/save_mcp_tools_cache`, `load_or_probe_mcp_tools`)
  out of `llm_info_compact.cpp` into `mcp_config.cpp`.
- [x] Re-expose `mcp_probe_cached()` / `mcp_tool_inventory()` from the shared
  module (or keep the `llm_info_compact` signatures as thin forwarders so the
  4 existing callers — `pm_image_cmd_llm_info.cpp`, `agent_feature.cpp`,
  `tools/info/InfoTool.cpp`, `llm_info_compact.cpp` — keep compiling).
- [x] Confirm no include cycle: `block_params_ui.cpp` (xblox) → `mcp_config`
  (llm) — same library target, already used by `network_blocks_mcp.cpp`.

---

## Phase 1 — `mcpCall` xBlox block ✓ (handler; UI routes Phase 2)

New TU mirroring `data_blocks.cpp` structure (handler + `register_*`).

- [x] Create `src/xblox/blocks/network_blocks_mcp.cpp`.
- [x] Add `src/xblox/blocks/network_blocks_mcp.cpp` to `CMakeLists.txt` (near `network_blocks.cpp`, ~line 1792).
- [x] Declare `void register_network_mcp_blocks(BlockRegistry&)` in `network_blocks.hpp`.
- [x] Call it from `register_network_blocks()` in `network_blocks.cpp`, `#if FEATURE_MCP_CLIENT`.
- [x] Implement `BlockResult mcp_call_block(BlockIO& io)`:
  - [x] `#if !FEATURE_MCP_CLIENT` → `io.error("mcpCall", "MCP client disabled in this build", 1)`.
  - [x] Load config via `mcp_config` helper; error if server not found.
  - [x] `create_mcp_session(server)`; enforce policy (`mcp_server_policy_allows` / `require_registered_mcp`).
  - [x] `client->set_request_options(io.i64("timeoutMs", ...), [&]{ return io.cancelled(); })`.
  - [x] `initialize()` → on failure `try_delete_session()` + `io.error(...)`.
  - [x] `tools_call(tool, resolve_json_templates(io, io.json("arguments")))` → `try_delete_session()`.
  - [x] Store result: `io.result(Value::from_json(result), ParamKind::json_value)`.
  - [x] Emit ok event with `{server, tool, result}`.
- [x] Register block:
  - [x] `registry["mcpCall"] = bdb("mcpCall", mcp_call_block, "MCP Call", "Network", <desc>, <defaults>)`.
  - [x] `.params({...})` (Phase 2 param defs inlined).
  - [x] `.exec({false, true, true})` — has side effects, cancellable.
  - [ ] Decide `.iterable()` if result is a JSON array (like `fetch`) — [?].
- [x] Wire handler into dispatch: `builtin_block_handler` already covers registry; no `network_block_handler` change needed.

---

## Phase 2 — Params + dynamic schemas (Replicate pattern) · C++ resolvers ✓

Reference: `image_blocks.cpp::ai_image_params()` + `docs/xblox/ui-schema.md`.
No web-app provider branching — declare everything in C++ `.dyn()`/`.ui()`.

- [x] `server` param — `pd_str("server").in().req()` with
  `.ui({{"control","select"},{"options_path","mcp.servers"}})`.
  - Verified: renders via `selectOptionsForParam` → `ParamOptionsSelect` in the
    generic `renderParam` path (panel line ~408/1540). **No picker needed.** ✓
  - Caveat: `options_path` is **static** — `ParamOptionsSelect` does NOT
    interpolate `{field}` tokens. So `server` (static path) is fine; `tool`
    (server-dependent) is not (see below).
- [x] `tool` param — `pd_str("tool").in().req()`, dynamic options dependent on `server`.
  - **Blocked on a web change** (see Phase 2b). The existing `options_list`
    typeahead is hardcoded to a param literally named `model` (panel line 1097)
    and only rendered inside `ProviderModelPicker`, which only appears when
    `detectProviderMode` matches `provider`/`model` naming (lines 189-205).
    A `server`/`tool` block gets `pickerMode = null`, so without a web change the
    `tool` field degrades to a **plain free-text input** (usable fallback).
- [x] `arguments` param — `pd_json("arguments").grp("request").lbl("Arguments")` with
  `.dyn({{"kind","provider_options"},{"schema_path","mcp.tools"},{"provider_field","server"},{"model_field","tool"},{"schema_method","xbloxProviderOptionsSchemaGet"},{"value_field","arguments"}})`.
  - Verified: `DynamicProviderOptionsGroup` renders at panel line ~1640, gated
    **only** on `record[provider_field]` non-empty — independent of the picker.
    Reads `provider_field`/`model_field` overrides, so provider=server /
    model=tool works. Query key includes both → refetches on server/tool change.
    Supports `x-xblox.kind` widgets + `visible_when`. ✓
  - Note: group title is hardcoded `"Model Options"` (panel line 802). For MCP it
    would read "Model Options", not "Arguments". [?] Add `title` to wire payload
    if we want a better label (small panel change).
- [x] `timeoutMs` — `pd_ms("timeoutMs").grp("timeouts").dflt(60000)` (match `PM_MCP_TIMEOUT_MS` default).
- [x] `pd_store_as().dflt("mcpResult")`.

### Param/UI parity with network_blocks (must match look & feel)

The block should read and feel like `fetch`/`httpRequest`. Concretely:

- [x] Use the same builder idioms: `pd_str`/`pd_json`/`pd_ms`/`pd_store_as`,
  `.grp("request"|"auth"|"timeouts")`, `.desc("… Supports ${var} interpolation.")`.
- [x] **Variable substitution in `arguments` is required and must match network.**
  Resolve the JSON via the same two-pass template resolver network uses:
  `resolve_json_templates(io, io.json("arguments"))` —
  - Pass 1: context vars via `io.context(name)`, with **whole-value passthrough**
    (a string that is exactly `${var}` returns the JSON value, not a stringified copy).
  - Pass 2: `bv::resolve` for `${USER:K}`, `${ENV:K}`, `${PIXLWIZ}`,
    `${KNOWNFOLDER:…}`, bare user vars.
  - This makes MCP `arguments` behave exactly like network `bodyJson`/`headers`.
  - Reference: `tests/xblox/network-deepl.xblox` uses
    `headers: {"Authorization":"DeepL-Auth-Key ${USER:DEEPL_KEY}"}` and
    `bodyJson: {"document_key":"${documentUpload.document_key}"}`.
- [x] `resolve_json_templates` is currently a **static** helper in
  `network_blocks.cpp` (anon namespace). Since `mcpCall` lives in a new TU, either
  (a) promote it to a shared header (e.g. `block_variables.hpp` / a small
  `json_templates.hpp`), or (b) duplicate the small helper. Prefer (a).
  → Done: `src/xblox/blocks/json_templates.{hpp,cpp}` + `string_utils` token parser.
- [ ] Optional literal `headers` param on the block (mirroring network's
  `pd_str("headers")` — JSON object or `"Name: value"` array) so a user can add
  per-call headers with `${USER:…}` substitution, same as deepl. This coexists
  with the config-level `headers` and the URL-detected router auth (Phase 4).
- [x] `timeoutMs` grouping/labels mirror network's `timeouts` group.

## Phase 2b — Web change for the server-dependent `tool` dropdown

Needed because the generic `options_list` typeahead is `model`-only and
picker-bound. Pick one:

- [ ] **Preferred:** generalize `ParamOptionsSelect` to accept an
  `options_path_template` (with `{field}` substitution from the block record)
  in addition to the static `options_path`. Then a plain
  `ui.control:"select"` + `ui.options_path_template:"mcp.tools.{server}"`
  renders a server-dependent dropdown via the existing generic `renderParam`
  path — reusable for any future block, no provider naming required.
  - [ ] Extend `selectOptionsForParam` (line 408) to read `options_path_template`.
  - [ ] Substitute tokens with `dynamicOptionsPath(template, record)` (already used elsewhere).
  - [ ] Empty path (unresolved `{server}`) → disabled/empty select.
- [ ] Alternative: accept the free-text `tool` fallback for v1, defer the dropdown.
- [ ] Reject: renaming to `provider`/`model` (would trip `detectProviderMode` into
  STT-picker semantics with the LLM catalog — wrong UX).

### Variable substitution across ALL tool params (incl. enums)

Requirement: every dynamic MCP tool arg (from `inputSchema`) should accept
`${var}` / `${USER:…}` / `${ENV:…}`, not just strings. Runtime is already
covered — `arguments` flows through `resolve_json_templates` (whole-value
passthrough converts a stored `"${var}"` into the real typed value before the
call). The **UI** is the gap. Current `DynamicProviderOptionsGroup` support:

| Schema field | Widget | `${var}` today |
|--------------|--------|----------------|
| `x-xblox.kind` path/list/prompt/color | Variable(Input/Text)Field | ✓ |
| enum + `x-xblox.kind:"editable_select"` | `EditableEnumInput` (free text + presets) | ✓ |
| enum (plain) | `Select` dropdown | ✗ dropdown only |
| boolean | checkbox | ✗ |
| integer / number | number input | ✗ |
| string (fallback) | `VariableInputField` | ✓ |

- [ ] **Enums:** make dynamic-schema enums variable-friendly. Cheapest: have
  `annotate_schema_param_kinds` (C++) tag enum props as `editable_select` so they
  render as a free-text-with-presets combobox (mechanism already exists, panel
  L945). Alternatively, switch the dynamic enum branch (panel L957) to
  `EditableEnumInput` by default.
- [ ] **Numbers / booleans:** no variable-capable widget today. Options: a shared
  "allow `${var}` override" affordance, or **new param kinds** (e.g. a
  variable-enabled numeric field / tri-state) — decide when we hit a real MCP
  tool that needs it. [?] Defer until a concrete schema forces it (per user).
- [ ] Sanity: whatever widget is used, it must store the raw `${var}` string so
  `resolve_json_templates` can resolve it at run time (don't coerce to a number in
  the UI when the value is a variable ref).

## Phase 2c — Property panel UI flow (server → tool → params)

Target UX (same mental model as image `providerOptions` in `image_blocks.cpp`):

```
select / create server  →  load tool catalog  →  select tool (dropdown)  →  show tool params (${var} on every field)
```

**Decided (2026-07-09):**

- **Max verbosity, no degradation** — full grouped UI is the target everywhere.
  Where a widget can't render yet (nested objects/arrays, tri-state booleans),
  ship a **stub** (per-field raw JSON textarea) and keep a todo — never a
  permanent "whole group falls back to raw JSON" cut.
- **Custom supports stdio too** — no HTTP-only v1 restriction.
- **Load tools is explicit** — a button; the panel never auto-probes a custom
  `serverConfig` (documents can carry arbitrary `command`s).
- **Custom configs stay inline** — never merged into the user's `mcp.json`
  (at least for now). The block document is the single owner of `serverConfig`.
- **Custom tool-schema cache** — same mechanism as the main probe cache:
  `<config_dir>/mcp_server-<slug>_cache.json`, same 8h TTL
  (`k_mcp_tools_cache_ttl_seconds`), where `<slug>` = stable hash/slug of the
  normalized `serverConfig` JSON. Invalidate when the config hash changes.

Reference implementations:

- **Hop 1 (nested select):** `image_blocks.cpp` — `replicateCollection` static
  `options_path` + `model` `dyn.options_path_template`.
- **Hop 2 (dynamic params):** `image_blocks.cpp` — `providerOptions` with
  `dyn.kind=provider_options` → `DynamicProviderOptionsGroup`.
- **Settings server fields (Custom only):** `apps/shared/components/settings/McpSettingsPanel.tsx`
  + `apps/shared/settings/mcp.ts` (`McpServerConfig`, `mcpServerConfigToForm`).

### Step 1 — Server: select or create

- [ ] **Server dropdown** — extend `mcp.servers` options (`block_params_ui.cpp`:
  `mcp_servers_options()`):
  - [ ] **User** servers — probed profile `mcp.json` entries (current behaviour).
  - [ ] **System** servers — merge bundled defaults (e.g. `dist/data/mcp.json`)
    into the list; mark with `group:"system"` (or `label` suffix) so the panel can
    optgroup them.
  - [ ] **Custom** sentinel — append `{value:"__custom__", label:"Custom…"}`.
- [ ] **C++ manifest** (`network_blocks_mcp.cpp`):
  - [ ] Keep `server` as `ui.control=select` + `options_path=mcp.servers`.
  - [ ] Add `serverConfig` param — `pd_json("serverConfig").grp("request")` with
    `.dyn({kind:"mcp_server_config", schema_path:"mcp.server", value_field:"serverConfig", …})`.
  - [ ] `visible_when`: only show `serverConfig` when `server == "__custom__"` (panel
    or C++ `ui.fields` on wire payload).
- [ ] **Custom server editor** — new `DynamicMcpServerConfigGroup` in
  `xblox-property-panel.tsx` (or extend `DynamicProviderOptionsGroup` with wire
  `title` + `ui.groups`):
  - [ ] Static schema from `mcp_server_config_schema()` in `block_params_ui.cpp`
    (Whisper-style grouped fields: transport / stdio / http / timeout).
  - [ ] Reuse field row layout from `McpSettingsPanel.tsx` (`type`, `command`,
    `args`, `url`, `headers`, `env`, `tool_timeout`) — extract shared subcomponents
    into `apps/shared/components/settings/`; do **not** pull in ping/tool-toggle cards.
  - [ ] **JSON drop / paste** — drop zone or paste button on the group:
    - [ ] Accept `{command,args,url,…}`, `{name:{…}}`, or `{mcpServers:{name:{…}}}`.
    - [ ] Normalize via `mcpServerConfigToForm` / `mcpServerFormToConfig` (`mcp.ts`).
    - [ ] `onPatch({ serverConfig })` — grouped fields and stored JSON stay in sync.
- [ ] **Runtime** (`network_blocks_mcp.cpp` `mcp_call_block`):
  - [ ] When `server == "__custom__"`, use `io.json("serverConfig")` instead of
    `cfg["mcpServers"][server]`; ephemeral key `"__inline__"` for policy logging.
  - [ ] Resolve `serverConfig` through `resolve_json_templates` (headers/env/args
    may carry `${USER:…}` — this is the sanctioned way to keep secrets out of docs).
  - [ ] Custom configs are **never written** to the user's `mcp.json` (decided) —
    no "save to profile" action for now. [ ] todo (later): optional "promote to
    profile server" affordance.
  - [ ] Policy: inline stdio goes through the same `mcp_server_policy_allows` /
    `require_registered_mcp` checks as profile servers (stdio **is** in scope —
    decided). Inline configs can't be `security.registered`, so when
    `requireRegisteredMcp` is on, fail with a clear "inline MCP servers are
    blocked by policy" error (not a generic policy string).

### Step 2 — Tool catalog (server-dependent)

- [ ] **Named server** — tool list from cached probe (already routed):
  - [ ] `resolve_options_path("mcp.tools.{server}")` ✓ (C++ done).
  - [ ] Wire dropdown (Phase 2b below).
- [ ] **Custom server** — catalog via explicit **Load tools** (decided; never auto):
  - [ ] Add `xbloxMcpProbeInline` RPC (`CBlockView.cpp` → new
    `mcp_probe_inline(server_cfg)` in `mcp_config`) → handshake + `tools/list`
    + per-tool `inputSchema` for a `serverConfig` object. Triggered **only** by
    the Load tools button — the panel must not fire it on render/mount.
  - [ ] **Disk cache (decided):** `<config_dir>/mcp_server-<slug>_cache.json`,
    same shape + 8h TTL as `mcp-tools-cache.json`; `<slug>` = hash of normalized
    `serverConfig`. Config change → new slug → cold cache. Load tools with a
    fresh cache reads from disk; the button re-probes when stale or on
    shift/long-press "force" (TBD affordance).
  - [ ] [ ] todo: GC/cap for orphaned `mcp_server-*_cache.json` files (config
    edits leave old slugs behind).
  - [ ] TanStack layer keyed on the same slug (in-memory dedupe on top of disk).
  - [ ] Empty / invalid config → disabled tool select + inline validation error
    (no spawn, no RPC).
- [ ] **Refresh tools** button for named servers too (re-probe bypassing disk
  TTL; hot UI paths use `allow_probe=false` — see edge cases below).

### Step 3 — Tool dropdown

- [ ] **Phase 2b (named servers)** — `network_blocks_mcp.cpp`:
  ```cpp
  pd_str("tool").in().req().grp("request")
      .ui({{"control","select"},
           {"options_path_template","mcp.tools.{server}"},
           {"empty_label","(select server first)"}})
  ```
- [ ] **Web** (`xblox-property-panel.tsx`):
  - [ ] `selectOptionsForParam` — prefer `ui.options_path_template` over static
    `options_path`; resolve with `dynamicOptionsPath(template, record)`.
  - [ ] `ParamOptionsSelect` — pass resolved path; empty/unresolved `{server}` →
    disabled select with empty label.
  - [ ] On `server` change → clear stale `tool` + `arguments` (mirror provider/model
    change behaviour in image blocks).
- [ ] **Custom server** — `tool` select backed by inline-probe options (Step 2), not
  `mcp.tools.{server}` path.

### Step 4 — Tool params (variable-capable, image-block parity)

Mirror `image_blocks.cpp` `providerOptions` + `DynamicProviderOptionsGroup`:

- [ ] **C++** — `arguments` dyn metadata already matches `providerOptions` pattern ✓;
  ensure `mcp_tool_options_schema` keeps calling `annotate_schema_param_kinds`.
- [ ] **Wire payload** (`mcp_tool_options_schema` in `block_params_ui.cpp`):
  - [ ] Add `"title": "Tool arguments"` (or read from param label) so the panel does
    not show hardcoded `"Model Options"`.
- [ ] **Web** (`DynamicProviderOptionsGroup`):
  - [ ] Read optional `payload.title` for `PropGroup` heading.
  - [ ] **Full `${var}` support on all field types** (image-block parity):
    - [ ] Enums → `editable_select` via `annotate_schema_param_kinds` (C++) or default
      `EditableEnumInput` in panel enum branch.
    - [ ] Integer / number → allow `${var}` draft (same pattern as `NumericParamInput`
      in generic `renderParam`: if trimmed value contains `${`, commit as string).
    - [ ] Boolean → tri-state or `${var}` text override when schema allows (or tag as
      `editable_select` with `true`/`false` presets). [?] pick when first tool needs it.
  - [ ] Path / prompt / string fields already use `VariableInputField` / `VariableTextField` ✓.
  - [ ] **Nested objects / arrays** (MCP `inputSchema` is arbitrary JSON Schema —
    unlike curated Whisper/Replicate): render unsupported property types as a
    **per-field raw JSON textarea stub** (decided: stub + todo, no whole-group
    degradation). [ ] todo: real nested-object/array widgets (recursive group
    rendering) in a later pass.
- [ ] **JSON drop on tool params** (optional, same group):
  - [ ] Accept `{"tool":"…","arguments":{…}}` or bare arguments object → patch
    `tool` + `arguments` together.
- [ ] **Gating** — render group only when `server` (or valid `serverConfig`) **and**
  `tool` are non-empty; show schema fetch error inline (existing pattern at ~L1643).
- [ ] **No-args tools** — schema is null: show an explicit "(this tool takes no
  arguments)" empty state, not a blank panel.

### Edge cases & decisions (pre-coding review, 2026-07-09)

Design-blocking items resolved:

- [x] Inline probe = code execution from a document → **explicit Load tools
  button only** (decided). Also applies to named servers on hot paths:
  - [ ] Switch UI resolvers (`mcp.servers`, `mcp.tools.*`, schema) to
    `mcp_probe_cached(allow_probe=false)`; live probing only via the explicit
    refresh/Load-tools RPC (closes the Phase 5 `[?]`).
- [x] Custom transport scope → **stdio included** (decided; no HTTP-only cut).
  Policy story per Step 1 runtime todos.
- [x] Custom config ownership → **inline only, never merged into `mcp.json`**
  (decided).
- [x] Custom schema cache → **`mcp_server-<slug>_cache.json`, 8h TTL** (decided).
- [x] Unsupported schema shapes → **stub widgets + todos, no degradation**
  (decided).

Remaining implementation-time todos:

- [ ] **Secrets in `serverConfig`:** headers persist plaintext in `.xblox` docs.
  Runtime `${USER:…}` resolution (Step 1) is the mitigation; add an editor hint
  ("use ${USER:KEY} for secrets") + never echo resolved headers in events
  (mask like `network_blocks.cpp`). Slug hashing must use the **raw** (unresolved)
  config so secrets don't leak into cache filenames.
- [ ] **Two dyn params on one block:** `GenericNativeParamEditor` singular
  `find(dyn.kind === "provider_options")` — new `mcp_server_config` kind must be
  hidden from the generic param list **and** rendered by its own group (panel
  currently only filters `provider_options`).
- [ ] **`visible_when` on native params doesn't exist** — add `ui.visible_when`
  to `BlockParamUi` (manifest.ts + panel) so `serverConfig` shows only when
  `server == "__custom__"`; generic mechanism, not MCP-hardcoded.
- [ ] **`${var}` in `server`:** select can't express it; `withCurrentDeviceOption`
  shows it as "(current)"; `mcp.tools.${srv}` resolves to empty options — must
  degrade to free-text fallback without breaking the panel.
- [ ] **Server referenced but missing on this machine** (docs travel): show a
  "server not found in profile" hint instead of a silently empty arguments panel.
- [ ] **Sentinel/name collisions:** user server literally named `__custom__`
  (reject/prefix), system-vs-user duplicate names after merging bundled defaults
  (user wins).
- [ ] **Stale `tool`/`arguments` on server/tool change:** clear on user-initiated
  change only — never on document load (guard against effect-on-value clearing).
  MCP servers commonly **reject** unknown args (unlike Replicate ignoring them).
- [ ] **`${var}` in numeric fields:** committed as string; whole-value passthrough
  restores type only if the variable holds a typed value — document; [ ] todo:
  optional schema-aware coercion pass at runtime.
- [ ] **JSON paste normalization:** generic `fromJson` contrib does a raw merge —
  the MCP contrib must normalize the three shapes (bare / `{name:{…}}` /
  `{mcpServers:{…}}`) before patching; multi-server snippet → take first + report.
- [ ] **CLI parity:** `--server __custom__` with missing/empty `serverConfig` →
  same clean block error as the panel path.
- [ ] **Simulate mode:** skip the inline probe as well as the call.
- [ ] **Secure storage:** system+user merge and cache freshness assume plain
  `mcp.json` mtime; under `FEATURE_SECURE_STORAGE` fall back to hash-based
  invalidation (existing open decision, applies to the new slug caches too —
  slug caches are hash-keyed already, so only the main cache is affected).

### Panel layout (single request group)

Suggested `PropGroup` order in `GenericNativeParamEditor` for `mcpCall`:

1. **MCP Server** — `server` select (+ `serverConfig` group when Custom).
2. **Tool** — `tool` select (catalog from Step 2).
3. **Tool arguments** — `DynamicProviderOptionsGroup` on `arguments` (Step 4).
4. **Timeouts** — `timeoutMs`.
5. **Output** — `storeAs`.

### Tests & smoke

- [ ] Harness subgroup `mcp-ui-panel` (or extend `mcp-ui`): assert manifest declares
  `options_path_template` on `tool`, `mcp_server_config` dyn on `serverConfig`.
- [ ] Manual smoke: select profile server → tool dropdown populates → change tool →
  argument fields refetch; set `${var}` on enum/number fields → block JSON retains
  raw string → `xblox run` resolves at runtime.
- [ ] Custom path smoke: `__custom__` + pasted `mcp.json` snippet → tool catalog →
  call (gated `PM_MCP_LIVE` or local stdio fixture).

### block_params_ui.cpp routes ✓

- [x] `resolve_options_path("mcp.servers")` → array of `{value,label}` from the
  cached probe (`mcp_probe_cached().servers`), **not** a fresh handshake.
- [x] `resolve_options_path("mcp.tools.{server}")` → array from the cached probe's
  `servers[name].tools`. Parse trailing segment like `providers.replicate.models.{collection}`.
- [x] `resolve_provider_options_schema()` route: `path_starts_with(schema_path, "mcp.tools")`
  → `mcp_tool_options_schema(server, tool)`:
  - [x] Pull `servers[server].tools[tool].inputSchema` from the cached probe.
  - [x] Emit `{block, provider:server, model:tool, value_field:"arguments", schema:{type,properties,required}, ui:{}}`.
  - [x] `annotate_schema_param_kinds(schema_props)` for native widgets.
  - [x] Null schema when tool has no `inputSchema`.
- [x] Leave MCP paths out of `bootstrap_payload()` (lazy fetch via TanStack).
- [x] Harness: `mcp-ui` subgroup (`xblox info options` / `xblox info schema`).

---

## Phase 3 — Refactor CSettingsWebView onto shared module ✓

- [x] `settingsMcpToolCall` (~`CSettingsWebView.cpp:2569`): replace inline mcp.json
  parsing + client construction with `mcp_config::load_mcp_config` +
  `create_mcp_session`.
  - [x] Fixes: currently reads **plain** `mcp.json` only (ignores secure storage) — shared loader fixes parity.
  - [ ] Fixes: currently skips sandbox launch options + policy checks (settings test path; block enforces policy).
- [x] Leave `settingsMcpGet` / `settingsMcpSave` / `settingsMcpPing` as-is (ping already delegates to `probe_mcp_config`).
- [x] Refactor `mcp_probe.cpp` + `McpBridge.cpp` onto the shared factory.

---

## Phase 4 — Security / policy parity

- [x] Block must call `mcp_server_policy_allows()` + honor `requireRegisteredMcp`
  (like `McpBridge.cpp`), not the slim settings path.
- [ ] Gate on policy: reuse `Agent.EnableMcpClient` or add a dedicated
  `Xblox.EnableMcp` catalog entry (`policy_catalog.cpp`). [?] decide.
- [ ] Respect `AllowedMcpHosts` restriction (exists in `policy_catalog.cpp`).
- [ ] Never persist/echo secret args or headers in events (reuse masking helpers from `network_blocks.cpp` if arguments can carry tokens). [?]

### Auth mechanisms — inventory + what to stub

Auth is **not just router-vs-literal-headers**. The existing surfaces suggest
several mechanisms users may eventually need per MCP server. v1 implements the
first two; the rest are **stubbed** (enum value + no-op/`not implemented` note)
so the config shape and UI don't need reworking later.

| Mechanism | Source of truth | v1 |
|-----------|-----------------|----|
| None (local stdio) | — | ✓ implement |
| Literal `headers` | `mcp.json` `headers` (existing) | ✓ implement |
| Router auth (pixlwiz/ZITADEL) | URL-match `resolve_llm_router_base()` → `pm_zitadel_oauth_read_access_token` | ✓ implement (Phase 8 Tier 2) |
| Provider OAuth | `provider_oauth.hpp` — **openrouter / openai / github** (`read_access_token`, device-code for github) | ⛔ stub |
| Explicit bearer / API-key field | new config field, header injection | ⛔ stub |
| `auth_mode` selector (auto/api_key/oauth) | mirrors `ProviderEntry.auth_mode` (`settings_types.hpp:14`) | ⛔ stub |

- [ ] Reserve an optional `auth` object on the MCP server config (or a block
  param) with a discriminated `mode`: `none | headers | router | oauth |
  apiKey`. v1 handles `none`/`headers`/`router` (router auto-detected); the
  others parse-and-warn `"auth mode '<x>' not implemented yet"`.
- [ ] Do **not** invent a new OAuth flow here — if wired later, route through
  `media::provider_oauth::read_access_token(provider_id, …)` so xBlox reuses the
  same tokens the chat/agent already manage. No token acquisition inside xBlox.
- [ ] Keep the stubs out of the happy path: unknown/stubbed modes must not break
  a server that also has working literal `headers`.

---

## Phase 5 — Caching & performance ✓ (disk layer; UI wiring next)

The disk cache from Phase 0 (`mcp-tools-cache.json`, 8h TTL) is the single source
for UI option/schema lookups — no second cache. Three layers stack:

1. TanStack (web): `xbloxUiOptionsGet` 30 s, `providerSchema` 5 min (already there).
2. Disk probe cache (C++): 8h TTL, `mcp.json`-mtime invalidated (reused, not rebuilt). ✓
3. Live handshake: only when disk cache stale/missing **and** probe allowed. ✓

- [x] UI resolvers (`mcp.servers`, `mcp.tools.*`, `mcp.tools` schema) call
  `mcp_probe_cached(allow_probe=true)` — reuse, don't re-probe.
- [ ] [?] Should the block-panel path be allowed to trigger a live probe, or only
  read an existing cache? Live probe on panel open can spawn stdio children.
  Consider `allow_probe=false` for hot UI, with an explicit "refresh tools" action.
- [x] Runtime block execution: fresh session per call (simple, matches
  `settingsMcpToolCall`). [?] Connection pooling deferred.
- [ ] Cache is keyed on `mcp.json` mtime — confirm this works with **secure
  storage** (`pm://config/mcp.json` has no plain-file mtime). The current cache
  assumes a plain `mcp.json` path; under `FEATURE_SECURE_STORAGE` the mtime is 0
  and invalidation degrades to pure 8h TTL. Document or fix (hash the decrypted
  config instead of mtime).
- [ ] Document per-call handshake cost (stdio spawn) in the block description.

---

## Phase 6 — Result normalization

- [?] MCP `tools/call` returns content blocks (`content:[{type:text|image|...}]`).
  Decide storage:
  - Option A: store raw MCP result JSON (matches agent bridge).
  - Option B: normalize — concatenate text content into a string for `PREVIOUS`,
    keep raw under `storeAs`.
- [ ] Whatever chosen, document in block desc + `docs/xblox/xblox.md`.
- [ ] Handle `isError:true` in result → surface as block error.

---

## Phase 7 — MCP client introspection CLI

Gap: `pm_image_mcp_embed.cpp` only had the **server** `mcp` subcommand (embedded
HTTP exposing *our* path-tools). There is now a **client** branch to inspect external
MCP servers from `mcp.json`. `llm info --json` still surfaces a cached probe
(`mcp_probe_cached`). The tests + the block both benefit from this first-class
client introspection surface.

- [x] Add `tanit-cli mcp client <verb>` (`pm_image_mcp_client.cpp`, gated `FEATURE_MCP_CLIENT`):
  - [x] `mcp client list` — server names + transport + enabled from `mcp.json`
    (reuse `mcp_config`/`probe_mcp_config`).
  - [x] `mcp client tools --server <name>` — `tools/list` (name + description).
  - [x] `mcp client schema --server <name> --tool <name>` — the tool's `inputSchema`.
  - [x] `mcp client call --server <name> --tool <name> --args '<json>'` — `tools/call`.
  - [x] `--json` on each for the test harness; `--no-probe`/`--probe` to control live handshakes.
- [x] These are the primitives the orchestrator test (Phase 8) asserts against,
  and mirror the block's own resolution path (shared `mcp_config`).

---

## Phase 8 — Tests (3 tiers) ✓ Tiers 1–2

Driver + harness already exist in `tests/orchestrator/test-xblox-network.mjs`
(`run(doc)` writes a temp xBlox doc, runs `xblox run --json --no-wait`, parses
trailing JSON `events`). Add MCP suites there. Server fixtures live in
`tests/xblox/xblox-mcp-tests.json` (Cursor-shape `mcpServers`).

### Tier 1 — local, no auth (fast, CI-safe)

Fixture: `code-mcp` (`codebase-memory-mcp.exe`, stdio, no headers/env) — good
local pick. Fallback: `@modelcontextprotocol/server-memory` via `npx` if the
exe isn't present.

- [x] `suiteMcp()` in `test-xblox-network.mjs`, gated on the server being present
  (skip with a clear message if the stdio binary/`npx` is unavailable — keep CI green).
- [x] Isolation (decided): pass the existing global `--config-dir <fixtureDir>`
  (parsed in `pm_image_run.cpp:503` → `set_config_dir_override()`; honored by
  `get_config_dir()` and thus `mcp_json_path()` / `probe_mcp_config`). Fixture
  dir holds its own plain `mcp.json` at `tests/xblox/mcp.json`. No new knob.
  - [x] Plain `mcp.json` fallback when secure document is absent (`load_mcp_config`).
  - [x] Stdio `command` resolves via `exe_bin_directory()` sidecars (`resolve_third_party_executable`).
  - [x] Keep a separate **real** run with **no** `--config-dir` override, so it
    exercises the user's default env + their own MCP catalog (per request #2).
    Gated `PM_MCP_LIVE=1` → `mcp-live` subgroup.
- [x] Manifest assertions (offline, no handshake): `xblox info --json` lists
  `mcpCall` with params `server`, `tool`, `arguments`, `timeoutMs`, `storeAs`.
- [x] Introspection assertions via Phase 7 CLI + `mcp-ui` (`xblox info options` / `schema`).
- [x] Block execution: an inline `mcpCall` doc (index/search or office view/query) → assert ok
  event + `result` in `storeAs`; assert `isError` path surfaces a block error.
- [x] `mcp-office` subgroup: `officecli` sidecar (`officecli mcp` stdio) against `tests/office`
  fixtures (`view` stats + `query` sheets + CLI `get`).
- [x] Simulate mode: `--simulate` skips the side-effecting call (per Phase 6 [?]).

### Tier 2 — service/auth (our MCP server via pixlwiz router)

Fixture: user's roaming `mcp.json` `pixlwiz` server (`url: https://llm.polymech.info/mcp`)
— exposes tavily/deepl/etc. tools. **Tier 2 does not use `--config-dir`**; it runs
against the same profile as `tanit-cli login` so `zitadel-oauth.json` resolves.

- [x] Verify the block's session auth reuses the **same ZITADEL OAuth path as
  `login` / `service_blocks`**: `pm_zitadel_oauth_read_access_token` (honours
  `api_bearer_source` id_token vs access_token from `zitadel-oauth.json`).
- [x] Design (decided): when an MCP server **is our own router**, inject the
  auth header from the logged-in bearer — no hardcoded `x-litellm-api-key` and
  no config flag. Detection is **URL-based**:
  - [x] In `create_mcp_session`, compare the server `url` against
    `pm::services::resolve_llm_router_base()` (`src/service_defaults.hpp`). Our
    MCP endpoint lives under that base (e.g. `<router-base>/mcp`).
  - [x] On match, set `Authorization: Bearer <token>` via
    `pm_zitadel_oauth_read_access_token` (same token `login` persists to
    `zitadel-oauth.json`).
  - [x] Non-matching servers keep their literal `headers` untouched (Tier 1 has none).
  - [x] Effect: roaming `pixlwiz` can drop its hardcoded `x-litellm-api-key` header
    and still authenticate via injected `Authorization`.
  - Note: router auth is the one credentialed mechanism v1 ships. Provider-OAuth
    (openrouter/openai/github) and an explicit api-key/`auth_mode` selector are
    **stubbed** — see Phase 4 "Auth mechanisms" inventory.
- [x] Gated test (needs live token/network + roaming `pixlwiz` in mcp.json): skip
  unless ZITADEL login is available (`status --json` → `pixlwiz.logged_in`). No
  `--config-dir` override. Assert:
  - [x] `mcp client tools --server pixlwiz --json` returns the remote catalog (auth OK).
  - [x] An `mcpCall` to a cheap tool (e.g. a search) returns a result.
  - [x] With no/invalid credential → clean auth-failure error (not a crash).

### Tier 3 — real e2e xBlox file ✓ (local + live)

Committed fixtures under `tests/xblox/` (deepl-style: context vars, stdout
banner, `mcpCall` chain, `storeAs` + `getVariable`, completion log).

- [x] `network-mcp-local.xblox` — `code-mcp` index → search via
  `--config-dir tests/xblox`; CLI context overrides `--repoPath`, `--project`,
  `--query` (same paths as inline `mcp/block`).
- [x] `network-mcp-live.xblox` — roaming `pixlwiz` router call; no
  `--config-dir`; gated like `mcp-router` (`--tool` from probe).
- [x] Harness: `mcp-xblox-local`, `mcp-xblox-live` in `test-xblox-network.mjs`.
- [ ] Document the local doc as the canonical "hello MCP" example in `docs/xblox/xblox.md`.
- [ ] **Next:** Phase 2c property panel (see above).



---

## Phase 9 — Docs

- [ ] `xblox info schema --schema-path mcp.tools --provider <server> --model <tool>`
  smoke (route already flows through `resolve_provider_options_schema`;
  `pm_image_cmd_xblox.cpp:304`).
- [ ] `xblox info options --path mcp.servers` / `--path mcp.tools.<server>`.
- [ ] Update `docs/xblox/ui-schema.md` — add "Implemented: MCP tools" next to Whisper/Replicate.
- [ ] Update `docs/xblox/xblox.md` — document the `mcpCall` block + the e2e sample.
- [ ] Document the new `tanit-cli mcp client` verbs in `docs/public/v1.0/cli.md`.

---

## Open decisions (collect answers before coding)

- [x] `tool` dropdown → **decided:** `options_path_template` in `ParamOptionsSelect`
  (Phase 2b/2c); free-text remains fallback when path unresolved.
- [?] Block kind name: `mcpCall` vs `mcp` vs `mcpTool`.
- [?] `schema_path` namespace: `mcp.tools` vs `providers.mcp` (consistency vs clarity).
- [?] Iterable producer (fan out array results like `fetch`)?
- [?] Result normalization (Phase 6).
- [?] Dedicated policy knob vs reuse `Agent.EnableMcpClient`.
- [?] Simulate mode: skip the call (side-effecting) or allow (read-only tools)? → **decided: skip** (implemented + tested).
- [x] Cache extraction scope: moved cache into `mcp_config`; `llm_info_compact` forwards `mcp_probe_cached()`.
- [?] Secure-storage cache invalidation (mtime is 0 for `pm://config/mcp.json`) —
  hash-based invalidation vs pure TTL.
- [x] Router auth marker → **decided: URL-based.** `create_mcp_session` compares
  the server `url` to `pm::services::resolve_llm_router_base()`; on match, sets
  `Authorization: Bearer …` via `pm_zitadel_oauth_read_access_token`. No config flag.
- [?] Auth mechanism scope: v1 ships `none`/`headers`/`router`; do we reserve the
  `auth.mode` enum now (recommended, cheap) and stub `oauth`/`apiKey`, or defer
  the whole config shape? Provider OAuth (openrouter/openai/github) already exists
  via `provider_oauth.hpp` if/when wired. See Phase 4 inventory.
- [x] Test config isolation → **decided: reuse global `--config-dir`.** Fixture
  dir carries its own plain `mcp.json`; router/auth tests run with **no** override
  against the user's roaming profile (`zitadel-oauth.json` + `mcp.json`). Only open
  sub-question: secure-storage fixture seeding for CI (build-off vs `mcp-store import-plain`).
- [x] Simulate mode: `--simulate` skips the side-effecting call (Tier 1 `mcp/block/simulate` green).

---

## Key files

| Role | Path |
|------|------|
| MCP session interface | `src/llm/mcp_session.hpp` |
| stdio transport | `src/llm/mcp_stdio_client.{hpp,cpp}` |
| HTTP transport | `src/llm/mcp_stream_client.{hpp,cpp}` |
| Config probe (reference) | `src/llm/mcp_probe.cpp` |
| Tools cache (extract) | `src/llm/llm_info_compact.{cpp,hpp}` (`mcp-tools-cache.json`, `mcp_probe_cached`, `mcp_tool_inventory`) |
| Agent bridge (reference) | `src/llm/tools/mcp/McpBridge.cpp` |
| Settings tool call (refactor) | `src/win/ui_next/CSettingsWebView.cpp` (~2569) |
| MCP server CLI (add client verb) | `src/cli/pm_image_mcp_embed.cpp` (`pm_image_register_mcp`, ~362) |
| ZITADEL bearer (router auth) | `src/lib/pm_zitadel_oauth.hpp` (`pm_zitadel_oauth_read_access_token`) |
| Router base (URL-match) | `src/service_defaults.hpp` (`resolve_llm_router_base`) |
| Provider OAuth (stub target) | `src/lib/provider_oauth.hpp` (openrouter/openai/github), `ProviderEntry.auth_mode` `src/core/settings_types.hpp:14` |
| config-dir override | `src/cli/pm_image_run.cpp:503`, `src/core/settings_store.hpp` (`set_config_dir_override`) |
| Test harness | `tests/orchestrator/test-xblox-network.mjs` |
| Test server fixtures | `tests/xblox/xblox-mcp-tests.json` |
| Shared MCP config | `src/llm/mcp_config.{hpp,cpp}` |
| Template resolver (share) | `src/xblox/blocks/json_templates.{hpp,cpp}` |
| `${var}` token parser | `src/string_utils.{hpp,cpp}` (`substitute_variables`) |
| MCP block | `src/xblox/blocks/network_blocks_mcp.cpp` |
| Header/var reference doc | `tests/xblox/network-deepl.xblox` (`headers` + `${USER:DEEPL_KEY}`) |
| Block pattern | `src/xblox/blocks/data_blocks.cpp` |
| Network blocks (sibling) | `src/xblox/blocks/network_blocks.cpp` |
| Dynamic schema resolver | `src/xblox/blocks/block_params_ui.cpp` |
| Dynamic schema reference | `src/xblox/blocks/image_blocks.cpp` (`ai_image_params`) |
| Param builder | `src/xblox/blocks/block_params.hpp` (`.dyn()`, `.ui()`) |
| UI RPC gateway | `src/win/ui_next/CBlockView.cpp` (`xbloxUiOptionsGet`, `xbloxProviderOptionsSchemaGet`) |
| Property panel (Phase 2b/2c) | `apps/xblox/src/xblox/react/xblox-property-panel.tsx` (`selectOptionsForParam` L408, `ParamOptionsSelect` L688, `DynamicProviderOptionsGroup` L782) |
| Settings MCP server fields (ref) | `apps/shared/components/settings/McpSettingsPanel.tsx`, `apps/shared/settings/mcp.ts` |
| Docs | `docs/xblox/ui-schema.md`, `docs/xblox/xblox.md` |
| Build | `CMakeLists.txt` (`FEATURE_MCP_CLIENT`, ~1751 / ~1792) |

## Examples

npm run test:xblox-network -- --only mcp-xblox-local   # 9 passed
npm run test:xblox-network -- --only mcp-xblox-live    # 6 passed (needs login + clean roaming mcp.json)

tanit-cli xblox run --src tests/xblox/network-mcp-live.xblox --tool deepl-get-source-languages

