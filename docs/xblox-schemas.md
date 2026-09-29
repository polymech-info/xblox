# xBlox value schemas — implementation matrix

This document defines how xBlox describes structured values across native
blocks, wiring UI, graph documents, generated documentation, LLM tools, MCP
tools/resources, simulation, and future optimization.

Legend: `[ ]` todo · `[~]` in progress · `[x]` shipped · `[?]` decision needed.

## Goals and priority

| Priority | Goal | User-visible result | Exit criterion |
| --- | --- | --- | --- |
| P0 | Structured output schemas for built-in blocks | `fsList.result` displays as `FsEntry[]`; users can discover `name`, `path`, `type`, and `size` before running | Native manifest, TS mirror, and wiring projection preserve one reusable schema reference |
| P0 | Test the vertical wiring UI early | Output tooltip and iterable-child scope expose structured fields | UI smoke fixture renders `fsList → result: FsEntry[]` and `PREVIOUS: FsEntry` |
| P1 | Cover remaining built-in structured outputs | Known JSON envelopes stop appearing as opaque `json_value` | Every GUI-visible structured output either has `schemaRef` or is explicitly marked opaque |
| P1 | Ambient declarations for API/custom blocks | Projects can teach wiring and documentation about external response shapes | A checked-in schema bundle can augment block params/outputs without recompiling C++ |
| P1 | TS/Zod schema import | Existing application schemas can feed xBlox | Importer emits the same canonical schema bundle; the UI never evaluates arbitrary TS |
| P2 | Graph-as-tool interface | An `.xblox` graph can become an LLM/MCP tool with stable inputs and outputs | Tool `inputSchema`/`outputSchema` derive from an explicit document interface |
| P2 | jq output accessors | Users can bind a field without adding a Parse block | `blockId://<id>/<slot>?jq=<query>` lowers to a block-output binding plus jq operation |
| Parked | Usage tracing and dead-output diagnostics | Design remains compatible with future instrumentation | Preserve stable block/slot/schema IDs; no counters or optimizer behavior in this phase |

## Core decision: JSON Schema plus xBlox metadata

Use a practical subset of JSON Schema Draft 2020-12 as the canonical value
shape. Do not replace the existing block manifest with JSON Schema: execution
flags, bindings, child lists, simulation policy, dynamic providers, and UI
controls remain xBlox metadata.

`ParamKind` remains the compact runtime/UI kind. A schema adds structure,
cardinality, constraints, descriptions, examples, and reusable references.

| Concern | JSON Schema | xBlox extension |
| --- | --- | --- |
| Primitive/object/array shape | `type`, `properties`, `items`, `required` | — |
| Alternatives | `oneOf`, `anyOf`, `const`, `enum` | `x-xblox.kindWhen` when output kind depends on block parameters |
| Reuse/versioning | `$id`, `$ref`, `$defs` | Canonical `xblox://types/<name>/<version>` IDs |
| Native widget | `type`/`format` where standard | `x-xblox.kind`, existing `ui` metadata |
| Bindings | Not part of the value schema | Existing accepted bindings and binding syntax |
| Runtime output identity | Not part of the value schema | Stable `blockId` + output `slot` |
| Dynamic schema loading | `$dynamicRef` is not sufficient for provider RPCs | Existing `dynamic_schema` resolver metadata |
| Sensitive values | `writeOnly`, descriptions | `x-xblox.sensitive`, redaction policy |
| Simulation/effects | Not a value concern | Future block execution/effect metadata |

### Why not create more `ParamKind` values?

`FsEntry`, OCR documents, HTTP responses, and MCP result envelopes are
structural types, not editor widgets. Keep `json_value` as the transport kind
and attach a schema. Add a `ParamKind` only when the runtime coercion or UI
control is genuinely different.

## Canonical schema bundle

All schema sources normalize into one serializable bundle:

```json
{
  "bundleVersion": 1,
  "schemas": {
    "xblox://types/fs.entry/1": {
      "$id": "xblox://types/fs.entry/1",
      "title": "Filesystem entry",
      "description": "One item returned by fsList.",
      "oneOf": [
        {
          "type": "object",
          "properties": {
            "name": { "type": "string" },
            "path": {
              "type": "string",
              "format": "path",
              "x-xblox.kind": "file_path"
            },
            "type": { "const": "file" },
            "size": { "type": "integer", "minimum": 0 }
          },
          "required": ["name", "path", "type", "size"]
        },
        {
          "type": "object",
          "properties": {
            "name": { "type": "string" },
            "path": {
              "type": "string",
              "format": "path",
              "x-xblox.kind": "dir_path"
            },
            "type": { "const": "dir" }
          },
          "required": ["name", "path", "type"]
        }
      ]
    }
  },
  "blocks": {
    "fsList": {
      "outputs": {
        "result": {
          "schema": {
            "type": "array",
            "items": { "$ref": "xblox://types/fs.entry/1" }
          },
          "itemSchemaRef": "xblox://types/fs.entry/1"
        }
      }
    }
  }
}
```

