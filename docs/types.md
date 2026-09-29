# XBlox Parameter Types & Feature Composition

How a block declares its parameters — and how the executor turns those
declarations into finished, typed values before a handler ever runs.

Source surface: `src/xblox/blocks/block_params.hpp` (ParamDef + builders),
`block_types.hpp` (ParamKind, Value), `block_validation.hpp` (ParamConstraint),
`block_resolver.hpp` / `builtin_blocks.cpp` (the resolver that executes it all).

User-facing behavior that falls out of this system — `${var}` interpolation,
globs, `PREVIOUS` sourcing — is documented in [expressions.md](./expressions.md).

---

## The idea: one kind, composable features

A parameter is **not** described by inventing a new type for every behavior.
It is described by one base `ParamKind` (what the value *is*) plus orthogonal,
composable declarations layered on top:

```
ParamKind            what it is        file_path, integer, enum, prompt, …
  + sourcing         where it comes from   field → PREVIOUS / variable → default
  + ParamResolve     how it is expanded    variables ∘ globs ∘ custom hooks
  + ParamConstraint  what must hold        nonEmpty, mustExist, writable, …
  + range/enums      how it is validated   clamp, allowed values
  + group/ui         how it is presented   input/output/options/advanced, hints
```

A single declaration says all of it fluently:

```cpp
pd_file("input").in().req().prev()
    .globs()                      // feature: glob expansion on top of file_path
    .must_exist()                 // constraint: path must exist + be readable
    .desc("Image to analyse. Leave empty to use PREVIOUS.")
```

That one line means: *file-path input, required, sources `PREVIOUS` when the
field is empty, interpolates `${var}`, expands `*`/`?`/`**` into a path array,
and the run fails the block early — with a structured diagnostic — if the path
does not exist.* The handler then reads a finished value and contains zero
resolution logic.

## ParamKind — the base vocabulary

`ParamKind` drives the JSON cast, the default resolution features, and the UI
widget. Manifest token in parentheses.

| Group | Kinds |
|-------|-------|
| Primitives | `string_` (string) · `integer_` · `float_` · `boolean_` · `expression_` |
| Paths | `file_path` · `dir_path` · `output_path` · `audio_path` · `image_path` · `video_path` · `screen_input` |
| Structured | `json_value` · `args_list` · `enum_` · `flags` · `variable_ref` |
| Rich | `color_` · `duration_ms` · `device_name` · `video_input` · `prompt` |

Notes:

- The media path kinds (`image_path`, …) are `file_path` with a UI filter —
  same resolution behavior, better picker.
- `expression_` is **deliberately not interpolated** — it has its own
  evaluator (muParser). `"condition": "n < 3"` reads `n` itself.
- `variable_ref` carries a *name*, so value-sourcing is skipped for it (a
  `storeAs` param must stay `"storeAs": "result"`, not become the value of a
  variable called `result`).
- `flags` is an integer bitmask whose options are declared per-bit with
  `.flag(value, label, description)` — the manifest carries the full legend.
- `duration_ms` is an integer with ms semantics (and a `.range()` in practice).

Factories: `pd_str`, `pd_int`, `pd_float`, `pd_bool`, `pd_expr`, `pd_file`,
`pd_dir`, `pd_outpath`, `pd_image`, `pd_audio`, `pd_video`, `pd_json`,
`pd_args`, `pd_enum`, `pd_flags`, `pd_ms`, `pd_dev`, `pd_prompt`,
`pd_varref`, … plus shared cross-block params like `pd_store_as()`,
`pd_provider()`, `pd_model()`, `pd_api_key()`.

## The resolver pipeline

For every declared param, `resolve_block_inputs` runs this fixed sequence
*before* the handler, producing the typed input frame (`ResolvedInputs`) that
`BlockIO` reads:

```
1. SOURCE      explicit field → declared source (PREVIOUS / variable) → default
2. TRANSFORM   kind-specific cast + enum check + range clamp     → Diagnostics
3. RESOLVE     features, in order: Variables → Globs → Custom    (composition)
4. CONSTRAIN   declared preconditions (existence, writability…)  → Diagnostics
```

### 1 · Sourcing

```cpp
pd_str("message").prev().dflt("PREVIOUS")   // log/stdout: field → PREVIOUS → default
pd_str("input").from_var("payload")          // explicit variable source
```

An explicit, non-empty field always wins. An **empty string on a sourced
param defers to the source** — that is the documented "leave empty to use
PREVIOUS" convention. If the source is also empty, the declared default
applies.

### 2 · Transform (cast + validate)

Casts are tolerant and diagnostic-driven, never throwing:

