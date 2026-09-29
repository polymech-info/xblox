# XBlox Language Blocks — control flow design

Status: **implemented** (Phases 0–2; Phase 3 UI harness pending). The sibling
chain is the *only* shape — the legacy nested `elseIfBlocks`/`alternate` form
was removed without back-compat.
Test harness: `tests/orchestrator/test-xblox-language.mjs` (`npm run test:xblox-language`).

## Scope

`ref/xblox/enums.ts` (esprima AST node types) is the upper bound of what a
"real" language would need. We deliberately implement only the basics:

| Implemented (target)            | Explicitly out of scope (for now)          |
| ------------------------------- | ------------------------------------------ |
| `if` / `elseIf` / `else`        | `try`/`catch`/`throw`, labels, `continue`  |
| `switch` / `case` / `default`   | `do…while`, `for…in`, functions/closures   |
| `while` / `for` / `break`       | `with`, sequence/comma expressions         |

## Current state (as implemented)

Runtime (C++):

- `if` / `elseIf` / `else` are sibling containers; the list runners
  (`execute_block_list` and `execute_compiled_blocks`) own a per-list
  `ChainState` (`src/xblox/blocks/block_types.hpp`) which handlers read via
  `BlockRuntime::chain`. The nested `elseIfBlocks`/`alternate` shape is gone.
- `case` / `switchDefault` are registered container blocks; inside a `switch`
  the parent drives them (first match wins, no fallthrough), standalone they
  are orphans (warning, body skipped).
- `break` exits the nearest `while`/`for`/iterable loop only: loops track
  depth via `FlowState::loop_depth` and clear `break_requested` on exit
  (`LoopGuard` in `builtin_blocks.cpp`, plus the compiled While path).
  `break` outside any loop emits a warning and has no effect.

Manifest / palette: `if`, `elseIf`, `else`, `switch`, `case`,
`switchDefault`, `break`, `group`, `for`, `while` are all registered and
palette-visible (group Flow/Logic), so the hosted native palette can create
every flow block.

UI (`apps/xblox/src/xblox/react`): `elseIf`/`else` are normal container rows
(body `items`); `canContain` no longer hard-blocks moving `case`/
`switchDefault`/`else` anywhere — placement is soft-validated at run time
(orphan warnings). Lint badges in the tree are still TODO (Phase 3).

## Design

### D1 — sibling-chain `if` / `elseIf` / `else`

`else` and `elseIf` become first-class **container blocks** (like `group`),
siblings of the `if` in the same block list:

```json
{ "kind": "if",     "condition": "x > 1", "consequent": [ … ] },
{ "kind": "elseIf", "condition": "x > 0", "items": [ … ] },
{ "kind": "else",   "items": [ … ] }
```

- Body key: `if` keeps `consequent` (back-compat); `elseIf`/`else` use
  `items` (they are groups).
- **Chain rule**: a chain is a contiguous run of siblings starting at an
  `if`, followed by zero or more `elseIf`, optionally terminated by one
  `else`. Any other kind (or list end) terminates the chain.
- **Execution**: the list runner (`execute_block_list`) owns chain state —
  handlers stay dumb. When a branch in the chain has run, subsequent
  `elseIf`/`else` members are skipped (emit `skipped` events so the UI can
  badge them).
- **Orphans are never fatal**: an `elseIf`/`else` with no preceding chain
  member (user dragged it anywhere — this is allowed by design) emits a
  single `warning` diagnostic event ("orphan else — no preceding if") and
  runs nothing. Same for `elseIf` appearing after `else` (an `else` consumes
  the chain).
- **No back-compat**: the nested `elseIfBlocks`/`alternate` shape was removed
  from `if_block`, the manifest, the TS schema, and all fixtures. The sibling
  form is the only form.

### D2 — `switch` / `case` / `default`

- Register `case` and `switchDefault` in the C++ registry as palette-visible
  container blocks (body key `consequent`, group Flow) so the hosted palette
  can create them.
- `case` containers stay movable anywhere (same philosophy as `else`):
  outside a `switch` they are orphans → warning diagnostic, skipped.
