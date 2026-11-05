# XBlox performance

How to measure, what the runtime actually costs, and the backlog.

## Measure

```bash
npm run bench:xblox                  # ~3s/case, auto-calibrates N
npm run bench:xblox -- --append      # append row to bench-results.md
npm run bench:xblox -- --only fps-raw-loop,fps-unthrottled --seconds 5
```

Append-only history: `packages/xblox/tests/xblox/bench-results.md`.

Functional correctness (separate from perf): `npm run test:xblox-next` and
`packages/xblox/scripts/xblox-tests.sh`. Do **not** use the xblox-next harness for throughput
regression gates — it exercises `--json` event payloads and nested-block
lifecycles, not loop FPS.

Bench harness (`packages/xblox/tests/xblox/bench.mjs`):

- No `--json` (no trailing report dump)
- `--event-mode count` (aggregate counters only; no retained event vector)
- `--log-level off`, child stdout drained
- Startup median subtracted before iters/sec
- Peak RSS polled every 50 ms

## Baselines (AMD Ryzen 7 3700X)

| benchmark | healthy (2026-06) | **current (2026-07-10)** | what it measures |
|---|---:|---:|---|
| `fps-raw-loop` | ~600,000,000 it/s | **~63,000** | `while(1){ setVariable=const }` — interpreted tight-loop floor |
| `fps-unthrottled` | ~11,000,000 it/s | **~33,000** | `while(1){ expr; if(cond) }` — compiled scalar-slot engine |
| `language` | ~850 it/s | ~100 it/s | full doc: if/elseIf/switch/while/for/break |
| `expressions-bench` | ~2,000 it/s | ~1,400 it/s | muparser/jq/PREVIOUS/templates, no stdout |

Healthy numbers assume **events disabled** on the CLI perf path (see below).
The July row is a real regression (~10⁴× on raw loop), not stale assertions.

Cross-runtime raw comparison (historical smoke, pre-bench.mjs):

| Runtime | Script | iters/sec (approx) |
|---|---|---:|
| C++ XBlox | `fps-raw-loop.xblox` | ~617,000,000 |
| Node | `fps-raw-loop-node.mjs` | ~248,000,000 |
| Rust | `fps-raw-loop-rust.rs` | ~128,000,000 |
| Python | `fps-raw-loop-python.py` | ~8,000,000 |

Goal: stay clearly above Python; close the gap to Rust/Node on realistic scripts,
not on the raw no-expression floor.

---

## Execution model (2026)

Documents are still JSON on disk. At run time the engine layers:

```
run_blocks_file / PassEngine
  ├─ ScopeSession + ScopeStack     ← muParser bindings, loop locals (NH_Context)
  ├─ NumericContextStore           ← scalar slots for compiled + fast reads
  ├─ OutputFrameStore              ← blockId → typed outputs (wiring bindings)
  └─ per-pass: prepare() → run_pass()
       ├─ compiled fast path       ← PassEngine::use_compiled
       ├─ raw while fast path      ← while_block is_raw_constant_set_loop
       └─ interpreted path         ← BlockIO + resolve_block_inputs + handler
```

### Variables

- **JSON context** — objects, command payloads, final reporting, `storeAs` sinks.
- **NumericContextStore** — simple scalars mirrored to slots during compilation /
  hot reads (`frames`, loop counters, …).
- **ScopeStack** — frame chain for loop/container locals; bumps `sequence()` on
  push/pop/new-var so muParser rebinding is skipped when bindings are stable.
- **PREVIOUS** — implicit primary output of the last block; still valid alongside
  explicit `bindings`.

### Expressions

Evaluation order on the interpreted path:

1. **`eval_fast_expression`** — literals, direct reads, simple binary ops,
   comparisons (no muParser).
2. **`CompiledExpr` RPN** — shunting-yard program over `{literal, slot, nowMs}`
   used by the compiled engine (`fps-unthrottled` shapes).
3. **`eval_mu_expression`** — cached muParser instance; rebind only when
   `ScopeStack::sequence()` changes; otherwise refresh `PREVIOUS`/`nowMs` and
   `Eval()` cached bytecode.

Param text that is expression-shaped goes through the resolver only for **explicit
document literals** — binding-sourced values are already typed and must not be
re-evaluated (see `resolve_block_inputs`).

### Input resolver (`resolve_block_inputs`)

Every interpreted builtin invocation resolves declared `ParamDef`s before the
handler runs (`src/xblox/blocks/block_resolver.hpp`,
`builtin_blocks.cpp::resolve_block_inputs`):

| stage | work |
|---|---|
| **binding** | `bindings[param]` → `blockOutput` (by `blockId` + slot), `scopeVariable`, `previous` |
| **source** | explicit field → declared source (PREVIOUS / variable) → default |
| **transform** | kind cast / coercions |
| **resolve features** | `${var}` / `${ENV:…}` / `${KNOWNFOLDER:…}` interpolation; globs; `ParamResolve::Custom` hooks |
| **constraints** | MustExist, Writable, enum clamp, … → diagnostics (severity ≥2 skips handler) |

Wiring v2 adds **`blockId`-keyed output frames** and document-level
`bindings` objects. Any non-empty `bindings` tree disables the compiled fast
path (`json_tree_has_bindings` in `PassEngine::prepare`).

### Event modes

| flag | effect |
|---|---|
| `--event-mode full` | retain all events; full emit path |
| `--event-mode errors` | retain failures only; successful events use aggregate bookkeeping (default) |
| `--event-mode count` | retain no events; aggregate counters only; enables raw-loop + compiled gates |
| `--json` (CLI) | defaults to `full` unless `--event-mode` is explicit |
| `--quiet` (CLI) | suppresses stdout block text; event still emitted |