Rules:

- `$id` is stable and versioned. Never silently change a published shape.
- Descriptions, titles, examples, and field descriptions are optional.
- Native built-ins own their canonical declarations.
- Ambient bundles may augment custom/API blocks but must not silently replace a
  native schema unless an explicit override policy allows it.
- Observed runtime values can assist debugging but never become the contract.
- Bundle loading must not execute code.

## Manifest additions

The native `BlockDescriptor` remains authoritative for built-ins.

```cpp
registry["fsList"] = bdb(/* ... */)
    .primary_output(ParamKind::json_value, "Entries")
    .output_schema(array_of(schema_ref("xblox://types/fs.entry/1")))
    .iterable_item_schema("xblox://types/fs.entry/1")
    .iterable();
```

Proposed serialized additions:

```ts
export type BlockManifestOutput = {
  // Existing output identity/type metadata.
  slot: string;
  kind?: ParamKind | string;
  primary?: boolean;
  conditional?: boolean;
  dynamic?: boolean;
  supportedKinds?: string[];

  // New structural contract.
  schema?: JsonSchema;
  schemaRef?: string;
  itemSchemaRef?: string;
};

export type BlockManifest = {
  // Existing fields.
  schemas?: Record<string, JsonSchema>;
};
```

Use `schemaRef` for shared named types and inline `schema` for small,
block-specific envelopes. `itemSchemaRef` states the `PREVIOUS` shape inside an
iterable producer's child list; do not force the UI to reverse-engineer array
schemas.

## Use-case matrix

| Use case | Declaration owner | Native/runtime | UI | Docs / LLM | MCP | Optimization / tracking |
| --- | --- | --- | --- | --- | --- | --- |
| Primitive block parameter | `ParamDef` | Coerce and validate | Existing editor widget | Tool input description | `inputSchema` property | Stable input provenance later |
| Structured parameter | Param schema or `schemaRef` | Optional validation | Object/array editor or JSON fallback | Explain nested fields | Nested `inputSchema` | Shape-aware simulation later |
| Fixed primary output | `OutputDef` | Publish stable slot | Typed output pin | Return contract | `outputSchema` property | Stable producer identity |
| Structured JSON output | Output `schemaRef` | Transport remains `json_value` | Shape tooltip and field browser | Document fields | Structured result | Enables field-level dependency analysis |
| Iterable output | Output schema + `itemSchemaRef` | Fan out array items | Child scope shows `PREVIOUS: ItemType` | Explain item contract | Array result | Per-item tracing parked |
| Conditional output | Existing `conditional` + schema | May not emit | Optional badge | Document condition | Optional result field | Absence tracking parked |
| Parameter-discriminated output | `x-xblox.kindWhen` / variants | Validate selected variant | Resolve current type from block values | Explain variants | `oneOf` plus extension | Better static compatibility |
| Truly dynamic output | `supportedKinds` + `oneOf` | Validate emitted variant | Show union | Explain runtime selection | `oneOf` | Conservative optimizer |
| Dynamic scope export | Existing `exportFromParam` | Publish actual export name | Complete-flow write edge | Document generated name | Usually internal | Stable slot remains trace key |
| API/custom block ambient type | Project schema bundle | No native recompilation | Wiring gains shapes | Generated custom docs | Tool schema when exposed | Untrusted declarations never authorize optimization |
| TS/Zod import | Offline/dev importer | Emits JSON only | Consumes bundle | Generates docs | Generates schemas | No runtime TS evaluation |
| Graph exposed as tool | `.xblox.interface` | Adapter validates boundary | Tool-preview panel | LLM function definition | MCP tool | Internal graph remains opaque |
| Graph exposed as resource | Document/resource metadata | Read-only serialization | Resource preview | Context retrieval | MCP resource | Not executable |
| jq accessor binding | Binding + parameter operation | Evaluate jq at input resolution | Field/query picker | Compact reference syntax | Internal graph detail | Explicit field dependency later |
| Runtime observed shape | Instrumentation event, parked | Debug-only sample/hash | Inspector after run | Never canonical | Never canonical | Future mismatch diagnostics |
| Dead/unused output | Instrumentation, parked | No behavior change now | Future dim/badge | Not documentation | Not exposed | Requires actual-read counters |

## Layer ownership matrix

