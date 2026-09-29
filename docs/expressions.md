# XBlox Expressions & Variables

How values flow through an XBlox document: `${...}` interpolation, the
expression engine, `PREVIOUS`/`storeAs`, built-in scope variables, system
variables, `${ENV:...}`, known folders, globs, and scoping rules.

For pipe-friendly I/O (`stdout`, `stdin`, `exit`) see [std.md](./std.md).
For *how* a parameter declares which of these behaviors it supports
(kinds, the resolver pipeline, feature composition) see [types.md](./types.md).

---

## The three value languages

XBlox block parameters use three distinct mechanisms. Knowing which one a
field speaks avoids most surprises:

| Mechanism | Where | Example | What it does |
|-----------|-------|---------|--------------|
| **Interpolation** `${var}` | string-ish params (`message`, `path`, `url`, prompts, …) | `"${SRC_DIR}/out-${YYYY}.csv"` | Textual substitution from scope + system variables. |
| **Expressions** | `expression`, `condition`, `initial`/`final`/`modifier` | `"n * 6"`, `"i < 100"` | Numeric evaluation (muParser). Variables are referenced *bare*, without `${}`. |
| **Variable names** | `input`, `valueFrom`/`from`, `variable`, `name` | `"input": "payload"` | The field *is* a variable reference — give it the plain name. |

All three speak **one reference grammar** for nested data:

```text
name                 simple variable
doc.meta.rate        nested object access
doc.items[0].name    array indexing — `doc.items[0]` ≡ `doc.items.0`
doc.items.length     array size pseudo-field (also `.count`)
```

So `${doc.items[0].name}` in a template, `doc.items[0].count > 3` in a
condition, and `"${doc.items[0]} == 2"` (template syntax inside an expression
field — it unwraps) all address the same value the same way.

---

## `${...}` interpolation

Any string parameter that supports interpolation can mix literal text and
variables:

```json
{ "kind": "fsWrite", "path": "${CWD}/report-${YYYY}-${MM}-${DD}.md", "value": "${PREVIOUS}" }
```

Resolution order for `${key}`:

1. `${ENV:NAME}` — environment variable `NAME`.
2. `${KNOWNFOLDER:NAME}` — OS folder (see below).
3. System variables (dates, paths — see below).
4. Scope variables — every top-level value in the run context, including
   `PREVIOUS` and everything written by `storeAs`/`setVariable`.
5. Nested references — keys with dots/brackets walk into objects and arrays:
   `${doc.items[0].name}`, `${PREVIOUS.data[0]}`. Whole arrays/objects are
   not flattened into strings (read them with `Parse` or `valueFrom`).

Unresolved simple keys are left as-is (`${missing}`) and log a warning, so
typos are visible rather than silently empty; unresolved nested paths degrade
to the bare key text (`doc.nope[3]`) with a warning. Whole-number floats
render cleanly (`${n}` prints `42`, not `42.000000`).

Substituted values are **data, not templates** for free-text params (`message`,
prompts, `value`, …): a variable whose value itself contains `${...}` is
inserted literally. **Path params are the exception** — file/directory/output
path inputs resolve deeply (up to 5 levels), so a context variable like
`"ocrInput": "${KNOWNFOLDER:Desktop}/scans/*.png"` works as a reusable path
template on any path field (`fsRead.path`, image/OCR `input`, `outputPath`, …).
Circular references abort the expansion with a warning and leave the value as
written.

## Expressions (muParser)