The bench harness explicitly uses `count`; normal CLI uses `errors`, and JSON
reports use `full`.

---

## P0 — restore perf path

**Cause:** `876a59d75` (2026-06-24) changed CLI from
`collect_events = st.xblox_json` to `collect_events = true` always
(`pm_image_cmd_xblox.cpp`), so bench runs (no `--json`) never disable events.

That disables both fast gates:

```cpp
// PassEngine::prepare — compiled engine
if (!events_enabled && !simulate && !is_raw_constant_set_roots(roots)
    && !json_tree_has_bindings(roots)) { … use_compiled … }

// while_block — raw constant loop (~600M it/s)
if (!runtime.events_enabled && is_raw_constant_set_loop(…)) { … }
```

Symptoms: ~63k it/s raw loop, ~633 MB RSS (events stored in `result.events`).

- [x] **P0.1** Split “surface errors on stderr” from “collect full event vector”
  with `--event-mode full|errors|count`.
- [ ] **P0.2** Re-run `npm run bench:xblox -- --append` after fix; expect
  `fps-raw-loop` back near ~600M and RSS ~21 MB on this machine.
- [ ] **P0.3** Add a CI smoke (optional): `--only fps-raw-loop --seconds 1` with
  a floor threshold so this regression cannot land silently again.

---

## P1 — hot path + resolver cost

### Engine

- [x] Compiled block list + RPN expressions for core control blocks (non-event runs).
- [x] Raw `while(1){ setVariable constant }` tight loop (non-event runs).
- [x] Function-pointer builtin dispatch; skip path strings when events off.
- [x] muParser bytecode reuse keyed on `ScopeStack::sequence()`.
- [ ] **Bindings-aware compiled path** — today any `bindings` object forces interpreted
  mode for the whole document; consider per-block fallback or binding resolution at
  compile time for stable `blockOutput` edges.
- [ ] **Output frame churn** — `clear_block_output_frames` every block when events on;
  scope to event/full mode or lazy clear on `blockId` overwrite.
- [ ] **resolve_block_inputs** — cache resolved frames per `(blockId, scope generation)`
  for loop bodies whose bindings/fields are static across iterations.

### Expressions

- [x] Fast path + compiled RPN + cached muParser (see above).
- [ ] Split bench fixtures: `raw` / `fast-expr` / `muparser` / `mixed` (partially
  covered by `expressions` vs `expressions-bench`; add forced muParser fixture).
- [ ] `--expression-fast-paths=false` for A/B measurement.
- [ ] Profile buckets: bind+Eval vs resolve vs dispatch (`PIXLWIZ_XBLOX_PROFILE_BLOCKS`).

### Context

- [x] NumericContextStore + slot compilation.
- [ ] Defer JSON sync for scalar slots until event payload / command / final report
  needs the object view.

---

## P2 — event pipeline + loops

- [x] Ship `--event-mode full|errors|count` (see table above).
- [ ] In `count`: no `ExecutionEvent` construction for successful hot blocks.
- [ ] In `logs`: errors + log blocks only; skip successful setVariable/if noise.
- [ ] Loop specialization: inline common `while(cond){1–2 items}` without recursive
  `execute_block_list` per iteration (partially done for raw-const shape).
- [ ] Cancellation poll interval on ultra-hot loops (raw path uses chunk polling).

---

## P3 — tooling + matrix

- [x] `bench.mjs` — bounded loopLimit, startup subtract, append-only results.
- [ ] Emit compact perf summary on non-JSON CLI exit (elapsed, event count, engine
  path: compiled / raw / interpreted).
- [ ] Cross-platform matrix row: OS, CPU, compiler, build type, git commit in
  `bench-results.md`.
- [ ] Optional `xblox-run-spawn` bench row for nested `xbloxRun` throughput (separate
  from loop FPS; parent-loop + repeated spawn is seconds/pass, not a CI gate).

---

## Guardrails

- [ ] Do not break `npm run test:xblox-next` (variables, expressions, wiring, loops, …).
- [ ] Do not break legacy `npm run test:xblox:expressions` / `test:xblox:context`.
- [ ] Preserve full event payloads for `--json` / WebView diagnostics.
- [ ] Preserve muParser compatibility outside fast paths.
- [ ] Preserve v2 binding semantics (`blockId`, unavailable source diagnostics).
- [ ] Preserve graceful Ctrl+C exit `130`.

---

## Key files

| area | path |
|---|---|
| Bench harness | `packages/xblox/tests/xblox/bench.mjs` |
| Results log | `packages/xblox/tests/xblox/bench-results.md` |
| CLI run options | `src/cli/pm_image_cmd_xblox.cpp` |
| PassEngine / gates | `src/xblox/xblox_commands.cpp` |
| Resolver + bindings | `src/xblox/blocks/block_resolver.hpp` |
| Input pipeline | `src/xblox/blocks/builtin_blocks.cpp` (`resolve_block_inputs`) |
| Param schema | `src/xblox/blocks/block_params.hpp` |
| BlockIO / events | `src/xblox/blocks/block_types.hpp` |
| Scope + options | `src/xblox/runtime.hpp`, `src/xblox/xblox_types.hpp` |
| Wiring contract | `docs/xblox/xblox-dev.md` |

---

## History worth keeping

- **`fps-unthrottled` orphaned compiled engine** (~151k it/s): re-wiring
  `CompiledBlock` into `run_block_roots` restored ~9–11M it/s (~62×).
- **muParser recompile on every scope push/pop** (June 2026): sequence-gated
  rebind raised `expressions-bench` ~1,282 → ~2,004 it/s.
- **jq filter cache + zero-copy converters**: `expressions-bench` ~41 → ~1,177 it/s
  (see `bench-results.md` 2026-06-18 rows).