| Layer | Owns | Consumes | Must not do |
| --- | --- | --- | --- |
| Native block registration | Built-in params, outputs, schema refs, execution flags | Native schema registry | Embed UI-specific React behavior |
| Native schema registry | Versioned JSON Schema definitions | Built-in and generated schema bundles | Execute TS/Zod or probe networks during render |
| Manifest serialization | Portable block/schema contract | Native descriptors | Drop fields for individual consumers |
| `apps/shared/xblox/manifest.ts` | TS mirror and normalization helpers | Serialized manifest | Become a second source of truth |
| Wiring projection | Resolve refs, output/item shapes, jq result shapes | Manifest + document values | Guess structures from runtime samples |
| Property/wiring UI | Discovery, autocomplete, field selection, docs | Projected schemas | Define canonical schemas |
| Executor | Coercion, binding resolution, optional schema validation | Compiled document + native contracts | Depend on labels/descriptions |
| `.xblox` document | Graph and explicit public interface | Manifest/schema refs | Copy every built-in description/schema |
| Ambient bundle | Custom/API declarations | Project/user schema files | Override trusted native contracts silently |
| TS/Zod importer | Convert trusted source to bundle | Build/dev tooling | Run in the embedded UI or native executor |
| LLM projection | Compact blocks, schemas, examples | Canonical manifest/interface | Receive UI layout metadata by default |
| MCP projection | Tool `inputSchema`, `outputSchema`, resources | Explicit graph interface | Expose internal context automatically |
| Instrumentation | Future emitted/read counters and observed shapes | Stable block/slot/schema IDs | Change execution semantics |
| Optimizer | Future purity/effects/dependency analysis | Declared contracts + instrumentation | Treat schemas or unbound outputs as proof of purity |

## Public graph interface

An `.xblox` document needs an explicit public boundary. Internal context,
PREVIOUS, and every `storeAs` variable must not automatically become tool
parameters/results.

```json
{
  "version": 2,
  "description": "List Markdown files in a directory.",
  "interface": {
    "name": "list_markdown_files",
    "description": "Return Markdown file entries.",
    "inputs": {
      "directory": {
        "description": "Directory to inspect.",
        "schema": {
          "type": "string",
          "format": "path",
          "x-xblox.kind": "dir_path"
        },
        "target": {
          "blockId": "fsList-1ret4d",
          "param": "path"
        }
      }
    },
    "outputs": {
      "entries": {
        "description": "Matching filesystem entries.",
        "source": {
          "blockId": "fsList-1ret4d",
          "output": "result"
        },
        "schema": {
          "type": "array",
          "items": { "$ref": "xblox://types/fs.entry/1" }
        }
      }
    }
  },
  "roots": []
}
```

Projection rules:

- LLM function and MCP tool inputs are plain values, not xBlox binding objects.
- Tool outputs include only declared interface outputs.
- Tool/resource descriptions are optional.
- Internal graph bindings retain the existing document-v2 representation.
- A graph may be an MCP resource without being executable as a tool.
- Remote/plugin descriptions are untrusted text: cap length and retain
  provenance before placing them in an LLM prompt.

Suggested resources:

```text
xblox://blocks/fsList/manifest
xblox://types/fs.entry/1
xblox://graphs/list-markdown-files
xblox://graphs/list-markdown-files/schema
```

## jq output accessors

Keep the canonical persisted binding object. Add a compact accessor syntax for
authoring, display, LLM output, and MCP/resource links:

```text
blockId://fsList-1ret4d/result?jq=.path
blockId://fsList-1ret4d/result?jq=.[].path
```

The parser lowers it to:

```json
{
  "kind": "blockOutput",
  "blockId": "fsList-1ret4d",
  "output": "result",
  "operation": {
    "kind": "jq",
    "query": ".path"
  }
}
```

Decisions:

- URL-encode the jq query when serialized as a URI.
- `blockId` and output slot remain the stable producer identity.
- Simple selectors (`.field`, `.[]`, `.field[]`) should propagate schema.
- Complex jq initially falls back to `json_value`/unknown shape.
- The wiring UI offers schema fields first and an advanced free-text jq editor.
- This is the first real `parameterOperation`; execution support must land
  before persisting accessors from the UI.
- Explicit accessors naturally expose field-level consumers for future
  traceability, but counters remain parked.

## Ambient declarations and importers

### Canonical project file

Start with a data-only file such as `.xblox.schemas.json`:

```json
{
  "bundleVersion": 1,
  "schemas": {
    "project://types/weather.response/1": {
      "$id": "project://types/weather.response/1",
      "type": "object",
      "properties": {
        "temperature": { "type": "number" },
        "unit": { "enum": ["C", "F"] }
      },
      "required": ["temperature", "unit"]
    }
  },
  "blocks": {
    "weatherApi": {
      "outputs": {
        "result": {
          "schemaRef": "project://types/weather.response/1"
        }
      }
    }
  }
}
```

Resolution precedence:

1. Explicit schema attached to the graph interface.
2. Project ambient declaration.
3. Trusted native block manifest.
4. Opaque `json_value`.