- `integer_`/`float_`: accepts numbers, booleans (`1`/`0`), numeric strings.
  `.range(min, max)` **clamps** out-of-range values and records a severity-1
  warning — the block still runs.
- `boolean_`: accepts `true/false`, numbers, and `"true"/"1"/"yes"/"on"`.
- `enum_`: a value outside `enum_values` records a warning (severity 1).
- `json_value`/`args_list`: passed through opaque — handlers open them via
  `io.json()`.
- A wrong, uncastable shape records a warning and leaves the param unset, so
  the handler sees its accessor default.

### 3 · Resolve — feature composition

This is the composition layer (`ParamResolve` bitmask). Features are
*additive declarations on the same param*, applied left-to-right:

| Feature | Declared by | Effect |
|---------|-------------|--------|
| `Variables` | **implicit** on every string-shaped kind (string, paths, color, device, prompt) | `${var}` / `${ENV:..}` / `${KNOWNFOLDER:..}` expansion, so the frame holds final values. |
| `Deep` | **implicit** on every path kind (`file_path`, `dir_path`, media paths, `output_path`); `.deep()` opts other params in | Re-expand `${..}` tokens that arrive *inside* substituted variable values (a context variable holding `"${KNOWNFOLDER:Desktop}/scans"`), up to 5 passes. Circular references abort the expansion with a warning and leave the value as written. Path kinds are *locations* — document-authored templates — so they expand fully; free-text kinds (`string_`, `prompt`) stay single-pass on purpose, because they carry untrusted runtime data (`${PREVIOUS}` passthroughs) that must remain data, not become templates. |
| `Globs` | `.globs()` | After interpolation, expand `*` `?` `**` into a **sorted JSON array** of matching paths. A pattern with no glob tokens stays a scalar. No matches = empty array, not an error. |
| `Custom` | `.guard()` | Run app-level resolve hooks (policy/guard/redirect) registered in `ExecutionOptions::resolve_hooks` — after variables and globs, so hooks see final values. |

Composition in practice — `fsList.path` declares all three behaviors on one
`file_path`:

```cpp
pd_file("path").in().req().globs().guard()
```

```json
{ "kind": "fsList", "path": "${ENV:PROJECT_ROOT}/assets/*", "only": "dir" }
```

resolves as: interpolate `${ENV:PROJECT_ROOT}` → expand the glob into a path
array → let policy hooks veto/redirect individual entries. The handler
receives the final array via `io.value("path")`.

Hook veto semantics differ by shape on purpose: a **scalar** veto is a
severity-2 diagnostic (block skipped); a vetoed **glob entry** is filtered
out as a severity-1 advisory so the remaining matches still flow.

Excluded from implicit interpolation: `expression_` (own evaluator),
`variable_ref` (a name), `enum_` (fixed set), structured kinds.

### 4 · Constraints — declared preconditions

`ParamConstraint` gives a param *teeth*: the resolver probes the resolved
value and a violated precondition (severity 2) makes the executor **skip the
handler** and emit a structured error event — the failure is predicted, not
stumbled into.

| Builder | Constraint(s) | Meaning |
|---------|---------------|---------|
| `.nonempty()` | `NonEmpty` | resolved string must be non-empty (gives `.req()` teeth) |
| `.must_exist()` | `MustExist \| Readable` | path exists and is readable |
| `.writable()` | `Writable` | path (or its parent) is writable |
| `.as_file()` / `.as_dir()` | `IsFile` / `IsDir` | existing path has the right type |
| `.create_parents()` | `CreateParents` | missing parent dirs are created (the one *effectful* constraint — suppressed under `--simulate`, where it is only predicted) |
| `.constrain(Absolute)` | `Absolute` | advisory (severity 1) |
| *(reserved)* | `RequiresPermission` | high bits reserved for host-mediated / async capability gates |

All probes are cheap and read-only (except `CreateParents`), so they run in
both normal and simulate mode — `--simulate` predicts precondition failures
without touching disk.

### Diagnostics

Every finding is a structured `Diagnostic { param, message, severity }`:

- **severity 1** — advisory: clamps, enum coercion, filtered glob entries.
  Attached to the block's event; the handler still runs.
- **severity 2** — violated precondition: error event, handler skipped,
  normal error policy applies (`continueOnError` etc.).

Carrying the param name (instead of a bare string) lets the UI group issues
per field and lets strict handling key off severity, not string matching.

## Presentation metadata

Orthogonal to resolution, for UI/host consumption only:

- **Groups**: `.in()` input · `.out()` output · `.opt()` options (default) ·
  `.adv()` advanced · `.grp("custom")`.
