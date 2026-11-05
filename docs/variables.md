# XBlox Variables

XBlox keeps a per-run JSON scope. Blocks read and write that scope through
`setVariable`, `getVariable`, `storeAs`, `target`, `PREVIOUS`, loop variables,
and block-specific outputs.

There are three related lookup modes:

- Scope lookup reads JSON values by name, including dot paths such as
  `result.stdout`.
- Expressions evaluate numeric formulas such as `count + 1` or `epoch > 0`.
- String interpolation replaces `${name}` placeholders in path, shell, log, and
  other string fields that opt into variable resolution.

## Default Scope Variables

These variables are added before each XBlox run starts. Document `context`
values are loaded first, then defaults are applied, so these reserved names are
owned by the runtime.

| Name | Type | Description |
| --- | --- | --- |
| `argv` | array of strings | Full process argument vector, including `argv[0]`. For CLI runs this is the executable path followed by arguments such as `xblox`, `run`, `--src`, and the document path. |
| `argv_str` | string | Full process command line string. On Windows this uses the original `GetCommandLineW()` text; on other platforms it is reconstructed from `argv`. |
| `os` | string | Node.js-style platform name: `win32`, `darwin`, `linux`, `freebsd`, `openbsd`, `netbsd`, or `unknown`. |
| `arch` | string | Node.js-style CPU architecture: `x64`, `arm64`, `arm`, `ia32`, `ppc64`, `s390x`, `riscv64`, or `unknown`. |
| `cwd` | string | Effective working directory for the run. CLI runs use the resolved default cwd passed into XBlox; other hosts fall back to the process current directory. |
| `epoch` | integer | Current Unix epoch time in seconds when the run scope is prepared. |
| `date` | string | UTC ISO timestamp for the same instant as `epoch`, formatted like `2026-06-08T08:27:08Z`. |
| `PREVIOUS` | any | Implicit chained result. Most value-producing blocks set this to their result; `setVariable` and `getVariable` also update it. |
| `nowMs` | number | Expression-only monotonic clock in milliseconds. It is available in expressions but is not stored as a JSON scope value. |

## CLI Args

`xblox run` accepts arbitrary extra `--name` arguments after known options. They
are parsed into scope before execution:

```shell
pm-image-cli xblox run --src flow.json --threshold 0.75 --label cat --debug
```

This injects:

```json
{
  "threshold": 0.75,
  "label": "cat",
  "debug": true,
  "args": {
    "threshold": 0.75,
    "label": "cat",
    "debug": true
  }
}
```

Accepted forms:

- `--name value`
- `--name=value`
- `--flag`

Values are coerced to JSON scalars:

- `true` and `false` become booleans.
- Integers become JSON integers.
- Decimal numbers become JSON numbers.
- Everything else remains a string.

Non-`--` stragglers are ignored by this parser. CLI arg values override the
document's stored `context` values with the same names. The complete parsed map
is also stored as `args`.

Do not confuse this with the `--arg` option. `--arg` appends values to
`cliCommand` / `external` command invocations; it does not create named scope
variables.

## Document Context

A document can seed scope with a top-level `context` object:

```json
{
  "version": 1,
  "context": {
    "seed": 3,
    "name": "demo"
  },
  "roots": [
    { "kind": "setVariable", "name": "score", "expression": "seed + 4" }
  ]
}
```

Top-level values are available to scope lookup, string interpolation, and numeric
expressions when they can be interpreted as numbers.

## Reading And Writing Variables

`setVariable` writes a value to scope and updates `PREVIOUS`.

```json
{ "kind": "setVariable", "name": "score", "expression": "seed + 4" }
```

`setVariable` chooses its source in this order:

1. `valueFrom` or `from`: copy another scope value.
2. `expression`: evaluate a numeric expression.
3. `value`: store a literal JSON value.

`getVariable` reads a value and can copy it to `target`; it also updates
`PREVIOUS`.

```json
{ "kind": "getVariable", "name": "score", "target": "copiedScore" }
```

Many blocks also support `storeAs` or `target` to save their result under a
named variable. Block results conventionally update `PREVIOUS`.

## Expressions

Expression fields are numeric. They use a fast path for simple expressions and
fall back to muParser for compound formulas.

Examples:

```json
{ "kind": "setVariable", "name": "sum", "expression": "a + b" }
{ "kind": "if", "condition": "score >= 10", "consequent": [] }
{ "kind": "while", "condition": "nowMs - startedMs < 1000", "items": [] }
```

Expression notes:

- Numeric JSON values, booleans, and numeric strings can be used as numbers.
- Simple top-level variable names are bound into the current scope frame.
- Container scopes can shadow outer numeric variables.
- `PREVIOUS` is global and intentionally not frame-scoped.
- `nowMs` is available as a dynamic monotonic millisecond clock.
- Dot-path lookup such as `result.exitCode` can resolve JSON scope values for
  numeric reads.
- Non-numeric values do not participate in arithmetic unless they can be parsed
  as numbers.

Conditions are truthy when their numeric expression evaluates to a non-zero
value. If a condition is not a valid expression, `false`, `0`, `null`, and
`undefined` are false-like strings; other non-empty strings are truthy, with
special handling for selection-related conditions.

## String Interpolation

String fields that opt into variable resolution can use `${...}` placeholders:

```json
{ "kind": "log", "level": "info", "message": "cwd=${cwd} os=${os}" }
```

Interpolation combines:

- System command variables from the host command context, such as date/CWD and
  selection variables.
- Environment variables with `${ENV:NAME}`.
- Known folders with `${KNOWNFOLDER:Name}`.
- Scalar top-level XBlox scope values, such as `${PREVIOUS}`, `${score}`,
  `${cwd}`, `${os}`, `${epoch}`, and `${date}`.

Only scalar top-level JSON scope values are flattened for interpolation. Arrays
and objects are not expanded as `${name}` strings; use `getVariable`, JSON-aware
blocks, or dot-path reads where appropriate.

Expression fields do not use `${...}` interpolation. Write `score + 1`, not
`${score} + 1`, in an expression.

## Logging The Scope

The `log` block has a few useful variable behaviors:

- Omit `message` and `value` to log `PREVIOUS`.
- Set `message` to an empty string to log the whole JSON scope.
- Set `message` to a bare variable name, such as `score`, to log that value.
- Set `message` to a numeric expression, such as `score + 1`, to log the result.
- Use `${...}` inside `message` for string interpolation.

## Reserved Names

Avoid using these names for ordinary user variables unless you intentionally want
to replace runtime-managed values:

- `PREVIOUS`
- `argv`
- `argv_str`
- `os`
- `arch`
- `cwd`
- `epoch`
- `date`
- `args`
- `nowMs` in expressions