Native replacement requires an explicit `overrideNative: true` policy and must
be visibly marked in the UI. Runtime-observed shapes are not in this chain.

### TS/Zod importer

The importer is trusted developer tooling, not an embedded runtime feature:

```text
tanit-cli xblox schema import \
  --from ./src/api-types.ts \
  --symbol WeatherResponse \
  --out .xblox.schemas.json
```

Candidate adapters:

- JSON Schema passthrough/normalization first.
- OpenAPI component/operation import second.
- TypeScript type import via a compiler-based tool.
- Zod import only through an explicit adapter compatible with the installed Zod
  version.

All adapters emit the same schema bundle. No consumer needs to know whether a
schema originated in C++, JSON Schema, OpenAPI, TypeScript, Zod, or MCP.

## Early vertical-UI slice

Implement the smallest complete path before broad built-in coverage:

| Step | Change | Main files | Acceptance |
| --- | --- | --- | --- |
| 1 | Add schema fields to native `OutputDef` and manifest TS types | `src/xblox/blocks/block_registry.hpp`, `apps/shared/xblox/manifest.ts` | `xblox info --json` includes `fsList.result.schema` and reusable definitions |
| 2 | Register `fs.entry/1` and attach it to `fsList` | New native schema registry + `src/xblox/blocks/fs_blocks.cpp` | One canonical definition; no duplicated inline field lists |
| 3 | Preserve schemas through host payload normalization | `CBlockView.cpp`, `PrototypeApp.tsx` | Browser receives schema unchanged |
| 4 | Resolve refs in wiring projection | `projectDocument.ts`, new shared schema helpers | `WiringOutputPort` has resolved display type and item schema |
| 5 | Render output shape | Wiring output tooltip/detail label | Tooltip shows `Entries · FsEntry[]` and fields |
| 6 | Render iterable child scope | Wiring/tree child context UI | Child displays `PREVIOUS: FsEntry`; `index: integer` is visible separately |
| 7 | Add field picker | Wiring port tooltip or binding dialog | Selecting `path` creates a proposed jq accessor/operation |
| 8 | Verify fallback | Same UI with unknown schema | Opaque JSON remains usable and clearly labeled |

Do not wait for TS/Zod import, graph-tool exposure, tracing, or optimization
before validating this slice.

## Test matrix

| Tier | Test | Fixture | Required assertion |
| --- | --- | --- | --- |
| Native unit | Schema registry and `$ref` validation | `fs.entry/1` | IDs unique; refs resolve; required fields valid |
| Native manifest | Descriptor serialization | `fsList` | Output schema and item schema survive `to_manifest()` |
| Native runtime | Optional debug shape validation | `fsList` temp directory | Every emitted entry satisfies file/dir variant |
| TS unit | Manifest normalization and ref resolution | Native payload JSON | No schema fields are dropped |
| Projection unit | Output/item type projection | `fsList` document | `result = FsEntry[]`; child PREVIOUS = `FsEntry` |
| UI component | Output tooltip | Synthetic manifest | Fields and optional `size` render |
| UI component | Child-scope picker | Synthetic manifest | `name`, `path`, `type`, `size`, and `index` are discoverable |
| Browser smoke | Vertical wiring view | `tests/xblox/agent-local.xblox` | Screenshot visibly exposes `fsList` output/item structure |
| Compatibility | Block without schema | Existing opaque JSON block | No regression; fallback is `json_value` |
| Ambient bundle | Project declaration | `weatherApi` fixture | Custom output shape appears without native registration |
| Importer golden | JSON Schema/TS/Zod adapter | Small checked-in source | Stable normalized bundle |
| jq parser | URI round-trip | Simple and encoded queries | URI ↔ binding operation is lossless |
| jq schema | Simple selector inference | `FsEntry[].path` | Result kind resolves to `file_path` |
| Graph interface | Tool projection | Public graph fixture | Plain input/output schemas; no internal context leakage |
| MCP contract | `tools/list` projection | Same public graph | Stable `inputSchema` and `outputSchema` |
| LLM compact | Token profile | Same graph and block catalog | Includes schemas/descriptions, omits UI-only metadata |

## Implementation backlog

### P0 — built-in schema foundation and UI proof

- [ ] Define the supported JSON Schema subset and `JsonSchema` TS type.
- [ ] Add native `SchemaRegistry` with unique `$id` and `$ref` validation.
- [ ] Add `schema`, `schemaRef`, and `itemSchemaRef` to `OutputDef`.
- [ ] Serialize a top-level schema bundle with block manifests.
- [ ] Mirror fields in `apps/shared/xblox/manifest.ts`.
- [ ] Add `fs.entry/1` once and attach it to `fsList.result`.
- [ ] Preserve/resolve schema refs in `projectDocument.ts`.
- [ ] Show `FsEntry[]` and fields in the vertical output tooltip.
- [ ] Show `PREVIOUS: FsEntry` plus `index` for `fsList.items`.
- [ ] Add native, TS projection, UI component, and browser-smoke tests.
- [ ] Run the UI proof before adding schemas to other blocks.

