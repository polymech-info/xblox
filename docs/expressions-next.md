# Expressions — Next: one reference grammar

Status and follow-up plan for unifying how XBlox flows reference data.
Supersedes the earlier "strings in muParser" plan: instead of teaching the
parser strings first, we removed the *two-standards problem* — string-literal
template keys vs muParser variable names — by giving templates, expressions,
and conditions **one reference grammar**, implemented in
`src/xblox/xblox_expressions.{hpp,cpp}`.

See [expressions.md](./expressions.md) for the user-facing reference.

---

## The grammar (shipped)

```
reference := name ( '.' segment | '[' digits ']' )*
segment   := name | digits
```

- `doc.items[0].name`, `doc.items.0.name` — equivalent spellings; `[n]` and
  `.n` are the same segment separator.
- Arrays expose `length` / `count` pseudo-fields: `doc.items.length`
  (the legacy flattened spellings `items_count` / `items_length` still
  resolve for compatibility).
- Works identically in **all three value languages**:

| Context | Example | Mechanism |
|---|---|---|
| Template field | `message: "first=${doc.data[0]}"` | `expr::interpolate_references` pre-pass before the flat `${VAR}` resolver |
| Expression field | `expression: "doc.data[0] + rate * 2"` | reference → synthetic muParser slot, resolved per eval |
| Condition | `condition: "PREVIOUS.data[0] == 2"` | same as expressions |
| Condition + template syntax | `condition: "${doc.data[0]} == 2"` | `${ref}` unwraps to the bare reference first |

String equality rides along: `status == "ready"`, `user.name != "kim"`,
`"kim" == user.name` are folded to `1`/`0` against the JSON context *before*
muParser parses — so quoted literals never reach the numeric engine, and they
compose with numeric terms (`status == "ready" && doc.data[0] == 2`).

## Where it lives

`xblox_expressions.{hpp,cpp}` — the central place for variables, references,
and the muParser front-end:

- **Reference paths**: `split_reference`, `resolve_reference` (JSON walk with
  array indexing), `reference_number` (numeric coercion + array-size
  pseudo-fields), `reference_string` (display formatting).
- **Template pre-pass**: `interpolate_references` — resolves `${path}` keys
  the flat resolver cannot parse (dots/brackets). Unresolved paths warn and
  degrade to the bare key text instead of aborting the whole template.
- **Expression front-end**: `prepare_expression` — the full normalization
  pipeline feeding the cached muParser evaluator:

  1. trim + legacy aliases (`this.`, `hasQueuedWork()`, `selection…`)
  2. `${ref}` unwrap (template syntax tolerated in expression fields)
  3. string-comparison folding (`==`/`!=` with a quoted literal, either side)
  4. nested references → deterministic synthetic slots (`_p_…`), returned as
     `ReferenceSlot{name, path}` for the evaluator to bind
  5. remaining identifiers sanitized for muParser (`mu_var_name`)

`xblox_commands.cpp` keeps the evaluator itself (it is coupled to the scope
stack and numeric-context store) but now delegates: `context_value_ptr` /
`split_path` forward to the module, and `eval_mu_expression` runs
`prepare_expression` and binds each `ReferenceSlot` to stable per-cache-entry
storage — refreshed by value before every `Eval()`, so the **bytecode-reuse
model is untouched** (no `SetExpr`/`DefineVar` churn between rebinds).

`blocks/block_variables.hpp` (`bv::resolve`) runs the template pre-pass before
the flat variable map, so every `${…}`-capable block field gets nested
references for free.

## Performance guardrails (held)

- The three-tier order is preserved: fast single-var / simple-binary paths
  run **first** on the raw source; `prepare_expression` only runs when they
  miss. If the pre-pass reduces the text to a fast shape (`"1"`,
  `"flag > 0"`), the fast path gets a second chance before muParser.
- The `sequence()`-gated rebind cache is intact; reference slots are bound to
  stable vector storage sized once per cache entry.
- Numeric-only expressions take the exact same path as before (the fast tiers
  hit before the new front-end is consulted).
- Covered by `tests/orchestrator/test-xblox-next.mjs --only refs`
  (plus the unchanged `expressions` / `loops` / `scoping` suites).

## Known holes (deliberate)

- **Literal indices only**: `arr[i]` (variable index) is not extracted — the
  bracket must contain digits. Workaround: none yet; needs either a pre-pass
  with per-eval rewriting (cache-key explosion) or parser support.
- **String comparisons are equality only**: `<`/`>` on strings, `contains`,
  prefix/suffix tests are not available.
- **No string results**: expressions still evaluate to doubles; references to
  string values only make sense inside a folded comparison. Templates remain
  the way to produce strings.
- Unresolved references inside an expression fail the evaluation (and a
  failed condition falls back to the legacy truthy-string heuristic — same
  as before).

## Still open — built-ins (cheap, numeric engine only)

- [ ] `DefineConst`: `pi`, `e`.
- [ ] `DefineFun`: `clamp(x,lo,hi)`, `round(x)`, `floor(x)`, `ceil(x)`,
      `random()` (note: `rint`, `min`, `max`, `abs`, `sqrt`, `exp`, `ln`
      are already built in — document them).
- [ ] Expression-side `epoch` (wall-clock seconds) alongside monotonic
      `nowMs` — refreshed via a stable slot like `nowMs` today.
- [ ] `exists("name")`: 1/0 whether a context variable/path is set —
      replaces brittle truthiness probing. (The name is a literal, so
      muParser's string-constant mechanics are fine here.)
- [ ] Document the ternary `cond ? a : b` and logical `&& || !` operators
      that muParser already provides (missing from expressions.md).

## Still open — vendored muParser strings (Phase 2, only if needed)

muParser is vendored at `packages/muparser` (CMake consumes it via
`FetchContent_Declare(muparser SOURCE_DIR …)`), so parser surgery is possible
in-tree. The previous evaluation concluded:

- **String slots in the token reader/bytecode** (real string variables with
  stable-pointer binding, `==`/`!=`, `len`/`contains`/`startsWith`) is the
  only option that keeps the cached-bytecode model. muparserx (slower, no
  bytecode reuse) and `DefineStrConst` (re-parse on every value change)
  remain rejected.
- With reference folding shipped, the remaining demand for this is narrow
  (string functions, string-vs-string with both sides dynamic in one
  expression). Re-evaluate against real flows before starting; the
  unification may have absorbed most of the use cases.

Deliverables if picked up: string slot type + binding in `bind_scope_vars`,
type-error diagnostics through the existing exception path, in-tree
`muParserTest` battery in CI, and a zero-regression benchmark gate for
numeric-only expressions (`tests/xblox/fps-*.xblox`).
