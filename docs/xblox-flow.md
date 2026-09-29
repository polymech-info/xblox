# XBlox Variable-Plumbing View — Agent Handoff

## Goal

Add a fourth XBlox toolbar view that lays blocks out vertically and exposes
connectable input/output ports. This view edits data plumbing only. Document
order and child lists remain the authoritative execution/control flow.

Use `@xyflow/react` (already installed in `apps/xblox`). Start with a read-only
projection, then add binding edits after the native contract and document-v2
schema are stable.

## Decisions already made

- Every runtime value `ParamDef` is bindable by default (`accepted_bindings = kAllParamBindings`).
- Declared output slots and output-name fields such as `storeAs` opt out (`.out()` → `binding: false` in manifest).
- Bindings are stored under `block.bindings[targetParam]`.
- Supported binding kinds: `blockOutput`, `scopeVariable`, `previous`, and
  `parameterOperation` (metadata only; evaluator dispatch deferred).
- Bindings never change execution order. Unavailable producer references fail at
  resolve time with explicit diagnostics (`unavailable_binding_source`, …).
- Stable output slot `result` is the conventional primary output.
- `storeAs` is an optional scope export of `result`; it is not the slot name.
- Binding resolution and input preprocessing belong in the executor
  (`resolve_block_inputs` in `builtin_blocks.cpp`, binding stage in
  `block_resolver.hpp`). Block algorithms must not contain `has_binding` or
  equivalent branches.
- Resolver values are materialized into a handler-local `BlockIO` frame only
  when needed, preserving legacy raw-field handlers without injecting omitted
  literal defaults.
- Binding-sourced values are **already final typed data** and must not be
  re-run through expression evaluation or `${var}` interpolation.
- Parameter operations are inline, pure edge transforms—not executable blocks.
  Metadata/registry/parsing are stubbed; evaluator dispatch and UI are deferred.
- Initial graph layout is deterministic top-to-bottom document order. Do not
  persist graph positions or add Dagre yet.

## Bindings & output contract (handoff reference)

### Document v2 shape

Bindings live on the **consumer** block. Producers are referenced by stable
`blockId`, not tree path.

```json
{
  "version": 2,
  "context": { "scopedMessage": "from-scope" },
  "roots": [
    {
      "blockId": "saveState-a1b2c3",
      "kind": "saveState",
      "scope": "context",
      "mode": "existing",
      "keys": ["count"]
    },
    {
      "blockId": "log-x9y8z1",
      "kind": "log",
      "level": "info",
      "bindings": {
        "message": {
          "kind": "blockOutput",
          "blockId": "saveState-a1b2c3",
          "output": "result"
        }
      }
    },
    {
      "blockId": "log-scope01",
      "kind": "log",
      "level": "info",
      "bindings": {
        "message": { "kind": "scopeVariable", "name": "scopedMessage" }
      }
    }
  ]
}
```

| binding kind | JSON fields | resolves to |
|---|---|---|
| `blockOutput` | `blockId`, `output` (slot, default `result`) | `OutputFrameStore` entry for that producer |
| `scopeVariable` | `name` | named context variable at resolve time |
| `previous` | _(none)_ | `PREVIOUS` pipeline value |
| `parameterOperation` | operation tree | **rejected** today (`unsupported_parameter_operation`) |

Explicit `previous` binding and implicit PREVIOUS sourcing (`.prev()` params) are
both supported; the wiring view renders implicit PREVIOUS as dashed compatibility
edges.

### Declaring outputs in C++ (`BlockDescriptorBuilder`)

Use the typed builder in `src/xblox/blocks/block_registry.hpp`. Audit handler
emissions while adding declarations—do not infer slots from property-panel groups.

**Primary output** — always slot `result`, marks `primary: true`, sets
`PreviousBehavior::Writes`:

```cpp
registry["saveState"] = bdb("saveState", save_state_block, "Save State", "Context", …)
    .params({ … })
    .primary_output(ParamKind::json_value, "Persisted state")
    .exec({false, true, true});
```

Same pattern for context blocks:

```cpp
.primary_output(ParamKind::json_value, "Value");          // setVariable / getVariable
.primary_output(ParamKind::json_value, "Run result");    // xbloxRun
.primary_output(ParamKind::boolean_, "Exists");           // fsExists
.primary_output(ParamKind::string_, "Answer");           // llm blocks
```