- `.lbl()`, `.desc()` — label and help text.
- `.hide()` — not shown in the palette UI.
- `.plat(mask)` — platform availability.
- `.ui({...})` — declarative widget hints (e.g. a select fed from
  `audio.devices.input`, as `pd_audio_input_device()` does).
- `.dyn({...})` — dynamic schema metadata for provider/model-specific option
  objects; advisory for sinks that can render it.

## The manifest

Everything declared above is serialized — nothing is invented by a frontend.
`tanit-cli xblox info --json` dumps each block with its `params[]`
(kind, default, enum values, ranges, `constraints: ["mustExist", ...]`,
`resolve: ["globs", ...]`, `uses_previous`, groups) plus `execFlags` and
`childLists`. The UI palette, the LLM tool surface, and the test harness all
read this same manifest, which is why a param's behavior is testable without
running the block:

```js
const decl = info.blocks.find(b => b.kind === 'stdin')
             .params.find(p => p.name === 'parse');
// decl.enum_values → ["text","json","number","boolean","auto"]
```

## What handlers see

Handlers never re-implement any of the above. `BlockIO` reads the
pre-resolved frame:

```cpp
const std::string mode = io.str("mode", "all");   // sourced + cast + validated
const long long   code = io.i64("code", 0);       // range-clamped already
const bool        trim = io.flag("trim", true);
const Value*      paths = io.value("path");       // glob array, if .globs()
```

If a param has no declared schema (undeclared fields like `id`), the
accessors fall back to a lazy field → source → default lookup, so migration
is incremental — but the goal is that every block declares its full surface
and the handler body contains only the block's actual work.

---

## Worked example

The `exit` block's full declaration:

```cpp
registry["exit"] = bdb(
    "exit", exit_block, "Exit", "Flow",
    "Stop the run immediately and set the process exit code.",
    {{"kind", "exit"}, {"code", 0}})
    .params({
        pd_int("code").in().dflt(0).range(0, 255)
            .desc("Process exit code (0-255). ..."),
        pd_str("message").in().dflt("")
            .desc("Optional message recorded on the exit event."),
    })
    .pure();   // ExecFlags: side-effect free → still runs under --simulate
```

From this single declaration the system derives: the JSON cast (`"code": "7"`
works, `"code": 999` clamps to 255 with a warning), the palette widget (number
spinner, input group), the manifest entry the harness asserts against, and the
typed `io.i64("code", 0)` read in the handler.

---

## Current limitations

Sourcing & kinds:

- **`ParamSource::Expression` is declared but not implemented** —
  `.from_expr()` exists on the builder, but the resolver does not evaluate
  expression sources yet. Use `.from_var()` or `.prev()`.
- **Undeclared params bypass the pipeline.** Fields with no `ParamDef` fall
  back to a lazy field lookup: no cast, no clamp, no constraints, no
  manifest entry. Migration is incremental — some blocks (`app_blocks`,
  `modbus_blocks`) still declare legacy raw-JSON params.
- **One source per param.** A param sources *either* PREVIOUS *or* one named
  variable — there is no fallback chain across multiple variables.

Validation:

- **Clamps and enum mismatches are advisories, not failures.** `.range()`
  clamps with a severity-1 diagnostic and the block still runs; an enum
  value outside the allowed set warns and passes through. There is no
  declarative "strict" mode that promotes these to severity 2.
- **`Absolute` is advisory** (severity 1); `RequiresPermission` is a
  reserved bit — the async/host capability gate that would enforce it does
  not exist yet.
- **Constraint probes are filesystem-only.** Existence/type/writability
  cover paths; there are no declarative constraints for URLs, ports, or
  device availability — handlers still check those themselves.
- **`Writable` is a heuristic** (owner-write permission bit on existing
  paths); it does not attempt an actual open, so ACL-denied paths can pass
  the probe and fail in the handler.

Resolution features:

- **Feature order is fixed** (Variables → Globs → Custom) and not
  declarable per-param.
- **Glob expansion is filesystem-only** — `*`/`?`/`**`, no braces or
  character classes, and only on path-shaped params that opt in with
  `.globs()`. No matches (or a missing base directory) yields an empty
  array, not an error.
- **Custom hooks are synchronous and first-veto-wins.** A hook cannot defer
  to an async host decision, and later hooks do not see a vetoed value.

Execution:

- **The compiled fast path covers only `setVariable`/`if`/`while`/`log`.**
  Any other kind — or a block using `storeAs`/`continueOnError`/`runFlags`/
  `onError` — drops the whole tree back to the interpreted path. Correctness
  is identical; only throughput differs.
- **The typed frame is rebuilt per execution.** Param resolution is cheap
  but not cached across loop passes; extremely hot loops should keep their
  param surfaces small.