- Semantics unchanged: first matching case wins, `switchDefault` is the
  fallback, no fallthrough — `break` inside a case is a no-op by design
  (document, don't error).

### D3 — `break` scoping

- Target semantics: `break` exits the **nearest enclosing** `while`/`for`
  only; execution continues after the loop.
- Fix: loops reset `break_requested` on exit (both the event path
  `for_block`/`while_block` and the compiled `CompiledKind::While` path).
- `break` outside any loop: warning diagnostic, no effect (today it silently
  kills the rest of the pass).

### D4 — UI: free movement, soft validation

Users can move any language block (with its children) anywhere — even when
syntactically wrong. Hard drop-rejections are replaced by soft validation:

- Remove the `canContain` hard block for switch items; keep `accepts` lists
  as *highlight hints* only.
- A shared TS lint pass (mirror of the executor's chain rules) computes
  per-block diagnostics: orphan `else`/`elseIf`/`case`, `elseIf` after
  `else`, `break` outside a loop. Tree rows show a small warning badge; the
  property panel shows the message.
- `else`/`elseIf`/`case` render as normal container rows (Flow color), with
  labels `else`, `else if <condition>`, `case <comparator> <expr>`.

## Todos

### Phase 0 — harness (CLI first)

- [x] `tests/orchestrator/test-xblox-language.mjs`: fixture-driven CLI suite
      (temp `.xblox` files → `tanit-cli xblox run` → assert stdout markers).
- [x] `package.json`: `test:xblox-language` script.
- [x] ~~Future-flag tests~~ — dropped; the sibling-chain tests run
      unconditionally (no back-compat, no `PM_XBLOX_LANGUAGE_FUTURE`).

### Phase 1 — C++ executor (sibling chains + break fix)

- [x] `execute_block_list` + `execute_compiled_blocks`: per-list `ChainState`
      for `if`/`elseIf`/`else` siblings (skip-after-match, orphan warnings) —
      `src/xblox/xblox_commands.cpp`.
- [x] Register `elseIf` / `else` (body `items`) and `case` / `switchDefault`
      (body `consequent`, orphan handler) — `src/xblox/blocks/builtin_blocks.cpp`.
- [x] `break_requested` reset at loop exit (`LoopGuard` for for/while, the
      iterable child loop, and the compiled While); `break` outside a loop
      (tracked via `FlowState::loop_depth`) warns and does nothing.
- [x] `append_plan`: `elseIf`/`else` bodies (`items`) and naked `case`/
      `switchDefault` bodies (`consequent`) included in simulate plans.
- [x] Rebuild + green: 23/23 language, 10/10 controllers, 369/369 xblox-next.

### Phase 2 — UI (move-anywhere + rendering)

- [x] `schema/blocks-file.ts`: `if` lost `elseIfBlocks`/`alternate`;
      `elseIf`/`else` node types + zod schemas added.
- [x] `BlockModel.ts`: `createDefaultBlock`/`getChildren`/`primaryChildKey`/
      labels/colors for `elseIf`/`else`; `deleteSelectedFromList` updated.
- [x] `canContain`/`canInsertIntoList`/`setListAtPath`: switch-item hard
      blocks removed — any flow block moves anywhere (children intact);
      misplacement surfaces as run-time orphan warnings.
- [x] Palette: `elseIf`/`else` in the JS dev palette; hosted palette gets all
      flow blocks from the native manifest registration.
- [x] Prototype preview (`execution.ts`) mirrors the chain semantics;
      sample data + mock host bridge converted to sibling form.
- [x] `npm run build:embed` (apps/xblox).
- [x] Chain sections: the tree renders an if/elseIf/else chain as one visual
      section — a vertical rail + faint tint span the members and their
      visible children (annotated in the flatten pass, mirroring the C++
      chain rule), `↳` continuation glyphs on `elseIf`/`else`, and
      whole-chain hover highlight.
- [x] Lint badges (tree row): orphan `else`/`elseIf` (amber dashed rail + ⚠),
      `case`/`switchDefault` outside a switch, `break` outside a loop
      (loop ancestry includes native iterable producers via exec flags).
- [ ] Lint message in the property panel (badge tooltip only for now).

### UI automation contract (UIA selectors)

The tree exposes stable selectors for Windows UI Automation (consumed by
`src/win/assistant/app_use.cpp` / `app_inspect.cpp`, where AutomationId is
the highest-scoring selector and `find_element_by_aid` matches it exactly):

- Tree container: `id="xblox-tree"`, `role="tree"`, name `XBlox blocks`.
- Block row: `id="xblox:<path>"` (e.g. `xblox:4/consequent/0`) → UIA
  AutomationId; `aria-label="<kind>: <label>"` → UIA Name;
  `aria-level`/`aria-posinset`/`aria-setsize` expose real nesting (rows are
  flat DOM siblings).
- Inline param editors: `id="xblox:<path>:<field>"` (`condition`,
  `variable`, `name`, `expression`, `target`, `level`, `message`,
  `comparator`, `initial`, `final`, `modifier`) with descriptive
  aria-labels — settable via the UIA Value pattern (`set_value_by_aid`).
- Expander buttons are labeled `Expand`/`Collapse`; childless ones are
  `aria-hidden`.

Paths are positional, so AutomationIds shift when blocks are reordered —
re-inspect after structural edits.

### Pseudo-code markdown (`xblox run --md`)

`tanit-cli xblox run --src <file> --md` prints the document as brief
pseudo-code markdown and exits without running it (`--md-numbered` switches
to numeric bullets; `--md-filter <kinds>` excludes block kinds with their
subtrees — comma-separated/repeatable, default `stdout`, `none` includes
everything). Generator: `src/xblox/xblox_markdown.{hpp,cpp}`
(`media::xblox::document_to_markdown`, options struct for hosts). Layout:
`# <file>` title, a `> loops every Nms…` note when the document loops,
`## Context` bullets (one per context variable, compact JSON values),
`## Script` bullet tree mirroring the block nesting
(`if`/`else if`/`else`, `switch`/`case`/`default`, `while`/`for`/`break`,
`print`/`set`/`get`). Non-language blocks render as
`<group>/<kind> <detail>` using the registry group (e.g.
`audio/audioRecordStop`, `input/keyEvent`), with a `-> <var>` suffix for
stored results (storeAs/storeResult/target). Covered by the `--md` section
of `test-xblox-language.mjs` against `tests/xblox/language.xblox`. Later:
combine with `--simulate` to pair the outline with predicted events.

### Mermaid flowcharts (`xblox run --md --mermaid`)

`--mermaid` (requires `--md`) replaces the `## Script` bullet tree with a
fenced ` ```mermaid ` flowchart; title, loop note and `## Context` bullets
stay as markdown above the diagram (variables live there, not in the graph).
Mapping: leaves are rectangles, `if`/`elseIf`/`switch`/`while`/`for` are
decision diamonds; chains wire `-->|yes|` into branch bodies and `-->|no|`
to the next chain member, switches label edges with the case value (or
`default` / `no match`), loops use `-->|do|` into the body, a back-edge from
the body to the diamond, and `-->|done|` onward; `break` nodes edge to the
nearest loop's continuation. `--md-filter` applies the same way. Knobs:

- `--mermaid-type flow|sequence` — default `flow`. `sequence` reads the
  script as a conversation: `Script` drives, registry groups (Audio, Input,
  Files, …) are participants declared in first-use order, core language
  blocks are Script self-messages, stored results come back as dashed return
  arrows (`Audio-->>Script: wavPath`). Control flow maps onto sequence
  fragments: if/elseIf/else chains → `alt`/`else`, switch cases → `alt` per
  case (+ `else default`), while/for → `loop`, `break` → `Note over Script`,
  and a controller-style document loop wraps the whole diagram in
  `loop every Nms`. `--mermaid-direction` / `--mermaid-color` apply to flow
  only (sequence diagrams have no linkStyle) and are ignored for sequence.
- `--mermaid-direction TD|LR|BT|RL` — default `TD` (vertical, flow only).
- `--expand-parameters` — full pseudo-line node labels
  (`input/keyEvent Shift+F12 -> f9`) instead of compact
  (`input/keyEvent -> f9`); conditions always appear on diamonds.
- `--mermaid-color edges|groups|both|none` — default `both`. `edges` tints
  links by semantics via `linkStyle` (yes `#9ece6a`, no/no-match `#f7768e`,
  loop do/done/back-edges `#7aa2f7`, switch cases `#e0af68`, plain sequence
  via `linkStyle default #565f89`). `groups` strokes nodes by registry group
  via `classDef <group>` (flow diamonds fixed `#bb9af7`; other groups cycle a
  palette in first-appearance order, so colours are stable per document).
  Class names are the group names (`audio`, `input`, `files`, …) so hosts
  can override the classDefs.

Emitter: `MermaidEmitter` in `xblox_markdown.cpp` (options on
`MarkdownOptions` so hosts can reuse it). Output renders in any mermaid
renderer (GitHub, beautiful-mermaid, craft.do).

### Phase 3 — UI harness

- [x] `test-xblox-ui.mjs` (npm run test:xblox-ui) — full win32 → WebView2
      round trip: launches `tanit --ui-preset viewer --src
      tests/xblox/language.xblox` on the isolated tests/ui profile and drives
      it cross-process via `assistant app-inspect` / `app-use batch` (UIA).
      Covers: viewer-ready signal (AutomationId `xblox-tree`), rows as
      TreeItems with `xblox:<path>` ids + structured names, nested rows,
      Value-pattern read AND write (set-value on `xblox:2:condition` →
      React re-render → row Name updates), click-element → row focus.
- [ ] Drag scenarios (float the `else` out → orphan badge, drag back, run
      the doc, assert branch output) — extend `test-xblox-ui.mjs` with
      app-use drag steps when needed.

## Test fixtures (harness reference)

Markers are `MARK:<name>` lines via `stdout` blocks; the harness counts exact
token occurrences. Fixtures: `if-then`, `if-else`, `elseif-first-match`,
`chain-skips-after-match`, `chain-interrupted`, `orphan-else`,
`orphan-elseif`, `double-else`, `nested-chain`, `switch-case`,
`switch-default`, `orphan-case`, `while-break`, `for-break`, `nested-break`,
`orphan-break`.