**Dynamic primary** — runtime kind varies; manifest advertises `supportedKinds`:

```cpp
.dynamic_primary_output(
    ParamKind::json_value,                      // fallback kind
    { ParamKind::string_, ParamKind::json_value }, // allowed kinds
    "Response body",
    /*conditional=*/false);
```

Use for `fsRead`, `shell`, `fetch`, stdin-driven blocks, etc.

**Named secondary slots** — additional bindable outputs; optional scope export:

```cpp
.output("entries", ParamKind::json_value, "Directory entries")
.output("statusCode", ParamKind::integer, "HTTP status", "httpStatus"); // exportName
.output_from_param("key", ParamKind::shortcut_, "Key", "storeAs", ".key");
```

`output_from_param` keeps the output slot stable while describing a scope export
whose name is derived from a block parameter (for example `storeAs=event` writes
the `key` slot to `event.key`).

Rules:

- Exactly one primary slot; primary slot ID must be `result`.
- Do not advertise event metadata (`path`, `duration`, …) as bindable outputs.
- `preserves_previous()` / `clears_previous()` for flow blocks that do not publish
  a primary result (`if`, `while`, `command`, …).
- Distinguish **declared outputs** from **internal PREVIOUS seeding** (e.g. MQTT
  message-loop child fan-out): only declare what downstream bindings may target.

### Param binding defaults (`ParamDef`)

Defined in `src/xblox/blocks/block_params.hpp`:

| builder | effect |
|---|---|
| _(default)_ | all binding kinds accepted; `bindingSyntax: value` |
| `.out()` | `binding: false` — used for `storeAs`, `target`, output-name fields |
| `.prev()` | adds `previous` + `blockOutput`; sets `ParamSource::Previous` |
| `.from_var("name")` | adds `scopeVariable` + `blockOutput` |
| `.literal_only()` / `.bindings(None)` | no bindings |
| `.bindable()` | restore full accept set |

Manifest v2 omits per-param binding defaults on every field; top-level
`bindingDefaults` declares `{ accepts: ["blockOutput", …] }` once. Opt-outs
serialize as `"binding": false` on individual params.

### Runtime resolution (executor)

Per interpreted builtin invocation:

1. **`resolve_param_binding`** — read `block.bindings[param]`; validate kind vs
   `accepted_bindings`; fetch value from output frame / scope / PREVIOUS.
2. **`source_param`** — if no binding: explicit field → declared source → default.
3. **`transform_param`** — kind cast/coercion.
4. **Resolve features** — `${var}` / globs / custom hooks (skipped for binding values).
5. **Expression syntax** — only for explicit document literals, not binding payloads.
6. **`validate_constraints`** — severity ≥2 skips handler with diagnostic.

Output frames (`OutputFrameStore` in `xblox_commands.cpp`):

- Keyed by **`blockId` + slot** (not path).
- Cleared at block entry to avoid stale loop/branch values.
- Populated by `EventBuilder::result()` / `bridge_publish_output()`.
- Legacy PREVIOUS writes auto-publish a primary `result` frame when the handler
  has not already done so.

Run events (when `--json` / host collection is on):

- `event.blockId` — stable graph identity.
- `event.outputs[]` — `{ slot, kind, value, primary?, exportName? }`.
- Semantic fields like command `id` stay in `event.data`; do not conflate with `blockId`.

**Perf note:** any non-empty `bindings` tree disables the compiled fast path for
the document pass (`json_tree_has_bindings` in `PassEngine::prepare`). See
`tests/xblox/performance.md` for bench gates and the open `collect_events`
regression.

### UI persistence

Connect in wiring view writes:

```ts
bindings[param] = { kind: "blockOutput", blockId: "<producer-blockId>", output: "result" }
```

via `mutateBindings.ts` → builder `commitChange` (undo/redo intact). Producer
lookup in projection uses `blockId`; paths remain selection/routing only.

## Native work already implemented

- Manifest v2 capabilities and compact top-level binding defaults.
- `OutputDef`, `PreviousBehavior`, dynamic/conditional output metadata, and
  supported output kinds.
