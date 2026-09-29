# XBlox

XBlox is the block-tree automation runner used by the `apps/xblox` web UI and the native `tanit-cli xblox run` command.

End-user guides:

- [std.md](./std.md) - standard I/O blocks (`stdout`, `stdin`, `exit`) and CLI piping recipes.
- [expressions.md](./expressions.md) - expressions, variables, `${ENV:...}`, known folders, globs, and scoping.
- [expressions-next.md](./expressions-next.md) - roadmap: strings in muParser, richer built-ins, vendored-parser plan.
- [types.md](./types.md) - parameter types (ParamKind), the resolver pipeline, and feature composition (sourcing, constraints, globs, hooks).

Documents are JSON blocks-files:

```json
{
  "version": 1,
  "context": {},
  "roots": []
}
```

## Involved Files

Native runner:

- `src/xblox/runtime.hpp` - public C++ runner runtime/result types: events, options, command invocation, result.
- `src/xblox/types.hpp` - shared native xBlox support types, including captured command output.
- `src/xblox/xblox_commands.hpp` - public C++ runner entry points.
- `src/xblox/xblox_commands.cpp` - JSON runner, compiled non-event core-block runner, expression handling, context handling, command execution.
- `src/xblox/utils/conv.hpp` / `src/xblox/utils/conv.cpp` - JSON boundary conversion helpers for native events and payload primitives.
- `src/xblox/blocks/builtin_blocks.hpp` - native builtin block ABI.
- `src/xblox/blocks/builtin_blocks.cpp` - builtin block implementations: `wait`, `break`, `setVariable`, `getVariable`, `if`, `for`, `while`, `switch`, `log`.
- `src/xblox/blocks/data_blocks.hpp` / `src/xblox/blocks/data_blocks.cpp` - data transform blocks, starting with `Parse`.
- `src/xblox/blocks/network_blocks.hpp` / `src/xblox/blocks/network_blocks.cpp` - curl-backed network blocks: `network`, `httpRequest`, and `fetch`.
- `src/xblox/blocks/shell_blocks.hpp` / `src/xblox/blocks/shell_blocks.cpp` - shell execution blocks backed by the LLM RunTool security, timeout, capture, and streaming hook path.
- `src/cli/pm_image_cmd_xblox.cpp` - CLI command wiring.
- `src/cli/pm_image_cli_state.hpp` - CLI state/options for XBlox.

Web and shared runtime:

- `apps/xblox/src/PrototypeApp.tsx` - main XBlox app and host/mock runtime wiring.
- `apps/xblox/src/xblox/react/xblox-builder.tsx` - builder UI and property panels.
- `apps/xblox/src/schema/blocks-file.ts` - block schema types.
- `apps/xblox/src/xblox/prototype/execution.ts` - web/dev execution plan for preview mode.
- `apps/xblox/src/xblox/prototype/mockHostBridge.ts` - mocked native host bridge for dev.
- `apps/xblox/src/xblox/prototype/sample-data.ts` - sample commands and blocks.
- `apps/xblox/src/xblox/hostBridge.ts` - XBlox app host bridge re-export.
- `apps/shared/xblox/runtime.ts` - shared TS runtime interfaces and memory/host runtime adapters.
- `apps/shared/web/hostBridge.ts` - shared WebView host bridge and XBlox RPC contracts.

Tests and benchmarks:

- `tests/orchestrator/test-xblox.mjs` - CLI regression suites.
- `tests/xblox/network.xblox` - live network fixture that fetches the public Cassandra page JSON and exercises `PREVIOUS`.
- `tests/xblox/performance.md` - performance notes and TODO backlog.
- `tests/xblox/while-count.xblox` - one-minute/tick script.
- `tests/xblox/fps-unthrottled.xblox` - realistic hot-loop FPS script.
- `tests/xblox/fps-raw-loop.xblox` - raw no-expression loop script.
- `tests/xblox/fps-unthrottled-node.mjs`
- `tests/xblox/fps-unthrottled-python.py`
- `tests/xblox/fps-unthrottled-rust.rs`
- `tests/xblox/fps-raw-loop-node.mjs`
- `tests/xblox/fps-raw-loop-python.py`
- `tests/xblox/fps-raw-loop-rust.rs`
- `tests/xblox/tests.sh` - ad hoc manual benchmark/run list.

## CLI

Build:

```powershell
npm run build:cpp:cli
```

Run a blocks file:

```powershell
dist\win-x64\tanit-cli.exe xblox --log-level info run --src tests\xblox\while-count.xblox
```

JSON report mode:

```powershell
dist\win-x64\tanit-cli.exe xblox --log-level off run --json --no-wait --src tests\xblox\while-count.xblox
```

Useful options:

- `--src <path>` - required blocks-file path.
- `--commands <path>` - optional `commands.json` override.
- `--json` - collect and print full event payloads.
- `--dry-run` - stage CLI/external commands without spawning.
- `--no-wait` - skip sleeping for wait blocks.
- `--max-loop-iterations <n>` - loop cap.
- `--arg <value>` - extra arg forwarded to command invocations.

Important behavior:

