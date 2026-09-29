# XBlox Standard I/O — `stdout`, `stdin`, `exit`

XBlox flows can behave like proper command-line citizens: read from a pipe,
write to a pipe, and finish with a meaningful exit code. Three blocks cover
this — `stdout`, `stdin`, and `exit` — together with two CLI switches that keep
the streams clean (`--log-level off`, and omitting `--json`).

Variable syntax (`${var}`), expressions, and the built-in scope variables used
in the examples below are documented in [expressions.md](./expressions.md).

---

## The pipe contract

```
producer | tanit-cli xblox --log-level off run --src flow.xblox | consumer
```

- **stdout** carries only what your flow explicitly writes with `stdout`
  blocks (plus the JSON report if you pass `--json`).
- **stderr** carries logging (`log` blocks, runtime warnings, model token
  streaming). `--log-level off` silences all of it.
- **exit code** is `0` on success, non-zero on failure — or whatever an
  `exit` block decides.

Rule of thumb: use `log` while designing and debugging a flow, switch the
final output to `stdout` for production piping.

---

## `stdout` — write to standard output

Writes a message directly to stdout, bypassing the logging system entirely.
Pipe-friendly: unaffected by `--log-level`.

| Param     | Type   | Default    | Description |
|-----------|--------|------------|-------------|
| `message` | string | `PREVIOUS` | Message, `${var}` interpolation, variable name, or expression. Empty = dump the whole scope. |
| `newline` | bool   | `true`     | Append a newline. Set `false` to build a line from several blocks. |

`message` resolution order: `${...}` interpolation first, then a bare variable
name lookup (`"vision"` prints the variable `vision`), then expression
evaluation (`"n * 2"` prints the computed number). Plain text passes through.

```json
{ "kind": "stdout", "message": "${visionCsvText}" }
```

```json
{ "kind": "stdout", "message": "count=", "newline": false },
{ "kind": "stdout", "message": "${count}" }
```

The default `PREVIOUS` makes the chained form trivial:

```json
{ "kind": "Parse", "parser": "jq", "filter": ".total" },
{ "kind": "stdout" }
```

## `stdin` — read piped input

Reads standard input — the whole stream or one line — parses it into a typed
primitive, and stores it in `PREVIOUS` / `storeAs`.

| Param     | Type | Default | Description |
|-----------|------|---------|-------------|
| `mode`    | enum | `all`   | `all` = read the entire stream to EOF · `line` = read the next line (streamable inside loops). |
| `parse`   | enum | `text`  | `text` · `json` · `number` · `boolean` · `auto` — see below. |
| `trim`    | bool | `true`  | Trim trailing whitespace/newlines before parsing. |
| `storeAs` | string | —     | Variable to store the result in (always sets `PREVIOUS`). |

### Parse modes

| `parse`   | Result type | Notes |
|-----------|-------------|-------|
| `text`    | string      | Raw input as-is (after trim). |
| `json`    | object / array / scalar | Feeds JSON-oriented blocks (`Parse` jq filters, …). Malformed JSON is a block error. |
| `number`  | integer or float | Lands in the numeric store, so expressions can use it directly (`n * 6`). Non-numeric input is a block error. |
| `boolean` | bool        | Accepts `true/1/yes/on` and `false/0/no/off`, case-insensitive. |
| `auto`    | detected    | JSON-scalar detection: `123` → number, `true` → bool, `{...}` → object; anything else stays a string. |

Binary input is **fenced off**: any NUL byte fails the block with
`stdin: binary input is not supported`. Pipe text (UTF-8) only.

```json
{ "kind": "stdin", "parse": "json", "storeAs": "payload" },
{ "kind": "Parse", "parser": "jq", "filter": ".items.0.name", "input": "payload" },
{ "kind": "stdout" }
```

```json
{ "kind": "stdin", "parse": "number", "storeAs": "n" },
{ "kind": "setVariable", "name": "doubled", "expression": "n * 2" },
{ "kind": "stdout", "message": "${doubled}" }
```

`mode: "line"` returns a clean error at EOF instead of hanging — a `while`
loop reading lines stops when the upstream producer closes the pipe.

## `exit` — stop with an explicit exit code

Stops the run immediately and makes `code` the authoritative process exit
code. Blocks after the `exit` (including in outer containers) do not run.

| Param     | Type | Default | Description |
|-----------|------|---------|-------------|
| `code`    | int  | `0`     | Process exit code, 0–255. |
| `message` | string | —     | Optional message recorded on the exit event. |