- Stable-slot runtime event outputs (`slot`, optional `exportName`, `kind`,
  `value`, `primary`).
- Per-run output frames keyed by block ID and output slot.
- Binding parsing/resolution with explicit diagnostics.
- Required-parameter validation.
- Document-v2 stable/unique block ID validation.
- Output-frame clearing to avoid stale loop/branch values.
- Legacy PREVIOUS writes publish a primary result frame automatically.
- Parameter-operation descriptor and empty registry in
  `src/xblox/blocks/parameter_operations.hpp`.
- Parameter-operation manifest transport to CLI and native web host.
- Dedicated `wiring` suite in `tests/orchestrator/test-xblox-next.mjs` and
  `scripts/xblox-tests.sh`.
- CMake includes the new parameter-operation header.

Output descriptors have been added for:

- core/context/flow builtins
- filesystem
- data/parse/iterator
- network/fetch/SSH/IPC/MCP
- shell/openPath/xbloxRun
- app/picker
- image blocks
- LLM blocks

Dynamic metadata corrections are already applied to `stdin` and `fsRead`;
`saveState` declares `.primary_output(ParamKind::json_value, "Persisted state")`.
Core context builtins use the same pattern:

```cpp
.primary_output(ParamKind::json_value, "Value");           // setVariable, getVariable
.primary_output(ParamKind::json_value, "Persisted state"); // saveState
```

See **Bindings & output contract** above for the full descriptor API
(`dynamic_primary_output`, named `.output(...)`, `preserves_previous()`).

## Verified status

Native build and focused suites green on the wiring contract work.

Passing focused suites (representative):

- `wiring`: manifest v2, runtime bindings, v1 backfill, nested `for` + `blockOutput`,
  fixture `wiring-block-output.xblox`, command `id` vs `blockId` separation
- `variables`, `expressions`, `loops`, `previous`, `refs`, `containers`, `scoping`
- `xbloxRun`: functional lifecycle (not loop-FPS; see `tests/xblox/performance.md`)

The manifest exceeds Node's default 1 MiB `spawnSync` buffer, so `xbloxInfo()` in
the harness uses a 16 MiB buffer. Per-param default binding metadata is omitted
from the payload; `bindingDefaults` declares the default once, while opt-outs
serialize as `binding: false`.

Regression gates:

```bash
npm run test:xblox-next -- --only wiring
bash scripts/xblox-tests.sh          # wiring + core slices
npm run bench:xblox -- --append      # throughput (separate from wiring correctness)
```

## P0 — finish the native contract before GUI work

### Complete output descriptor coverage

Audit the actual handler emissions while adding declarations. Do not infer
outputs from presentation groups alone. Remaining GUI-visible registration
files:

- [x] `input_blocks.cpp`
  - `keyEvent`: primary JSON plus stable typed `key`, `down`, `pressed`,
    `released`, and `toggle` slots; dynamic `storeAs.*` names remain scope
    exports rather than output-slot identities.
  - `keyWait`: primary shortcut.
- [x] `audio_blocks.cpp`
  - device list/play/record/session controls declare their fixed primary kinds.
  - transcribe declares string/JSON and speak declares string/audio-path dynamic
    primary kinds; event-only diagnostics remain non-bindable metadata.
- [x] `video_blocks.cpp`, `video_core_blocks.cpp`,
  `video_detect_blocks.cpp`, `video_blocks_streaming.cpp`
  - capture/source/stream/screens/detect primary contracts are declared,
    including conditional lifecycle results and `videoSourceFps`.
  - per-frame `prime_previous` child seeding remains internal.
- [x] `video_filter_blocks.cpp`
  - grayscale/color/crop/resize and `pictureOut` declare image results;
    `videoWriter` declares a video result.
- [x] `vision_blocks.cpp`
  - ask/describe declare their dynamic JSON/string primary result. Structured
    event payload fields remain diagnostics, not additional bindable outputs.
- [x] `ocr_blocks.cpp`
  - dynamic JSON/string primary plus conditional typed documents/timing/GPU
    outputs; untyped payload fields remain metadata.
- [x] `model_blocks.cpp`
  - model control and group aliases declare dynamic JSON/string list/unload
    results plus conditional typed `models`/`unloaded` scope exports.