- Non-JSON CLI runs count events but do not retain the full event vector.
- JSON mode preserves full event payloads and paths for tests and diagnostics.
- Ctrl+C installs CLI interrupt handlers and should exit as failed with code `130`.
- Command blocks capture process stdout/stderr into line arrays on command events.
- Command process blocks use `ExecutionOptions::default_timeout_ms` (`30000` ms by default); timeout exits as failed with code `124`, cancellation exits as failed with code `130`.
- Command payloads can include `storeResult` or `target` to write a structured command result back into the runtime context for later blocks.
- Network blocks capture `raw`, `statusCode`, response headers, URL metadata, curl errors, redirects, retries, and timeout/cancel failures into both the event payload and implicit `PREVIOUS`.
- `Parse` defaults to parser `jq`, reads `PREVIOUS` by default, transforms it with a jq-style path filter such as `.page.title`, and writes the result back to `PREVIOUS`. The current native parser is a small path-selector subset; full jq semantics can be added later from the jq execution model.
- Any block can set `storeAs` (alias `storeVariable`) to store its latest result into scope for later blocks. For data transforms that expose a `result` field, the stored value is that transformed result.
- `shell` blocks accept `mode`, `shell`, `command`/`line`, `cwd`, `args`, `timeoutMs`, `background`, and `storeAs`. Foreground shell blocks validate through `RunValidators`, capture stdout/stderr, collect streaming chunks, and put stdout into `PREVIOUS`/`PREV`. Background execution is reserved and currently fails closed.
- `setVariable` can copy an existing context value with `valueFrom`/`from`; `log` resolves context values such as `PREVIOUS` before writing.
- Blocks abort following siblings on error by default. A block can opt into continuing with `continueOnError: true`, `runFlags: { "continueOnError": true }`, or `onError: "continue"`.

## Runtimes

Native C++ runtime:

- Public entry points are `run_blocks_file`, `run_block_roots`, and `build_execution_plan`.
- `ExecutionOptions` controls commands, waiting, event collection, max loop iterations, default command timeout, and cancellation.
- `BlockRunFlags` carries per-block execution policy. Defaults are conservative: abort on error, cancellable, event-consuming, and no per-block timeout override.
- Builtins use a native function-pointer bridge instead of per-block `std::function` callbacks.
- Non-event core blocks can run through a compiled representation for `setVariable`, `if`, `while`, and `log`.
- The raw constant loop benchmark has a dedicated no-JSON hot path.
- `ExecutionEvent` carries the legacy fields plus structured `blockId`, `type`, `errorCode`, `stdout`, and `stderr` fields for run result consumers.

Web runtime:

- `createMemoryXbloxRuntime` runs in dev/mock mode with an in-memory context.
- `createHostBridgeXbloxRuntime` sends `xbloxCommandRun` and `xbloxDocumentRun` RPC calls through the host bridge.
- `PrototypeApp` selects host bridge runtime when embedded, and installs a mock bridge in standalone dev.

Host bridge:

- Shared contracts live in `apps/shared/web/hostBridge.ts`.
- XBlox payload types include `XbloxCommandsPayload`, `XbloxDocumentPayload`, `XbloxRunEvent`, and `XbloxRunPayload`.
- Native WebView code should expose document get/save/run and command run methods through this bridge.

## Current Status

Implemented:

- First-class `command` blocks in the web builder.
- Shared custom command types/editing support used by settings and XBlox.
- C++ builtins for `if`, `for`, `while`, `switch`, `wait`, `break`, `setVariable`, `getVariable`, and `log`.
- muParser fallback for native expression evaluation.
- Mutable C++ run context with dot-path reads/writes.
- `nowMs` expression variable for time-based scripts.
- Ctrl+C cancellation checks for loops/waits.
- Native logger-backed `log` block.
- Non-JSON event count mode for CLI performance runs.
- Fast expression paths for common benchmark expressions.
- Cached muParser fallback instances.
- Numeric scalar context mirror.
- Native builtin ABI via function pointers.
- Direct builtin dispatch.
- Compiled non-event core-block path for supported hot scripts.
- Raw no-JSON benchmark path for `while(1) { setVariable constant }`.

Latest rough performance notes are in `tests/xblox/performance.md`. Current smoke numbers from the latest slice:

- `fps-unthrottled.xblox`: `546,811,263` counted events in about `6.2s`; roughly `22M` loop iterations/sec for the current block shape.
- `fps-raw-loop.xblox`: about `598M` raw loop iterations/sec.

Verified after the latest runner changes:

```powershell
npm run build:cpp:cli
npm run test:xblox:expressions
npm run test:xblox:context
```

## Next TODOs

Near-term, doable:

- Add an explicit CLI/runtime engine flag: `--engine json|compiled|bytecode`.
- Keep `json` as the full diagnostics path and make `compiled` opt-in until parity is proven.
- Extend compiled blocks beyond the current hot subset:
  - `getVariable`
  - `for`
  - `switch`
  - `wait`
  - `break`
  - `command`
- Add parity tests that run the same blocks file through JSON and compiled engines and compare observable results.
- Add a bounded benchmark mode (`--duration` or equivalent) to avoid Ctrl+C wrappers.
- Add benchmark metadata: OS, compiler, build type, CPU, commit.
- Split benchmark categories clearly:
  - raw no-expression
  - fast-expression
  - muParser fallback
  - mixed realistic scripts
- Add matching Node/Python/Rust expression-engine benchmarks for fair comparisons.

Bigger next step:

- Introduce a real bytecode/instruction stream for hot execution:
  - typed variable slots
  - expression indexes
  - jump targets
  - source-path side metadata for diagnostics
  - verifier for jumps/slots/expression indexes

Guardrails:

- Preserve full event payloads for `--json`.
- Preserve Ctrl+C exit code `130`.
- Preserve muParser compatibility for expressions outside fast paths.
- Do not make compiled/bytecode the default until parity tests cover normal scripts and command blocks.