### P1 — built-in coverage

- [ ] Inventory every `json_value` output and classify it as known, union,
  parameter-discriminated, or intentionally opaque.
- [ ] Prioritize filesystem, network/service envelopes, OCR documents, model
  inventories, device lists, vector results, key events, and MCP results.
- [ ] Add reusable schemas rather than repeating inline objects.
- [ ] Add optional runtime validation behind a debug/test flag.
- [ ] Add generated reference documentation from the canonical bundle.
- [ ] Add a registry gate: structured built-in outputs must declare a schema or
  explicit `opaqueReason`.

### P1 — ambient declarations and import

- [ ] Define and validate `.xblox.schemas.json`.
- [ ] Load project ambient bundles without executing code.
- [ ] Mark provenance/trust/override state in the UI.
- [ ] Implement JSON Schema import first.
- [ ] Implement OpenAPI operation/component import.
- [ ] Prototype TypeScript type import.
- [ ] Add Zod adapter only after choosing supported versions.

### P2 — graph tools/resources and jq accessors

- [ ] Add the `.xblox.interface` document schema.
- [ ] Generate LLM and MCP tool projections from that interface.
- [ ] Generate MCP resources for manifests, types, graph source, and graph
  interface schema.
- [ ] Specify and parse `blockId://<id>/<slot>?jq=<query>`.
- [ ] Implement jq as a parameter-operation evaluator.
- [ ] Add simple jq schema propagation and advanced unknown fallback.

### Parked — instrumentation, simulation, optimization

- [ ] Preserve schema IDs on runtime output events.
- [ ] Record declared versus observed shape mismatches in debug mode.
- [ ] Define actual-read counters separately from schema validation.
- [ ] Do not classify unused outputs from schemas alone.
- [ ] Add purity, determinism, idempotence, effect-domain, resource-lock, and
  simulation-policy metadata before any dead-block elimination or caching.
- [ ] Keep ambient/untrusted schemas advisory; they must never authorize
  optimization.

## Open decisions

| Decision | Recommendation | Blocks P0? |
| --- | --- | --- |
| Full Draft 2020-12 validator or subset | Document and implement a subset; preserve unknown keywords | No |
| Inline schemas versus refs | Refs for reusable/domain types; inline for small one-off envelopes | No |
| Schema bundle location in payload | Top-level `schemas`, referenced by block outputs | Yes |
| Native schema builder representation | Thin typed wrapper over `nlohmann::json`, serialized unchanged | Yes |
| Runtime validation default | Off in release; on in tests/debug or explicit strict mode | No |
| Ambient native overrides | Disabled by default; explicit and visibly marked | No |
| TS importer implementation | Compiler-based offline tool; never UI evaluation | No |
| Zod versions | Choose when importer prototype begins | No |
| jq accessor URI grammar | Query parameter form shown above; canonical binding remains JSON | No |
| Complex jq type inference | Fall back to unknown/`json_value` | No |
| Graph tool inputs inferred or explicit | Explicit public interface, with optional draft inference | No |
| Description duplication in `.xblox` | Only graph/interface descriptions; built-in field docs remain in manifests | No |

## Legacy data-flow baggage and removal order

Schemas describe value shape; they do not make hidden data flow safe. The
canonical model should be:

1. a block publishes values to stable output slots;
2. a consumer reads a slot through an explicit binding;
3. lexical state is written only by an explicit state operation;
4. child-list context is named independently from sequential block output.

`PREVIOUS`, `storeAs`, and scope variables currently overlap these roles. Do not
remove them as one feature: separate their semantics first.

### Recommendation

Remove **implicit sequential `PREVIOUS` data flow first**. Deprecate ordinary
producer `storeAs` second. Keep explicit named state (`setVariable` or its
eventual equivalent) as a language feature.

`PREVIOUS` is the larger architectural liability because it creates a hidden
edge selected by document order. It impedes:

- dependency and liveness analysis;
- safe reordering and parallel scheduling;
- deterministic replay and partial execution;
- clear missing-output behavior after branches/loops;
- precise caching and invalidation;
- graph-as-tool input/output projection.

`storeAs` is redundant for direct producer-to-consumer data flow, but it is at
least an explicit named write that the complete-flow projection can display.
It also remains necessary where the intent is genuinely persistent/shared
state, interpolation, or expression lookup. Removing it before implicit
`PREVIOUS` would leave the less explicit mechanism in place.

### Performance: `PREVIOUS` versus explicit bindings

Raw lookup cost favors a single `PREVIOUS` cell only in an uncompiled
interpreter:

```text
PREVIOUS read       ≈ load one known context entry
naive binding read  ≈ parse binding + hash blockId + hash slot + lookup frame
compiled binding    ≈ indexed frame/slot load
```

The naive binding path should not be the steady-state implementation. Resolve
and validate bindings once when preparing an execution plan:

```cpp
struct ResolvedInput {
    std::uint32_t producer_index;
    std::uint16_t slot_index;
};
```

Runtime execution can then read `frames[producer_index][slot_index]`, making an
explicit binding the same complexity as `PREVIOUS` and usually only one extra
indexed load. Keep string IDs for serialization, diagnostics, and tracing—not
for hot-loop lookup.

The important trade-offs are:

| Dimension | Single `PREVIOUS` cell | Explicit output bindings |
| --- | --- | --- |
| Lookup | O(1), minimal constant | O(1) after plan compilation |
| Memory | O(1) value history | O(live producer outputs) |
| Lifetime | Implicit overwrite | Defined by consumer liveness |
| Parallel scheduling | Serial order dependency | Independent branches can run concurrently |
| Incremental execution | Must replay preceding sequence | Recompute dependency closure only |
| Caching | Weak identity: "last value" | Stable key: producer + slot + inputs |
| Partial graph execution | Fragile | Natural |
| Diagnostics | Origin must be inferred/traced | Producer identity is already known |
| Branch/loop correctness | Requires careful clearing | Frame generation/iteration can be explicit |

Explicit bindings may initially consume more memory because producer frames
must survive until their final consumer. Control this with a liveness pass:

1. count consumers for each output slot;
2. decrement after each read;
3. release or move the value after its final consumer;
4. retain values requested by tracing, public outputs, state writes, or retries;
5. use iteration/generation IDs so loop values cannot leak.

Large values need value-level policy as well. Prefer move/shared immutable
handles or resource references for images, video, tensors, and files rather
than repeatedly copying JSON/buffers into output frames. This concern exists
with `PREVIOUS` too; one cell merely hides ownership and fan-out.

The likely net performance result is:

- tiny straight-line scalar graphs: `PREVIOUS` can be marginally cheaper;
- normal I/O, media, model, or network blocks: lookup difference is noise;
- branched/repeated graphs: explicit bindings enable much larger gains from
  parallelism, liveness release, caching, and incremental recomputation;
- debugging/instrumented runs: explicit provenance avoids separate origin
  reconstruction.

Do not use performance as a reason to retain implicit `PREVIOUS`. Add a compiled
binding plan and benchmark both paths. The migration gate should require no
material regression for a scalar microbenchmark and demonstrate reduced work
for a branched incremental-execution fixture.

### Lessons from Stackless Python and V8

Neither system uses an ambient "last result" as the durable identity of values
across independently schedulable work. They separate a compact execution
representation from the semantic model.

#### Stackless Python

Stackless Python primarily solves **suspendable control flow**, not data-flow
binding:

- Python execution frames and tasklet state can survive independently of the C
  call stack;
- tasklets carry their own instruction position, frames, locals, and exception
  state;
- a scheduler selects runnable tasklets;
- channels provide explicit communication and block/wake tasklets;
- soft switching avoids rebuilding an operating-system thread stack for every
  logical task.

The xBlox lesson is to place resumable block/container state in an owned
execution frame rather than ambient process state:

```text
Tasklet                         xBlox analogue
-----------------------------   --------------------------------
instruction/frame state         block/container continuation
locals                          indexed input/output frame
scheduler runnable queue        ready-node queue
channel send/receive            explicit data/event edge
blocked tasklet                 node waiting for input/resource
tasklet context                 iteration/event lexical context
```

Stackless channels are closer to explicit bindings than to `PREVIOUS`: the
communication relationship is represented and the scheduler knows why a
tasklet is blocked. Stackless does not eliminate Python globals or infer a
data-flow graph, so it is not a direct optimization model for xBlox.

The useful design point is **heap-owned continuations plus explicit wake-up
dependencies**. For loops, events, background blocks, pause/resume, and
simulation, an xBlox execution instance should own:

- plan/node and instruction position;
- iteration/generation ID;
- resolved inputs and live outputs;
- lexical item/event context;
- cancellation and deadline state;
- pending resource/channel waits;
- trace span and deterministic replay metadata.

That state must not be reconstructed from a global `PREVIOUS` cell.

#### V8 Ignition and optimizing tiers

V8 demonstrates why an accumulator can be useful without becoming the public
semantic model.

Ignition is a register-based bytecode interpreter with a special accumulator.
Many bytecodes use the accumulator as an implicit input/output to reduce
bytecode size and dispatch operands. This resembles `PREVIOUS`, but with crucial
constraints:

- it belongs to one interpreter activation/frame;
- its producer and consumer are fixed by bytecode position;
- named locals remain frame registers;
- control-flow joins and calls have defined frame semantics;
- deoptimization metadata can reconstruct the exact registers and accumulator.