- [x] `bluetooth_blocks.cpp`
  - list/endpoints and mutation result envelopes declare JSON primaries.
- [x] `modbus_blocks.cpp`
  - server/connection/read/write aliases declare JSON primaries.
- [x] `mqtt_blocks.cpp`
  - server/client status results are declared; message-loop PREVIOUS remains
    internal seeding.
- [x] `service_blocks.cpp`
  - primary responses plus actually typed status/count/total/size outputs;
    untyped failure summaries remain event metadata.
- [x] `vector_blocks_local.cpp`
  - primary JSON plus typed counts/dimensions/GPU/index-size outputs.
- [x] `vector_blocks.cpp`
  - disabled registrations remain disabled, with aligned primary and typed
    secondary contracts if retained.

For dynamic primaries use `dynamic_primary_output(fallback, supportedKinds,
label, conditional)`. Use `output(slot, kind, label, exportName, conditional)`
for named slots. Un-typed event metadata must not be advertised as a bindable
output.

### Add registry validation after coverage

- [x] Reject empty or duplicate output slots.
- [x] Reject multiple primary outputs.
- [x] Require primary slot ID `result`.
- [x] Validate `writesPrevious` against block-level `PreviousBehavior`.
- [x] Validate fixed export names and duplicate exports.
- [x] Ensure every GUI-visible descriptor explicitly declares either outputs or
  `preserves_previous()`.
- [x] Add deterministic manifest assertions for dynamic/conditional outputs.
- [x] Add representative runtime assertions that emitted slots match declared
  slots.

### Native regression gate

- [ ] Run `npm run build:cpp`.
- [ ] Run `npm run test:xblox-next -- --only wiring`.
- [ ] Re-run at minimum: `engine`, `variables`, `expressions`, `loops`,
  `previous`, `containers`, `scoping`, `transform`, `simulate`, `constraints`,
  `iterate`, and `refs`.
- [ ] Run hardware/network suites only where their prerequisites exist.
- [ ] Do not weaken legacy assertions to make the new resolver pass.

## P1 — TypeScript contract and projection

Most of this slice is shipped; remaining work is polish and hardening.

- [x] Update `apps/shared/xblox/manifest.ts` — output descriptors, binding kinds,
  manifest v2 / `bindingDefaults`, `previousBehavior`.
- [x] Extend `NativePaletteBlock` / `nativeBlocksFromPayload()` for outputs and
  manifest-v2 fields.
- [x] Wiring module: `types.ts`, `projectDocument.ts`, `toXYFlow.ts`,
  `mutateBindings.ts`, `scopeVariables.ts`.
- [x] Deterministic port IDs; project explicit bindings + dashed PREVIOUS edges.
- [x] Resolve producers by `blockId`; paths for selection/property panel only.
- [ ] Unit tests for projection/diagnostics independent of React Flow mount.
- [ ] Kind-compatibility checks before connect mutation (manifest `ParamKind` vs
  output slot kind).
- [ ] Surface projection diagnostics in the wiring canvas (missing producer, type
  mismatch) beyond console/dev-only paths.

## P2 — wiring GUI

Read-only and editable binding slices are largely shipped.

- [x] `"wiring"` preview mode, toolbar pill, feature flag wiring.
- [x] `WiringFlowPreview.tsx`, `WiringBlockNode.tsx`, scope-variable nodes,
  `@xyflow/react` CSS, dark/light styles.
- [x] Vertical document-order layout; input left / output right handles.
- [x] Connect/disconnect persists `bindings` via `commitChange` (undo/redo).
- [x] Node click → `selectedPath`; execution badges from `runState`.
- [ ] Scope-variable and explicit-`previous` source picker UX (beyond connect drag).
- [ ] Parameter-operation pseudo-node rendering (blocked on evaluator dispatch).
- [ ] Reject future/unavailable producer references at connect time with inline
  diagnostic (runtime already fails resolve).

## P3 — editable direct bindings / stable identity

Stable identity contract is shipped; finish test coverage and edge UX.

- [x] Use `blockId` for graph/runtime identity. Do not reuse `id`; command and
  runScript blocks already use `id` for command semantics.
- [x] Generate glob-friendly IDs as `<kind>-<short-random>`, for example
  `setVariable-k7m2p9`.