Standard meanings — `0` success, `1` general error, `2` misuse/invalid input,
`126` not executable, `127` not found, `130` interrupted (Ctrl+C). Anything
else is yours for app-specific signalling to pipelines:

```json
{ "kind": "stdin", "parse": "number", "storeAs": "value" },
{ "kind": "if", "condition": "value < 0",
  "consequent": [ { "kind": "exit", "code": 65, "message": "negative input not allowed" } ] },
{ "kind": "stdout", "message": "${value}" }
```

`exit` works at any nesting depth — an `exit` inside an `if` inside a `while`
unwinds the whole run:

```json
{ "kind": "setVariable", "name": "i", "value": 0 },
{ "kind": "while", "condition": "i < 100", "loopLimit": 200, "items": [
    { "kind": "setVariable", "name": "i", "expression": "i + 1" },
    { "kind": "if", "condition": "i == 3",
      "consequent": [ { "kind": "exit", "code": 5 } ] }
] },
{ "kind": "stdout", "message": "never reached" }
```

An `exit 0` also overrides earlier non-fatal errors (`continueOnError` runs):
the deliberate code wins. A document-level `loop` stops on `exit` as well.

---

## CLI recipes

Clean production run — stdout is exactly what the flow prints:

```powershell
tanit-cli.exe xblox run --log-level off --src .\flow.xblox
```

Debugging — full event report on stdout, logs on stderr:

```powershell
tanit-cli.exe xblox run --log-level debug --json --src .\flow.xblox
```

Pipe two flows together (producer prints a number, consumer adds to it):

```powershell
# producer.xblox: setVariable lhs=35 → stdout ${lhs}
# consumer.xblox: stdin parse:number storeAs:n → setVariable sum = n + 7 → stdout ${sum}
tanit-cli.exe xblox run --log-level off --src producer.xblox |
  tanit-cli.exe xblox run --log-level off --src consumer.xblox
# → 42
```

Mix with ordinary shell tools:

```powershell
Get-Content data.json | tanit-cli.exe xblox run --log-level off --src extract.xblox > result.txt
echo $LASTEXITCODE   # whatever the flow's exit block decided
```

Vision-to-CSV, piped onward (see `tests/xblox/vision-csv.xblox`):

```powershell
tanit-cli.exe xblox run --log-level off --src .\tests\xblox\vision-csv.xblox | my-csv-import
```

### Behavior notes

- Without `--json`, events are still counted and errors still drive the exit
  code — only the report payload is suppressed. `stdout`/`stdin`/`exit`
  behave identically in both modes.
- With `--json`, the trailing JSON report is printed to stdout *after* any
  `stdout` block output; the report's `ok`/`exitCode` mirror the process
  exit code.
- `--log-level off` also silences `log` blocks and VLM token streaming, so a
  production pipe stays byte-clean on both streams.

### Testing

The `std` suite of the regression harness covers all of the above, including
a real two-process pipe:

```powershell
npm run test:xblox-next -- --only std
```

---

## Current limitations

- **Binary I/O is fenced off.** `stdin` rejects any input containing a NUL
  byte; there is no raw-bytes mode yet. Pipe text (UTF-8) only — base64-wrap
  binary payloads upstream if you must move them through a flow.
- **Text-mode line endings on Windows.** stdout is opened in text mode, so
  `\n` becomes `\r\n` on Windows. Fine for text pipelines; byte-exact
  consumers should normalize (the test harness does).
- **One stdin, consumed once.** `stdin mode:"all"` drains the stream; a
  second `all` read yields an empty string, and a `line` read after EOF is a
  block error. There is no rewind.
- **`line` mode blocks without a timeout.** A `stdin` block waits on the
  pipe until a line or EOF arrives; an idle upstream producer stalls the
  flow (Ctrl+C still cancels). No `timeoutMs` yet.
- **No `stderr` block.** Diagnostics belong to `log` (which writes to the
  logging system on stderr); there is no block for raw, unformatted stderr
  output.
- **`exit` codes are clamped to 0–255.** POSIX-compatible by design; values
  outside the range clamp with a warning rather than failing. Windows-wide
  32-bit exit codes are not exposed.
- **`exit` stops the run, not spawned work.** Detached/background jobs
  started by other blocks are not awaited or killed by `exit`.
- **`--json` shares stdout.** The report prints to stdout after any `stdout`
  payload — a byte-clean pipe means running without `--json` (events are
  still counted and errors still set the exit code).