When V8 optimizes code, it does not optimize an unexplained global accumulator.
The bytecode is translated into compiler IR with explicit relationships:

- **value edges** identify which operation produced an input;
- **control edges** constrain branches, merges, and reachability;
- **effect edges** order observable state changes;
- merge/phi-like values represent alternatives at control-flow joins;
- liveness and allocation decide where values physically reside;
- deoptimization maps optimized state back to the interpreter frame.

This is the strongest model for xBlox:

```text
.xblox source
    explicit blockId/slot bindings
            ↓
validated execution-plan IR
    value + control + effect/resource edges
            ↓
interpreter plan / optimized plan
    indexed frames; accumulator fast paths where legal
```

In other words: **remove semantic `PREVIOUS`, not necessarily the accumulator
optimization**.

The planner may lower a linear single-use chain:

```text
A.result → B.input
B.result → C.input
```

to an internal accumulator sequence:

```text
run A        ; accumulator = A.result
run B(acc)   ; accumulator = B.result
run C(acc)
```

This is valid only when the compiler has proven:

- each accumulator read refers to the immediately available value;
- no branch, retry, skip, error, or asynchronous suspension changes it;
- fan-out values are retained elsewhere;
- tracing/public outputs do not require premature release;
- effect and resource ordering remains intact.

At a branch, named frame slots or SSA-like plan values carry values to all
consumers. At a merge, the plan represents alternatives explicitly rather than
leaving whichever value happened to run last in `PREVIOUS`.

#### Treatment of `storeAs`

V8 locals/registers also suggest a distinction for `storeAs`:

- direct block-to-block flow is a compiler value edge;
- short-lived lexical names are frame slots;
- mutable shared state is an effectful load/store;
- externally visible state is an explicit interface/resource.

An xBlox planner can lower a local, non-escaping `storeAs` alias to a numeric
frame slot or eliminate it entirely. A value read by interpolation, dynamic
expression, another concurrent scope, or an external observer remains an
effectful state operation and must not be optimized as an ordinary value edge
without dependency information.

#### Practical architecture

Use three levels:

1. **Document IR:** stable block IDs, output slots, explicit value/control/state
   relationships; no order-relative value identity.
2. **Resolved plan:** numeric node/slot indexes, context-frame layout,
   liveness, effect/resource edges, continuation points.
3. **Execution tier:** interpreter initially; optional accumulator fusion,
   caching, parallel scheduling, or native/specialized plans later.

Keep a source map from every resolved/optimized operation to document block,
port, and binding IDs. This is the xBlox equivalent of deoptimization/debug
metadata and is required for runner highlighting, diagnostics, replay, and
simulation.

The resulting decision is:

| Question | Decision |
| --- | --- |
| Should documents retain implicit `PREVIOUS` for speed? | No |
| Can the interpreter retain a frame-local accumulator? | Yes |
| Should direct bindings use string-map lookup per execution? | No; resolve to indexes |
| Should `storeAs` always be a scope-map write? | No; lower non-escaping aliases |
| Must real mutable/external writes remain ordered effects? | Yes |
| Should suspended work retain its own context? | Yes, like a tasklet/frame |

### Split the overloaded concepts

| Current mechanism | Actual role | Target model | Removal priority |
| --- | --- | --- | --- |
| Handler writes `PREVIOUS` | Legacy result publication | Publish stable output slot | First |
| Empty `.prev()` input reads `PREVIOUS` | Hidden producer-to-consumer edge | Explicit `blockOutput` binding | First |
| Explicit `previous` binding kind | Order-relative binding | Concrete `blockId` + slot binding | First, after materialization |
| Top-level `PREVIOUS` | Last sequential result | No canonical equivalent; compatibility view only | First |
| Child-list `PREVIOUS` item | Lexical iteration/event item | Named context such as `ITEM`/`EVENT`, with schema | Later rename; do not remove with sequential `PREVIOUS` |
| `storeAs` on an ordinary producer | Alias output into scope | Bind consumers directly to output slot | Second |
| `storeAs.*` derived exports | Alias named secondary slots into scope | Bind stable named slots directly | Second |
| Explicit `setVariable` | Intentional mutable/shared state | Retain as explicit state operation | Keep |
| Graph public output | External API contract | Explicit interface output mapping | Keep, never infer from `storeAs` |
| `${name}` / expression lookup | Reads lexical state | Explicit state dependency or expression input set | Redesign before removing scope aliases |
| `writesPrevious` / `previousBehavior` metadata | Compatibility description | Migration diagnostics only | Remove last |

### Phase 0 — establish evidence

Before changing document behavior:

- instrument implicit `PREVIOUS` reads and writes by block kind and document;
- instrument `storeAs` writes and identify whether each value is later read;
- distinguish direct parameter bindings, `${name}` interpolation, expression
  reads, public outputs, and child-scope reads;