- [x] Keep position as the ordered-array path used by selection/tree UI. Never
  encode mutable position into stable identity.
- [x] Treat all block IDs as one document-wide namespace, including nested
  blocks and switch items.
- [x] Preserve valid unique IDs during load.
- [x] Backfill missing IDs through a central document normalizer.
- [x] Build a legacy path → new ID map during backfill and rewrite v1
  `bindings.*.blockId` path references before any tree mutation.
- [x] Detect duplicate IDs from manual edits/imports. Keep the first occurrence,
  regenerate later occurrences, and rewrite references when their source is
  unambiguous; otherwise emit a migration diagnostic.
- [x] Assign IDs at the editor's centralized insertion/change boundary.
- [x] Regenerate IDs when cloning/pasting/importing a subtree and rewrite all
  bindings internal to that subtree.
- [x] Preserve IDs during reorder/move/undo/redo.
- [x] Update C++ v2 validation/output frames/events to read `blockId`, preserving
  semantic `id` behavior.
- [x] Update wiring projection to resolve producer `blockId` to a current path;
  continue using paths for React Flow node selection and property-panel routing.
- [x] Harness coverage: v1 backfill, nested `for` + `blockOutput`, missing producer,
  command `id` vs `blockId`, fixture `tests/xblox/wiring-block-output.xblox`.
- [ ] Editor-side tests: duplicate-ID import, clone/reorder binding rewrite,
  legacy path migration edge cases (beyond harness).

JSON arrays (`roots`, `items`, `consequent`, etc.) are ordered by the JSON
standard and by JavaScript/C++ parsers. Object member order must not drive
execution or identity.

- [x] Extend `apps/xblox/src/schema/blocks-file.ts` — v2 `blockId`, `bindings` map.
- [x] Add migration/normalization for v1 documents (`normalize-blocks-file.ts`).
- [x] Connect/disconnect binding mutations (`mutateBindings.ts` + `commitChange`).
- [ ] Binding variants for parameter-operation expressions (when evaluator exists).
- [ ] Keep invalid documents editable with inline diagnostics (partially via projection).

## Important integration points

| area | path |
|---|---|
| Preview / save / normalize | `apps/xblox/src/PrototypeApp.tsx` |
| Tree + wiring slot | `apps/xblox/src/xblox/react/BlockTreeView.tsx` |
| Toolbar | `apps/xblox/src/xblox/react/BlockTreeToolbar.tsx` |
| Builder undo/save | `apps/xblox/src/xblox/react/xblox-builder.tsx` |
| Document schema | `apps/xblox/src/schema/blocks-file.ts` |
| ID migration | `apps/xblox/src/schema/normalize-blocks-file.ts` |
| Manifest types | `apps/shared/xblox/manifest.ts` |
| Wiring projection | `apps/xblox/src/xblox/wiring/projectDocument.ts` |
| Binding edits | `apps/xblox/src/xblox/wiring/mutateBindings.ts` |
| Wiring canvas | `apps/xblox/src/xblox/wiring/WiringFlowPreview.tsx` |
| Output / builder API | `src/xblox/blocks/block_registry.hpp` |
| Param + binding schema | `src/xblox/blocks/block_params.hpp` |
| Binding resolver | `src/xblox/blocks/block_resolver.hpp` |
| Input pipeline | `src/xblox/blocks/builtin_blocks.cpp` (`resolve_block_inputs`) |
| Executor / output frames | `src/xblox/xblox_commands.cpp` |
| Wiring harness | `tests/orchestrator/test-xblox-next.mjs` (`--only wiring`) |
| Perf / bench | `tests/xblox/performance.md`, `tests/xblox/bench.mjs` |
| Shipped summary | `docs/xblox/xblox-dev.md` |

## Guardrails

- Do not add binding-specific conditions to block handlers.
- Do not make edge connections reorder blocks.
- Do not treat event metadata as bindable output slots.
- Do not infer connectability from property-panel `group`.
- Do not persist React Flow coordinates in the first slice.
- Do not implement parameter-operation execution or UI in this handoff.
- Do not remove PREVIOUS compatibility until migration policy is explicit.
- Follow `AGENTS.md`: do not use git diffs; build only when requested/needed by
  the active task.
