## Relationship to existing MCP work

`docs/xblox/xblox-mcp.md` already consumes arbitrary MCP `inputSchema` for
dynamic tool arguments. Reuse its JSON Schema mapping and `x-xblox.kind`
annotation pipeline where possible.

This plan adds the opposite direction:

- external MCP/OpenAPI/TS/Zod schemas can guide xBlox block wiring;
- xBlox built-in output schemas can drive UI and documentation;
- an explicit xBlox graph interface can project back to LLM/MCP tools and
  resources.

There must be one normalized schema-bundle format between those directions.

## Literature and concepts to study

This is a focused reading map, not a requirement to adopt every technology.
Start with the P0 material; later sections become relevant for importers,
graph-as-tool exposure, tracing, simulation, and optimization.

### P0 — read before implementing the schema registry

| Topic / name to search | Why it matters to xBlox | Focus |
| --- | --- | --- |
| [JSON Schema Draft 2020-12](https://json-schema.org/draft/2020-12) | Canonical language proposed for value contracts | Core vs Validation specifications, dialects, vocabularies |
| [Understanding JSON Schema](https://json-schema.org/understanding-json-schema/) | Practical guide with better examples than the normative spec | Objects, arrays, composition, conditionals, references |
| JSON Schema `$id`, `$ref`, `$defs`, base URI and canonical URI | Foundation of the reusable `xblox://types/...` registry | Reference resolution, bundling, embedded resources |
| JSON Schema annotations versus assertions | Descriptions/defaults are annotations; `type`/`required` are assertions | Do not assume `default` mutates an instance |
| JSON Schema applicators | `allOf`, `anyOf`, `oneOf`, `not`, `if`/`then`/`else` compose schemas | Model discriminated file/dir entries and result variants |
| JSON Schema output formats | Validators can return basic, detailed, or verbose evaluation results | Decide how runtime validation diagnostics map to block paths |
| `additionalProperties` versus `unevaluatedProperties` | Important when composing closed object schemas | Avoid accidentally accepting or rejecting extension fields |
| [JSON Schema official test suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite) | Provides conformance fixtures and edge cases | Reuse tests for the subset xBlox claims to support |
| [JSON Type Definition — RFC 8927](https://www.rfc-editor.org/rfc/rfc8927) | Useful simpler alternative for comparison | Understand why xBlox still needs JSON Schema composition/annotations |
| Algebraic data types and discriminated unions | File-or-directory variants, success/error envelopes, and parameter-dependent outputs | Sum types, product types, tags/discriminators |
| Structural typing versus nominal typing | JSON Schema describes shape; `schemaRef` also gives a useful stable name | Decide compatibility rules for equal shapes with different identities |
| Refinement types | Path formats, numeric ranges, patterns, and non-empty strings refine primitives | Separate runtime kind from additional constraints |

Recommended implementation stance after this reading:

- Support a documented Draft 2020-12 subset.
- Preserve unknown keywords while ignoring unsupported validation behavior.
- Treat `$id`/`$ref` resolution and schema bundling as first-class P0 work.
- Keep annotations available even when runtime validation is disabled.
- Do not implement a home-grown schema composition language.

### JSON Schema terminology cheat sheet

| Term | Meaning in xBlox |
| --- | --- |
| Instance | The runtime JSON value being described or validated |
| Schema resource | One schema document with its own canonical `$id` |
| Subschema | A schema nested under `properties`, `items`, `oneOf`, etc. |
| Dialect | The metaschema/vocabulary set selected by `$schema` |
| Vocabulary | A related group of keywords such as Core, Applicator, Validation, or Unevaluated |
| Assertion | A keyword that can make validation fail, such as `type` or `required` |
| Annotation | Metadata collected during evaluation, such as `title`, `description`, or `default` |
| Applicator | A keyword that applies other schemas, such as `items`, `properties`, or `allOf` |
| Canonical URI | Stable identity used for registry lookup and cross-bundle references |
| Bundling | Packaging referenced schema resources together without rewriting their identities |
| Dereferencing | Resolving `$ref`; not the same as replacing every reference with an inline copy |
| Open versus closed object | Whether undeclared properties are accepted |
| Discriminator | A field such as `type: "file"` selecting one union variant |
| Validation result | Structured output explaining which instance and schema locations passed or failed |

### P0/P1 — C++20 language and standard-library features

The project globally requires C++20 (`CMAKE_CXX_STANDARD 20`). Prefer standard
language/library features over macros or custom utility types where they make
contracts clearer.

| Feature / term | Use in xBlox | Guidance |
| --- | --- | --- |
| `enum class` | `ParamKind`, schema dialect, validation mode, cardinality, effect policy | Prefer scoped enums over strings internally; serialize at the boundary |
| `std::optional<T>` | Optional labels, descriptions, schema refs, defaults, validation results | Use when absence differs from an empty value; avoid parallel boolean flags |
| `std::variant` + `std::visit` | Closed native unions such as validation outcomes or compiled schema instructions | Good for small closed sets; do not mirror arbitrary JSON Schema recursively with a giant variant |
| `std::monostate` | Explicit empty alternative in a variant | Prefer it to magic null strings |
| Concepts and `requires` | Constrain schema-builder property values and registry adapters | Use narrowly for readable diagnostics; avoid template-heavy public APIs |
| `std::source_location` | Contract-registration errors, emitted-slot assertions, tracing call sites | Prefer defaulted `source_location::current()` parameters over `__FILE__`/`__LINE__` macros |
| `std::span` | Non-owning views over contiguous params, outputs, validation errors | Callee must not retain it; use const spans for read-only APIs |
| `std::string_view` | Lookup keys and short-lived parser inputs | Registry must own strings; never retain views into temporary JSON or moved descriptors |
| Ranges and views | Filtering registry entries, outputs and schema diagnostics | Use when clearer than loops; avoid returning views whose backing storage may move |
| Structured bindings | Registry and JSON iteration | Already natural for block maps and schema properties |
| Designated initializers | Readable aggregate construction | C++20 requires declaration order; still fragile when structs grow |
| `constexpr` / `consteval` | Stable slot names, schema IDs, feature-independent tables | Useful for literals and compile-time checks; `nlohmann::json` remains runtime data |
| `[[nodiscard]]` | Registry validation, schema compilation, accessor parsing | Apply where ignoring failure would create an invalid contract |
| RAII | Trace spans, schema-registration transactions, resource/cancellation cleanup | Preferred foundation for instrumentation scopes |
| `std::jthread` / `std::stop_token` | Cooperative cancellation of background blocks and schema/import work | Candidate replacement/adapter for bespoke cancellation callbacks; migration must preserve host cancellation |
| `std::chrono` | Validation/import/projection timing and cache TTLs | Store durations, not raw integer milliseconds, internally |
| `std::filesystem` | Ambient bundle discovery and importer paths | Keep schema identity URIs separate from local filesystem paths |
| `std::pmr` | Allocation-heavy schema compilation/validation | Profile first; use only if manifest/schema allocation becomes measurable |
| Coroutines | Asynchronous schema/API loading or block execution | Do not introduce solely for schemas; requires an executor-wide ownership/cancellation design |
| Atomics and memory ordering | Future low-overhead counters | Prefer mutex/local aggregation initially; relaxed atomics only after the data model is fixed |

#### Avoid aggregate drift in descriptors

`OutputDef` and `ExecFlags` currently have positional aggregate initializers.
Adding fields can silently bind later values to the wrong member or require
editing unrelated registrations.

Prefer named factories/builders:

```cpp
OutputDef::primary("result", ParamKind::json_value)
    .label("Entries")
    .schema_ref("xblox://types/fs.entry-list/1")
    .writes_previous();
```

Options, in preferred order:

1. Existing fluent descriptor builders with focused methods.
2. Static named factories returning a fully valid object.
3. Designated initializers for local/simple aggregates.
4. Positional aggregate initialization only for tiny immutable structs.

Registry validation remains necessary regardless of construction style.

#### Keep the schema representation pragmatic

Do not build a complete recursive C++ type hierarchy for JSON Schema in P0.
A thin owning wrapper over `nlohmann::json` is sufficient if it:

- inserts the selected `$schema` dialect;
- owns all strings and nested values;
- provides named helpers for common object/array/ref constructions;
- preserves unknown keywords;
- validates schema resources before registration;
- returns immutable schema JSON after registration.

Use native C++ structs for xBlox metadata (`OutputDef`, `ParamDef`, execution
flags), and JSON for the standards-defined schema payload. This keeps the
standard vocabulary extensible without recompiling a deep variant hierarchy.

#### Error transport

The project is C++20, so `std::expected` is not available until C++23. Options:

- return a small project `Result<T, E>`/status type if one already exists;
- use `std::optional<T>` only when a missing value needs no diagnostic;
- return `bool` plus an explicit diagnostics collection for multi-error
  validation;
- use exceptions only for registry/programmer invariants during initialization,
  not normal document validation failures.

Do not add a third-party `expected` implementation solely for this slice unless
it will be adopted consistently.

#### Reflection and code generation

C++20 has no standard static reflection. Avoid designs that assume automatic
conversion of arbitrary C++ structs into JSON Schema.

Viable approaches:

- explicit schema registration for native block/domain types;
- a small macro/X-macro only to remove repetitive field declarations;
- external code generation from canonical JSON Schema;
- library-specific reflection traits only behind an adapter.

Generated code must retain schema `$id`, source provenance and deterministic
ordering so documentation and manifests remain stable.

#### C++ JSON Schema implementation candidates

| Candidate | Strength | Concern / evaluation |
| --- | --- | --- |
| [SourceMeta Blaze](https://blaze.sourcemeta.com/) | C++20, Draft 2020-12, compiled evaluation, standard output formats | AGPL licensing requires explicit review before embedding; useful reference/CLI even if not linked |
| [jsoncons JSON Schema](https://github.com/danielaparker/jsoncons/blob/master/doc/ref/jsonschema/jsonschema.md) | Draft 2020-12, header-oriented C++, configurable dialect/format validation | Introduces a second JSON value type unless adapted carefully to `nlohmann::json` |
| JavaScript Ajv in tooling/tests | Mature schema compilation and ecosystem | Not suitable as the native runtime validator; useful for cross-implementation conformance tests |
| Implemented xBlox subset | Small dependency surface and direct `nlohmann::json` integration | High standards-compliance risk; must use official tests and clearly reject unsupported vocabularies |

Selection checklist:

- Windows/MSVC and current CMake integration;
- Draft 2020-12 `$id`/`$ref`/bundling behavior;
- detailed error output with instance and schema locations;
- custom format/vocabulary support for `x-xblox.*`;
- schema compilation and cacheability;
- thread safety;
- binary size and startup cost;
- license compatibility;
- ability to validate `nlohmann::json` without repeated conversion;
- official test-suite/Bowtie compliance.

#### C++ topics to study

| Name / search term | Direct application |
| --- | --- |
| C++ Core Guidelines: ownership, interfaces, error handling | Registry lifetime and safe schema views |
| Rule of zero / rule of five | Owning descriptors and compiled-schema handles |
| Value semantics | Immutable manifest/schema snapshots shared across consumers |
| Type erasure | Pluggable validators/importers without exposing library types |
| Pimpl idiom | Keep validator dependency and compile cost out of public headers |
| Visitor pattern and `std::visit` | Closed compiled-schema instruction sets |
| Arena allocation and `std::pmr` | Only after schema compilation profiles justify it |
| Cooperative cancellation | Importers, remote schema resolution, background execution |
| Thread-safe initialization / magic statics | Cached immutable block and schema registries |
| ODR and inline variables | Shared constexpr slot/schema IDs across translation units |
| ABI stability | Avoid exposing third-party validator types across DLL/plugin boundaries |
| Fuzzing, ASan, UBSan | Schema/ref/parser/accessor inputs are ideal fuzz targets |
| Property-based testing | Generate schema-valid and invalid instances |

### P0/P1 — TypeScript and schema-library ecosystem

| Project / term | Why inspect it | xBlox decision it informs |
| --- | --- | --- |
| [Zod JSON Schema support](https://zod.dev/json-schema) | Zod 4 emits Draft 2020-12 and supports registries/reused refs | Prefer native `z.toJSONSchema()` over the older unmaintained adapter |
| Zod input versus output schemas | Transforms/defaults/coercions can have different input and output types | Graph tool inputs and runtime outputs may need separate schemas |
| [Standard Schema](https://standardschema.dev/) | Small interop contract implemented by multiple TS validation libraries | Potential importer adapter boundary without depending directly on Zod |
| [TypeBox](https://github.com/sinclairzx81/typebox) | JSON Schema-first TypeScript type builder | Compare schema-first authoring with Zod's validator-first model |
| [ts-json-schema-generator](https://github.com/vega/ts-json-schema-generator) | Compiler-based TS type to JSON Schema conversion | Candidate for ambient TypeScript importer |
| TypeScript compiler API | Correct way to resolve exported symbols, aliases, generics, and declarations | Avoid regex-based `.d.ts` import |
| `satisfies` and generated TS types | Lets checked-in declarations verify bundle shapes at build time | Developer ergonomics for ambient schema bundles |
| Schema provenance | Record source file, symbol, importer version, and source hash | Explain stale or conflicting ambient declarations |
| Schema normalization versus canonicalization | Equivalent schemas can have different JSON representations | Stable hashing, caches, and generated-doc diffs |

Questions to answer before selecting an importer:

- Is the source model input-shaped or output-shaped?
- How are transforms, branded values, dates, binary values, maps, sets, and
  functions rejected or represented?
- Are recursive schemas allowed?
- Are generics instantiated explicitly?
- Are descriptions sourced from JSDoc?
- Does regeneration produce deterministic IDs and ordering?
- What happens when an importer cannot represent a source type?

### P1 — API and data-contract ecosystem

| Specification / practice | Relevance |
| --- | --- |
| [OpenAPI 3.1](https://spec.openapis.org/oas/v3.1.0) | Uses a JSON Schema-aligned dialect and supplies operation input/output contracts |
| OpenAPI `operationId`, components and media types | Natural source of custom block kinds, reusable schemas, and response variants |
| AsyncAPI | Equivalent contract patterns for message/event APIs such as MQTT |
| GraphQL introspection | Another source of structural API result types; selection sets resemble explicit field access |
| Protocol Buffers / Avro schema evolution | Mature compatibility rules: backward, forward, and full compatibility |
| Consumer-driven contracts / Pact | Tests whether API providers still satisfy consumer expectations |
| Data contracts and schema registries | Operational model for ownership, compatibility gates, provenance, and rollout |
| CloudEvents | Standard event envelope worth comparing with block event/result separation |
| MIME/media-type negotiation | One endpoint may return JSON, text, files, or streams based on headers |
| Problem Details for HTTP APIs — RFC 9457 | Standard error envelope that should not be confused with successful output shape |

Useful search terms:

- schema evolution compatibility;
- backward-compatible JSON Schema changes;
- contract testing;
- schema registry subject/version;
- tolerant reader;
- Postel's law criticism;
- API response envelope;
- content negotiation;
- discriminated response unions.

### P1/P2 — jq, accessors, optics and query typing

| Topic / name | Relevance |
| --- | --- |
| [jq 1.8 manual](https://jqlang.org/manual/) | Normative behavior for filters, streams, paths, optional access, and multiple outputs |
| jq exact versus non-exact path expressions | `.a.b` identifies one path; `.a[].b` can identify many |
| jq `path`, `paths`, `getpath`, `setpath`, `delpaths` | Useful vocabulary for schema-driven field browsing and future mutation |
| jq stream semantics | A filter can emit zero, one, or many values; output cardinality is part of its type |
| Lenses and optics | Composable field/index traversal with laws; conceptual model for simple accessors |
| Prisms | Optic for selecting one variant of a union; relevant to discriminated outputs |
| Traversals | Optic that focuses zero or more values; relevant to `.[]` |
| Bidirectional transformations / lenses | Relevant only if xBlox later supports writing through an accessor |
| Static analysis of query languages | Foundation for inferring a schema through a safe jq subset |
| Abstract interpretation | General method for computing conservative output types without executing a query |

Initial jq schema propagation should deliberately support only:

- identity `.`;
- exact object field `.field`;
- optional field `.field?`;
- array traversal `.[]`;
- exact array index `.[0]`;
- simple compositions such as `.items[].path`;
- basic object/array constructors if implementation remains small.

Everything else can execute normally while its projected type becomes unknown.
Do not claim full jq type inference.

### P2 — LLM tools and MCP

| Specification / term | Relevance |
| --- | --- |
| [MCP Tools specification](https://modelcontextprotocol.io/specification/draft/server/tools) | Defines `inputSchema`, optional `outputSchema`, annotations, and tool execution |
| [MCP SEP-2106](https://modelcontextprotocol.org/seps/2106-json-schema-2020-12) | Aligns tool schemas with JSON Schema 2020-12 and permits arbitrary structured output values |
| MCP `structuredContent` versus `content` | Modern clients consume typed JSON; compatibility may still require serialized text content |
| MCP resources and resource templates | Appropriate for graph source, manifests, type definitions, and non-executable documentation |
| MCP tool annotations | Behavior hints are advisory and untrusted unless the server is trusted |
| Tool schema prompt injection / description trust | External descriptions can influence the model and require provenance/length limits |
| Structured outputs in LLM APIs | Compare provider-specific subsets and strict-mode restrictions with full JSON Schema |
| Function calling versus workflow execution | A graph's public interface is a tool contract; internal blocks are implementation details |
| Tool capability negotiation | Clients differ in support for output schemas, resources, media, and structured content |

Important MCP details to retain:

- Tool inputs remain object-shaped.
- Tool `outputSchema` may describe objects, arrays, primitives, or unions.
- Structured results must conform when an output schema is declared.
- Return compatibility content according to the protocol/client version in use.
- Do not expose every internal graph variable merely because it has a schema.
- Keep tool descriptions and remote schemas tagged with provenance/trust.

### Later — graph interfaces and module systems

| Concept | What to learn from it |
| --- | --- |
| Module signature / interface | Public graph inputs and outputs should hide internal blocks and context |
| ABI versus API | Stable slot/schema IDs are machine contracts; labels and layout are presentation |
| Semantic versioning | Decide when graph or schema changes require major/minor versions |
| Referential transparency | A graph with the same inputs has the same outputs only if all effects are controlled |
| Capability-based security | Public graph inputs should not implicitly grant filesystem/network/device authority |
| Information hiding | Public interface outputs should be selected explicitly, not inferred from all `storeAs` values |
| Link-time versus run-time binding | Ambient declarations enrich authoring; runtime handlers remain independently registered |
| Package lockfiles | Inspiration for recording exact schema IDs/hashes used when a graph was authored |

### Parked — instrumentation and provenance literature

| Topic / name | Why it will matter later |
| --- | --- |
| [W3C PROV](https://www.w3.org/TR/prov-overview/) | Standard vocabulary for entities, activities, agents, derivation, and attribution |
| Data lineage / field-level lineage | Tracks which output fields contributed to which inputs |
| Program dependence graph | Combines data and control dependencies |
| Dynamic program slicing | Computes dependencies for one actual execution |
| Static slicing | Conservative dependencies across possible executions |
| Taint tracking | Propagates origin/security labels through transformations |
| OpenTelemetry traces, spans, links and attributes | Practical distributed execution instrumentation model |
| Event sourcing | Durable history of state transitions; not automatically appropriate for high-volume block events |
| Deterministic replay | Reconstruct execution from inputs, effects, scheduling, and nondeterministic choices |
| Observability cardinality | Stable IDs are safe dimensions; raw paths/prompts/values can explode cost and leak data |

Terms to distinguish:

- **declared consumer**: a graph binding references a slot;
- **actual read**: the runtime resolved that slot during this execution;
- **emitted output**: the producer published a slot value;
- **scope export**: a value was copied into named context;
- **observed shape**: shape inferred from one or more values, never canonical;
- **provenance**: where a value/schema came from;
- **lineage**: how data flowed and transformed;
- **trace**: one execution timeline;
- **profile**: aggregate performance/resource measurements.

### Parked — simulation and effect-system literature

| Topic / name | Why it will matter later |
| --- | --- |
| Effect systems | Statically summarize filesystem/network/device/process/state behavior |
| Algebraic effects and handlers | Separate effect requests from interpreters; useful mental model for simulation |
| Free monads / tagless-final style | Alternative ways to represent operations independently of execution |
| Capability security | Effects require explicit authority rather than global ambient access |
| Hexagonal architecture / ports and adapters | Run the same block logic against real, simulated, or recorded adapters |
| Record/replay systems | Capture external interactions and replay deterministic fixtures |
| Property-based testing | Generate schema-valid inputs and check invariants |
| Model-based testing | Compare runtime behavior against a state-machine model |
| Idempotency and retry semantics | Essential before replaying or optimizing side-effecting blocks |
| Virtual time | Deterministic tests for wait, timeout, polling, and retry behavior |
| Fault injection / chaos testing | Exercise network/device/filesystem failures deliberately |

Simulation vocabulary to define before implementation:

- effect domain;
- operation;
- capability;
- simulation policy (`execute`, `stub`, `record`, `replay`, `skip`, `error`);
- fixture;
- virtual resource;
- deterministic seed;
- nondeterministic observation;
- redaction rule.

### Parked — optimization and incremental-computation literature

| Topic / name | Why it will matter later |
| --- | --- |
| Purity and referential transparency | Minimum basis for safe memoization and dead-producer elimination |
| Idempotence | Repeating an effect may be safe even when memoization is not |
| Common subexpression elimination | Reuse equivalent pure computations |
| Dead-code elimination and liveness analysis | Remove computations whose results/effects cannot be observed |
| Static single assignment (SSA) | Clear value-definition/use relationships; block output slots resemble named definitions |
| Partial evaluation / constant folding | Evaluate pure operations whose inputs are known |
| Memoization and content-addressed caching | Cache by implementation version, schema, inputs, environment, and declared dependencies |
| Incremental computation / self-adjusting computation | Recompute only graph regions affected by changed inputs |
| [Build Systems à la Carte](https://www.microsoft.com/en-us/research/publication/build-systems-la-carte/) | Taxonomy of dependency discovery, scheduling, rebuilding, and early cutoff |
| Demand-driven evaluation | Execute producers only when outputs are demanded; unsafe until effects are modeled |
| Work stealing and DAG scheduling | Parallelize independent blocks while respecting dependencies/resource locks |
| Amdahl's law | Limits of parallel speedup |
| Cache invalidation | Implementation/schema/environment changes must invalidate results |

Optimization terms that must not be conflated:

- `pure` versus merely `has_side_effects=false`;
- deterministic versus idempotent;
- cacheable versus replayable;
- unbound versus unread;
- unread versus unobservable;
- skipped versus eliminated;
- control dependency versus data dependency;
- declared dependency versus discovered dependency;
- semantic equivalence versus identical JSON serialization.

### Adjacent products and systems worth examining

| System | Inspect for |
| --- | --- |
| Node-RED | Message-oriented flow UX, node help, typed/config nodes |
| n8n | Output-data inspection, item lists, expression field picker, node schemas |
| Apache NiFi | Provenance, backpressure, queues, replay, processor relationships |
| KNIME | Typed table ports, schema propagation, node configuration/execution states |
| Apache Beam | PCollection schemas, transforms, windowing, runner separation |
| Temporal | Durable execution, retries, deterministic workflow constraints, activities |
| Airflow / Dagster / Prefect | Asset/data lineage, orchestration metadata, retries and observability |
| LangGraph | LLM-oriented state graphs, checkpoints, interrupts, tool nodes |
| OpenAI function/structured outputs | Provider-specific schema restrictions and model behavior |
| MCP SDKs/Inspector | Actual client compatibility for tools, output schemas, resources, and content |
| OpenAPI generators | Schema normalization, naming, `$ref` resolution, generated documentation |
| Confluent Schema Registry | Compatibility policies, subjects, versions, and operational schema governance |

Do not copy their execution models wholesale. The useful comparison questions
are:

- Where is the canonical type contract?
- When is schema known: authoring, compile, run, or after observation?
- How are list/item shapes displayed?
- How are fields selected without handwritten queries?
- What survives across retries and replays?
- Which effects prevent caching or parallelization?
- How are schema changes versioned and surfaced?
- How do they prevent internal state from becoming public API?

## Suggested learning sequence

| Order | Time box | Outcome |
| --- | --- | --- |
| 1 | 2–3 hours | Read JSON Schema object/array/composition/reference chapters; write `fs.entry/1` by hand |
| 2 | 1–2 hours | Study `$id`/`$ref` bundling and official tests; decide registry URI and bundle layout |
| 3 | 1 hour | Compare JSON Schema, JTD, Zod 4, and TypeBox; confirm JSON Schema remains canonical |
| 4 | 1 hour | Read jq path/filter/stream sections; freeze the small statically typed accessor subset |
| 5 | 1 hour | Read MCP tools/output schema/resources; confirm graph interface projection |
| 6 | Half day prototype | Carry `fs.entry/1` from C++ registration through manifest to the vertical UI |
| 7 | Half day tests | Add native serialization, TS projection, tooltip, child-scope, and browser smoke tests |
| 8 | Later | Study provenance/effects/incremental computation before designing instrumentation or optimization |

The most important practical exercise is not more reading: implement one schema
end to end (`fsList`) and use the friction to refine this plan before inventorying
the rest of the built-ins.