Fields like `expression` (setVariable), `condition` (if/while), and the `for`
loop's `initial`/`final`/`modifier` are evaluated by a numeric expression
engine — [muParser](https://beltoforion.de/en/muparser/) — with a fast native
path for common shapes (`a + b`, `x < y`, `nowMs - t >= 5000`, …).

- Reference variables **bare**: `score + bonus * 2`, not `${score}` —
  although `${score}` is tolerated and unwraps to the same reference.
- Nested references work directly: `doc.items[0].count > 3`,
  `PREVIOUS.data[0] + PREVIOUS.data[1]`, `doc.items.length`.
- Arithmetic: `+ - * / ^`, parentheses, the usual function set (`min`, `max`,
  `abs`, `sqrt`, `rint`, …).
- Comparisons/logic produce `1`/`0`: `a > b`, `x == 3`, `a < b && c != 0`.
- Booleans participate as `1`/`0` — `flag + 100` works.
- `PREVIOUS` is readable inside expressions: `"expression": "PREVIOUS + 1"`.
- **String equality** with a quoted literal works in any expression field:
  `status == "ready"`, `user.name != "kim"`, `"kim" == user.name` — the
  comparison is resolved against the context up front and becomes `1`/`0`,
  so it composes with numeric terms (`status == "ready" && n > 3`).
- Results are numbers. Beyond `==`/`!=` against a literal, string values are
  **not** expression operands (a bare string variable in a condition falls
  back to truthiness, below).

```json
{ "kind": "setVariable", "name": "fps", "expression": "frames / (nowMs - startMs) * 1000" }
```

### Condition truthiness

`if`/`while` conditions evaluate as an expression first; the result is truthy
when non-zero. If the text is not a valid expression, a fallback applies:
empty, `false`, `0`, `null`, `undefined` are false — anything else is true.
Prefer explicit comparisons (`flag == 1`) over relying on the fallback.

## `PREVIOUS` and `storeAs`

Every block writes its primary result to the implicit variable `PREVIOUS`,
which the next block in the chain can consume — many params (e.g. `Parse`'s
`input`, `llmAgent`'s `prompt`, `stdout`'s `message`) default to it.

Any block can additionally name its result with `storeAs`:

```json
{ "kind": "network", "url": "https://api.example.com/items", "decode": "json", "storeAs": "items" },
{ "kind": "Parse", "parser": "jq", "filter": ".0.name", "input": "items", "storeAs": "firstName" },
{ "kind": "stdout", "message": "${firstName}" }
```

`PREVIOUS` is global — it always survives loop/container boundaries.
Numeric results are mirrored into the expression engine automatically, so a
`stdin parse:number storeAs:n` immediately supports `"expression": "n * 6"`.

`setVariable` is the explicit writer; `getVariable` copies one variable into
another (`name` → `target`); `valueFrom`/`from` on `setVariable` copies any
context value, including non-scalars.

## Built-in scope variables

Seeded into every run's root scope:

| Variable   | Type   | Description |
|------------|--------|-------------|
| `argv`     | array  | Full process argument vector of the run. |
| `argv_str` | string | The same as a single command-line string. |
| `os`       | string | Platform — `win32`, `darwin`, `linux`. |
| `arch`     | string | CPU architecture — `x64`, `arm64`, … |
| `cwd`      | string | Working directory of the run. |
| `epoch`    | int    | Unix epoch seconds at run start. |
| `date`     | string | ISO-8601 timestamp at run start. |
| `nowMs`    | number | **Expression-only** monotonic clock in milliseconds — for durations and rate limiting (`nowMs - lastMs >= 5000`). |

A document can seed its own initial scope through the top-level `context`
object of the blocks-file.

## System variables (interpolation-only)

Date/time of the current run:

| Variable | Example |
|----------|---------|
| `${YYYY}` | `2026` |
| `${MM}` `${DD}` | `06` · `10` |
| `${HH}` `${SS}` | `20` · `07` |

Paths:

| Variable | Description |
|----------|-------------|
| `${CWD}` | Working directory. |
| `${CURRENT_PATH}` | Normalized current directory (or the source file's folder). |
| `${CURRENT_FILE}` / `${CURRENT_FILE_NAME}` | Current input file, when the host provides one. |
| `${CURRENT_SELECTION}` | Selected file(s), when launched from the viewer/host. |
| `${SRC_FILE}` `${SRC_DIR}` `${SRC_NAME}` `${SRC_EXT}` | The source file split into parts. |
| `${PATH_SEP}` | `\` or `/`. |
| `${PATH_LIST_SEP}` | `;` or `:`. |

### `${ENV:...}` — environment variables

```json
{ "kind": "fsList", "path": "${ENV:USERPROFILE}/Downloads/*.png", "storeAs": "shots" }
```

Unset variables stay unresolved (with a warning) rather than expanding to an
empty string.

### `${KNOWNFOLDER:...}` — OS folders

Resolves per-platform standard folders, e.g.:

```json
{ "kind": "fsWrite", "path": "${KNOWNFOLDER:DOCUMENTS}/xblox/run.log", "value": "${PREVIOUS}" }
```

Common names (case-insensitive): `HOME`, `DESKTOP`, `DOCUMENTS`, `DOWNLOADS`,
`PICTURES`, `MUSIC`, `VIDEOS`, `TEMP`, `CONFIG` (the app's config directory),
`DATA`, `CACHE`, `PUBLIC`. Windows additionally exposes `SCREENSHOTS`,
`PROGRAM_FILES`, `PROGRAM_DATA`, `START_MENU`, `STARTUP`, `FONTS`, and more.

### `${PATH_*:varName}` — path decomposition

Decompose any variable that holds a path into its parts. `varName` is the name
of a top-level scope variable (context variable, CLI passthrough arg, or
`storeAs` result):

| Token | Equivalent | Example result |
|---|---|---|
| `${PATH_DIR:f}` | `dirname` | `C:\Users\zx\Desktop\data` |
| `${PATH_NAME:f}` | stem (no extension) | `invoice` |
| `${PATH_EXT:f}` | extension with dot | `.json` |
| `${PATH_BASE:f}` | filename with extension | `invoice.json` |
| `${PATH_ABS:f}` | absolute path (relative resolved against `${CWD}`) | `C:\…\invoice.json` |

```json
{
  "context": { "inputFile": "${KNOWNFOLDER:Desktop}/data/invoice.json" },
  "roots": [
    { "kind": "fsRead",  "path": "${inputFile}", "storeAs": "data" },
    { "kind": "Parse",   "filter": ".total", "storeAs": "total" },
    { "kind": "fsWrite",
      "path": "${PATH_DIR:inputFile}/${PATH_NAME:inputFile}_modified${PATH_EXT:inputFile}",
      "content": "" }
  ]
}
```

`PATH_DIR:` returns `"."` when the variable holds a bare filename with no
directory component. An unresolvable variable (not in scope) leaves the token
literal, just like any other unresolved `${…}`.

Path functions operate on the **string value** of the named variable; they do
not walk dot-paths (`${PATH_DIR:doc.items[0].path}` does not work — use
`setVariable` to extract the path first).

## Globs

File-path parameters that declare glob support expand wildcards **after**
interpolation, producing either a single path or an array of matches:

```json
{ "kind": "fsList", "path": "${ENV:PROJECT_ROOT}/assets/*", "only": "dir", "storeAs": "folders" }
```

- `*` and `?` match within a path segment.
- `fsList` adds `only: "dir" | "file"` type filtering.
- An array result feeds iteration (below) or `storeAs` directly; the event
  reports a `count`.
- Vision/OCR inputs accept globs too — `"input": "shots/*.png"` runs the
  model over each match.

## Iteration over results

Any producer block whose result is an array can carry `items` children — they
run once per element with `PREVIOUS` set to the element and a numeric `index`
in scope:

```json
{ "kind": "fsList", "path": "${CWD}/inbox/*.json", "only": "file",
  "items": [
    { "kind": "Parse", "parser": "jq", "filter": ".name" },
    { "kind": "log", "message": "file ${index}: ${PREVIOUS}" }
  ] }
```

## Scoping rules

- **Loops isolate their locals.** Variables first assigned inside a `for`/
  `while` body are discarded when the loop ends; the `for` counter is gone
  after the loop.
- **Assignments to outer variables stick.** Writing to a variable that
  already exists in an outer scope updates it — `total = total + i` inside a
  loop accumulates as expected.
- **`PREVIOUS` is global.** It always hoists out of any container.
- **`group` is transparent.** It runs children in the current scope —
  variables set inside remain visible after.

```json
{ "kind": "setVariable", "name": "total", "value": 0 },
{ "kind": "for", "variable": "i", "initial": "0", "comparator": "<", "final": "3", "modifier": "+1",
  "items": [ { "kind": "setVariable", "name": "total", "expression": "total + i" } ] },
{ "kind": "stdout", "message": "${total}" }
```

→ prints `3`; `i` no longer exists after the loop.

---

## Current limitations

Expressions:

- **Numeric results only.** The expression engine (muParser) evaluates over
  doubles. String values participate only through `==`/`!=` against a quoted
  literal (resolved up front); there is no string concatenation, ordering
  (`<`/`>`), or string-returning expression. See
  [expressions-next.md](./expressions-next.md) for what may come next.
- **Literal array indices only.** `items[0]` works; `items[i]` (a variable
  index) does not — iterate with `items` children instead.
- **Double precision.** Integers beyond 2^53 lose precision; expression
  results are stored as numbers, not exact 64-bit integers.
- **Condition fallback is blunt.** When a condition is not a valid
  expression, the truthiness fallback treats any non-empty string except
  `false`/`0`/`null`/`undefined` as true. Always prefer a real expression
  (`flag == 1`, `${flag}` unwraps to the same thing).

Variables & interpolation:

- **Unresolved keys stay literal.** `${typo}` passes through with a warning —
  downstream blocks see the literal text, not an error. Unresolved nested
  paths (`${doc.nope[3]}`) degrade to the bare key text.
- **Scope-frame tracking covers simple names.** Only plainly-named numeric
  variables participate in loop-frame isolation; dotted or exotic names live
  in the flat context only.
- **`nowMs` is expression-only** — it is not available to `${...}`
  interpolation, and it is a monotonic clock (not wall time; use `epoch`/
  `date` for timestamps).

Globs:

- **`*`, `?`, `**` only.** No brace expansion (`{a,b}`), no character
  classes (`[0-9]`), no negation. `*`/`?` match within one path segment;
  `**` spans directories.
- **No matches is not an error** — the result is an empty array; guard with
  a count check if emptiness should fail the flow.
- **The literal base directory must exist**; a missing base also yields an
  empty array rather than an error.

Scoping:

- **Scope policy is per-block-kind, not user-selectable.** Loops isolate,
  `group` inherits — a document cannot currently choose a policy (e.g.
  collect) per container.
- **`PREVIOUS` is always global** — there is no way to sandbox it inside a
  container.

## Quick reference

```text
${name}                interpolation: scope/system variable into a string
${doc.items[0].name}   interpolation: nested reference (dots/brackets)
${ENV:NAME}            environment variable
${KNOWNFOLDER:NAME}    OS standard folder
${PATH_DIR:var}        parent directory of var's path value
${PATH_NAME:var}       stem (no extension)
${PATH_EXT:var}        extension with dot
${PATH_BASE:var}       filename with extension
${PATH_ABS:var}        absolute path (relative resolved against ${CWD})
name (bare)            in expression/condition fields: numeric variable
doc.items[0].count     nested reference in expressions (≡ doc.items.0.count)
doc.items.length       array size pseudo-field
status == "ready"      string equality in conditions (either side quoted)
PREVIOUS               implicit result of the previous block (global)
PREVIOUS.data[0]       nested access into the previous result
storeAs                name a block's result for later blocks
nowMs                  monotonic ms clock (expressions only)
*.ext / dir/*          globs on file-path params (after interpolation)
Parse filter (jq)      .items[] | select(.active) | .name
```

---

## The `Parse` block and jq filters

The `Parse` block ships with **real libjq** (jqlang/jq 1.8) built in. Any
filter expression accepted by the `jq` command-line tool works directly.

```json
{ "kind": "Parse", "input": "payload", "filter": ".user.name", "storeAs": "name" }
```

`input` defaults to `PREVIOUS`. `filter` defaults to `.` (identity). `storeAs`
names the result for later blocks.

### Worked examples

Given a context variable `doc`:
```json
{
  "data": [2, 5, 8],
  "meta": { "rate": 1.5, "label": "metrics" }
}
```
and `user`:
```json
{ "name": "kim", "age": 30 }
```

**Field access — simple dot-path (fast path, no libjq overhead):**
```json
{ "kind": "Parse", "input": "doc", "filter": ".meta.label", "storeAs": "label" }
```
→ `label = "metrics"`

**Arithmetic:**
```json
{ "kind": "Parse", "input": "doc", "filter": ".meta.rate * 2", "storeAs": "doubleRate" }
```
→ `doubleRate = 3`

**Map over array:**
```json
{ "kind": "Parse", "input": "doc", "filter": ".data | map(. * 2 | tostring) | join(\", \")", "storeAs": "doubled" }
```
→ `doubled = "4, 10, 16"`

**Select / filter:**
```json
{ "kind": "Parse", "input": "doc", "filter": "[.data[] | select(. > 4) | tostring] | join(\", \")", "storeAs": "big" }
```
→ `big = "5, 8"`

**String interpolation (jq syntax):**
```json
{ "kind": "Parse", "input": "user", "filter": "\"hello, \\(.name)! age=\\(.age)\"", "storeAs": "greeting" }
```
→ `greeting = "hello, kim! age=30"`

**Keys / length:**
```json
{ "kind": "Parse", "input": "user",  "filter": "keys | join(\", \")", "storeAs": "userKeys" }
{ "kind": "Parse", "input": "doc",   "filter": ".data | length",       "storeAs": "dataLen" }
```
→ `userKeys = "age, name"` · `dataLen = 3`

**Alternative operator (null/false default):**
```json
{ "kind": "Parse", "input": "doc", "filter": ".meta.missing // \"fallback\"", "storeAs": "val" }
```
→ `val = "fallback"`

**Sum / reduce:**
```json
{ "kind": "Parse", "input": "doc", "filter": ".data | add", "storeAs": "total" }
```
→ `total = 15`

**Collect → feed iteration:**
```json
{ "kind": "network", "url": "https://api.example.com/items", "decode": "json", "storeAs": "items" },
{ "kind": "Parse",   "input": "items", "filter": "[.[] | select(.active)]", "storeAs": "active" },
{ "kind": "for", "variable": "i", "initial": "0", "comparator": "<", "final": "active.length", "modifier": "+1",
  "items": [
    { "kind": "Parse", "input": "active", "filter": ".[${i}].name", "storeAs": "nm" },
    { "kind": "stdout", "message": "  ${i}: ${nm}" }
  ] }
```

### Filter patterns

| Pattern | Example |
|---------|---------|
| Field access | `.user.name` · `.meta.rate` |
| Nested access | `.a.b.c` · `.items[0].title` |
| Array iteration | `[.items[] \| .name]` |
| Select | `[.items[] \| select(.active)]` |
| Map | `.data \| map(. * 2)` |
| Arithmetic | `.count + 1` · `.a - .b` |
| String interpolation | `"\(.name) — \(.title)"` |
| Builtins | `keys` · `length` · `add` · `type` |
| Type conversion | `.val \| tonumber` · `.n \| tostring` |
| Alternative | `.label // "unknown"` |
| Slicing | `.items[2:5]` |
| Reduce | `.data \| add` · `[.[] \| . * .]  \| add` |
| Recursive descent | `.. \| numbers` |

### Notes

**Simple dot-paths bypass libjq** — `.foo.bar[0]` is handled by the built-in
path resolver without spinning up the jq runtime. A path is "simple" when it
starts with `.` and contains only alphanumerics, `_`, `.`, and `[0-9]`.

**Multiple outputs:** `".items[]"` produces one value per element; only the
first is captured. Wrap in `[...]` to collect all:
`"[.items[] | select(.active) | .name]"`.

**Displaying array/object results in `${...}` templates:** `${...}`
interpolation only expands scalars cleanly. If a filter returns an array or
object, pipe through `join` or `tostring` before `storeAs` to get a
printable string:

```json
{ "kind": "Parse", "filter": ".data | map(tostring) | join(\", \")", "storeAs": "readable" }
```

Then `${readable}` expands to e.g. `2, 5, 8`.

**Disabled builtins** (oniguruma not vendored):
`test/1`, `match/1`, `scan/1`, `sub/2`, `gsub/2` — regex operations.

**Error messages** from jq compilation or evaluation surface as block errors
in the event stream with `status: "error"`. The flow continues (errors are
non-fatal at block level) but the `storeAs` variable is not written.

See [jqlang.org](https://jqlang.org) for the complete filter reference.