- warn when a `PREVIOUS` read has no unique preceding producer;
- detect stale reads across skipped, conditional, loop, and error paths;
- add a document/runtime semantics version rather than guessing from fields.

Metrics must distinguish **declared**, **emitted**, **read**, and **externally
observed**. An unwired output is not necessarily unused.

### Phase 1 — stop using `PREVIOUS` as the result transport

Every handler publishes declared output slots directly. During compatibility:

- slot publication may mirror the primary result into legacy `PREVIOUS`;
- a legacy handler write may synthesize `result`, but emits a diagnostic;
- output frames are cleared at the correct block/iteration boundary;
- tests assert that declared slots and runtime emissions agree.

Remove the legacy-write-to-`result` bridge only after no built-in handler relies
on it. This phase can proceed independently of schema coverage.

### Phase 2 — materialize hidden reads

On loading a legacy document, resolve each sequential `PREVIOUS` read to the
specific producer and slot that legacy execution would use:

```json
{
  "bindings": {
    "input": {
      "kind": "blockOutput",
      "blockId": "capture-1",
      "slot": "result"
    }
  }
}
```

The migration must use the legacy executor's exact control-flow rules. A simple
"previous array element" rewrite is incorrect around containers, conditional
emission, loops, skipped blocks, errors, and child scopes.

If there is no unique static producer:

- keep the compatibility binding;
- mark the edge unresolved/order-dependent;
- require runtime tracing or user choice;
- do not claim the graph is safely schedulable.

Newly authored documents should create explicit bindings. Saving an upgraded
document should persist concrete bindings and increment its semantics version.

### Phase 3 — separate child context from sequential history

`PREVIOUS` inside an iterable/event child list means "current item/event", not
"result of the prior sibling." Preserve the capability but rename and type it:

```text
ITEM: FsEntry
INDEX: integer
```

or:

```text
EVENT: MqttMessage
```

The child-list descriptor should declare context slots and schema refs. The UI
can temporarily display `ITEM (legacy PREVIOUS)` during migration. Do not route
these values through ordinary block output history.

### Phase 4 — reduce `storeAs` to explicit state

For an ordinary producer:

```json
{
  "id": "list-files",
  "kind": "fsList",
  "storeAs": "files"
}
```

rewrite direct consumers of `files` to:

```json
{
  "kind": "blockOutput",
  "blockId": "list-files",
  "slot": "result"
}
```

For derived exports such as `event.key`, bind to the stable `key` output slot
instead of treating `event.key` as output identity.

Keep an explicit state write only when at least one of these is intentional:

- the value outlives or crosses the producer's lexical region;
- several dynamically selected consumers read it by name;
- an expression/interpolation language reads it;
- the write is externally observed;
- the graph explicitly exposes state as a resource.

Long term, ordinary producer blocks should not carry `storeAs`. Authors should
use direct bindings for data flow and a visible `setVariable`/state-write block
for mutable state. This makes the side effect explicit and independently
instrumentable.

### Phase 5 — retire compatibility metadata

Only after documents and built-ins are migrated:

- reject new `previous` bindings in the editor;
- stop serializing implicit `uses_previous` defaults;
- remove automatic primary-result mirroring;
- remove `writesPrevious` and `previousBehavior` from current manifests;
- retain a versioned legacy loader if old documents must remain readable;
- keep migration fixtures permanently.

### What should not block schema P0

The `fsList` schema/UI proof should still show the current child context as
`PREVIOUS: FsEntry` if that is what the runtime exposes today. Add a context
role (`item`) beside the label so it can later render as `ITEM: FsEntry`
without changing the schema.

Likewise, attach schemas to stable output slots, never to `storeAs` names or
the global `PREVIOUS` cell. This lets schema work survive the data-flow
migration unchanged.

### Required migration tests

| Case | Required assertion |
| --- | --- |
| Straight-line implicit `PREVIOUS` | Migrates to the exact preceding producer's primary slot |
| Explicit `previous` binding | Persists as concrete `blockOutput` when unambiguous |
| Conditional producer emits nothing | No stale value is read from an earlier block |
| Continue-on-error / skipped block | Legacy and migrated behavior are compared explicitly |
| Loop iteration | Output frame and child context do not leak across iterations |
| Iterable child `PREVIOUS` | Becomes typed item context, not prior-sibling output |
| `storeAs` read through parameter binding | Rewrites to producer slot |
| `storeAs` read through `${name}` | Remains stateful until interpolation dependencies are explicit |
| `storeAs.*` secondary export | Rewrites to the corresponding stable named slot |
| Public graph output | Remains explicit and is not inferred from state writes |
| Old document version | Legacy loader preserves behavior and emits migration diagnostics |
