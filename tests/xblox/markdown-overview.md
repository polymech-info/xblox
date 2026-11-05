# Markdown Overview

> Pretty index for the Markdown files in this folder: `tests/xblox`.

## At a glance

| Metric | Value |
|---|---:|
| Markdown files found | 5 |
| Primary topic | XBlox tests, benchmarks, images, and performance roadmap |
| Main benchmark target | XBlox C++ runner throughput |
| Generated overview | `markdown-overview.md` |

## File index

| File | Size | Purpose | Highlights |
|---|---:|---|---|
| [`agent.xblox.dump.md`](agent.xblox.dump.md) | 1.6 KB | Dump of an `agent.xblox` JSON workflow | Captures a local agent run, current folder answer, and filesystem listing of Markdown files. |
| [`bench-results.md`](bench-results.md) | 416 B | Append-only benchmark log | Latest compact table for `fps-raw-loop` and `fps-unthrottled` on AMD Ryzen 7 3700X. |
| [`images.md`](images.md) | 99 B | Image asset list | References three video/filter/writer frame image files. |
| [`performance.md`](performance.md) | 14.9 KB | XBlox C++ performance roadmap | Detailed TODOs, benchmark history, guardrails, and recent wins. |
| [`markdown-overview.md`](markdown-overview.md) | generated | This overview | Human-friendly summary of all Markdown files in this folder. |

## Highlights

### 🚀 Current benchmark snapshot

From `bench-results.md` and `performance.md`:

| Benchmark | Current / recorded throughput | Notes |
|---|---:|---|
| `fps-raw-loop` | ~612M iters/sec | Pure tight loop floor; constant `setVariable` path. |
| `fps-unthrottled` | ~9.5M iters/sec | Multi-block compiled scalar-slot engine path. |
| C++ raw comparison | ~616.7M iters/sec | Raw C++ XBlox loop from the comparison table. |
| Node raw comparison | ~247.8M iters/sec | Matching raw-loop simulation. |
| Rust raw comparison | ~128.1M iters/sec | Matching raw-loop simulation. |
| Python raw comparison | ~8.1M iters/sec | Matching raw-loop simulation. |

### 🧭 Performance roadmap themes

`performance.md` organizes future work around these tracks:

- **Measurement discipline** — bounded benchmark mode, compact perf summaries, platform/compiler metadata, cross-platform scripts.
- **Hot-loop overhead reduction** — cached JSON fields, direct dispatch, less allocation, fewer path/string builds.
- **Compiled and bytecode engines** — compiled AST, execution modes, bytecode verifier, parity tests.
- **Expression compilation** — fast paths, RPN compiled expressions, muParser reuse, benchmark mode splits.
- **Fast context store** — numeric scalar slots alongside JSON context.
- **Event pipeline control** — count-only, logs/errors, and full event modes.
- **Loop specialization** — optimized `while` shapes, one/two-item bodies, cancellation polling intervals.
- **Benchmark matrix** — Windows/Linux/macOS compiler and flag coverage.

### ✅ Recent wins captured

The performance notes document several completed optimizations, including:

- Non-JSON CLI runs can count events without storing full event vectors.
- Hot simple expressions bypass muParser.
- Builtin dispatch avoids registry lookups in hot paths.
- Simple scalar variables mirror into a numeric context store.
- Non-event core blocks can execute through a compiled block representation.
- Compiled expressions now use a generic RPN program over scalar slots and `nowMs`.
- The compiled block engine is wired back into `run_block_roots` for suitable runs.
- Benchmarking is now captured via `npm run bench:xblox` and appends to `bench-results.md`.

## File summaries

### `agent.xblox.dump.md`

A Markdown-wrapped JSON dump of `agent.xblox`.

**What it contains:**

- Source path and file metadata for `agent.xblox`.
- Captured context values, including the detected current working directory.
- A stored filesystem listing for Markdown files.
- Workflow roots for an LLM agent prompt, logging, and Markdown file listing.

**Use it for:** understanding or debugging the small XBlox workflow that produced the folder/file listing context.

---

### `bench-results.md`

A compact append-only benchmark report.

**Latest recorded environment:**

- Date: `2026-06-01`
- CPU: `AMD Ryzen 7 3700X 8-Core Processor`
- Build: `RelWithDebInfo`

**Recorded results:**

| Benchmark | Iterations/sec | Note |
|---|---:|---|
| `fps-raw-loop` | 612,117,201 | Pure tight-loop floor. |
| `fps-unthrottled` | 9,511,502 | Multi-block compiled scalar-slot engine. |

---

### `images.md`

A tiny image inventory.

**Referenced image files:**

- `video-detect-record-filtered-frame.jpg`
- `video-filter-out.jpg`
- `video-writer-frame.jpg`

---

### `performance.md`

The main planning document for XBlox C++ performance work.

**Key story:** `fps-unthrottled` previously regressed to roughly `151k/sec` when the compiled engine became orphaned. Reconnecting the compiled engine and generalizing expression compilation restored throughput to roughly `9.4M/sec` — about **62× faster** than the regressed path.

**Major sections:**

1. P0 measurement requirements.
2. P1 hot-loop interpreter overhead work.
3. P1 compiled block execution.
4. P1 bytecode execution options.
5. P1 expression compilation.
6. P1 fast context store.
7. P2 event pipeline and loop specialization.
8. P3 native block ABI and benchmark matrix.
9. Guardrails and recent wins.

**Guardrails to preserve:**

- Do not break XBlox expression or context tests.
- Preserve full event payloads for `--json`.
- Preserve graceful Ctrl+C exit code `130`.
- Preserve muParser compatibility for expressions outside fast paths.
- Maintain behavior parity across `json`, `compiled`, and `bytecode` engines before changing defaults.

## Suggested next actions

1. Keep appending new measured runs to `bench-results.md`.
2. Add platform/compiler metadata to future benchmark output.
3. Split benchmark modes into `raw`, `fast-expr`, `muparser`, and `mixed`.
4. Continue checking off completed items in `performance.md`.
5. Keep this overview regenerated when Markdown files are added or major benchmark numbers change.
