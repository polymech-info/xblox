# Type schemas — flexible native declarations and surface projection

This document follows [xblox-schemas.md](./xblox-schemas.md). That document
defines JSON Schema plus xBlox metadata as the portable contract for values.
This document defines how native operations, CLI commands, xBlox blocks,
provider APIs, tools, and imported schemas can contribute to that contract
without making C++ reflection, CLI11, `ParamDef`, or a runtime options struct
the universal source of truth.

Legend: `[ ]` todo · `[~]` in progress · `[x]` shipped · `[?]` decision needed.

## Decision

Use an explicit, serializable schema model with adapters.

Do not derive the architecture from C++ reflection. C++ types are implementation
details and may be incomplete, reused across unrelated operations, contain
opaque JSON, or expose a wider runtime range than one UI should suggest.

Do not make CLI11 introspection canonical. It describes command syntax after
registration and loses semantic kinds, dynamic option relationships, reusable
value types, structured outputs, and provider schemas.

Do not make xBlox `ParamDef` canonical for library operations. `ParamDef`
contains block-language behavior such as bindings, PREVIOUS, deep variable
resolution, `storeAs`, grouping, and editor controls that do not belong to the
underlying operation.

Instead:

```text
declaration sources
    native schema builders
    checked-in JSON bundles
    OpenAPI
    TypeScript/Zod importers
    MCP/tool schemas
    provider catalog/schema services
            │
            ▼
normalized TypeSchema / OperationSchema registry
            │
      ┌─────┼──────────┬────────────┬───────────┐
      ▼     ▼          ▼            ▼           ▼
    xBlox  CLI11   JSON Schema   LLM/MCP     docs/UI
   overlay binding   bundle       tools
```

The normalized registry is the shared contract. Declaration sources and
consumer projections remain replaceable.

## Goals

1. Describe a value or operation once without coupling it to one UI framework,
   parser, compiler feature, or transport.
2. Let native library functions remain the execution source while schemas
   describe their public contract.
3. Support static fields, structured values, aliases, dynamic choices,
   provider-specific fields, and surface-only options.
4. Preserve the JSON Schema bundle and stable IDs defined by
   `xblox-schemas.md`.
5. Allow incremental adoption. Existing manual CLI11 and `ParamDef`
   registration must continue to work.
6. Keep runtime behavior explicit. Schema metadata must not silently alter
   execution, permissions, effects, or optimization.

## Non-goals

- C++ reflection or compiler-generated schemas.
- A universal serializer for arbitrary C++ objects.
- Replacing JSON Schema with a C++-only type system.
- Forcing every CLI flag and block property to be identical.
- Encoding React components or Win32 controls in the portable type contract.
- Treating suggested values as hard validation.
- Fetching network-backed options while serializing a manifest.
- Inferring contracts from observed runtime values.

## Three independent concerns

The current duplication is easier to reason about when split into three
independent concerns.

### 1. Value shape

What value crosses a boundary?

Examples:

- string, integer, boolean;
- image path;
- `FsEntry`;
- `FsEntry[]`;
- a transform result envelope;
- a provider-defined input object.

This is represented by `TypeSchema`, normalized to JSON Schema.

### 2. Operation contract

What does an operation accept and produce?

Examples:

- `media.transformImage/1`;
- `media.createImage/1`;
- `media.runAgent/1`;
- `media.resizeBatch/1`.

This is represented by `OperationSchema`. It references value schemas and adds
field identity, defaults, aliases, option sources, and operation-level
relationships.

### 3. Surface behavior

How does one surface expose an operation?

Examples:

- CLI flag `--aspect-ratio`;
- xBlox property `aspectRatio`;
- xBlox PREVIOUS fallback for `input`;
- CLI-only `--job-ui`;
- xBlox-only `storeAs`;
- an advanced group collapsed by default.

This is represented by a surface projection or overlay. It is not part of the
portable value schema.

## Canonical models

The names below are conceptual. Initial C++ implementations may use thin
builders over `nlohmann::json`, as recommended by `xblox-schemas.md`.

### `TypeSchema`

A reusable value declaration:

```cpp
struct TypeSchema {
    std::string id;              // Stable, versioned URI.
    nlohmann::json schema;       // Supported JSON Schema 2020-12 subset.
    std::string provenance;      // native, project, openapi, typescript, mcp...
    TrustLevel trust;
};
```

`TypeSchema` owns shape only. Standard JSON Schema keywords are preferred.
`x-xblox.*` extensions are allowed where JSON Schema cannot express a native
kind or xBlox-specific value behavior.

### `FieldSchema`

An operation field declaration:

```cpp
struct FieldSchema {
    std::string id;                   // Stable within the operation version.
    std::string wire_name;            // Canonical serialized name.
    std::vector<std::string> aliases; // Accepted compatibility names.

    nlohmann::json schema;             // Inline value schema, or:
    std::string schema_ref;            // reusable TypeSchema ID.

    nlohmann::json default_value;
    bool required = false;
    bool sensitive = false;

    std::vector<nlohmann::json> suggested_values;
    std::optional<OptionSourceRef> option_source;
};
```

`suggested_values` are deliberately separate from JSON Schema `enum`:

- `enum` means other values violate the contract;
- `suggested_values` means a picker or completion list should offer common
  values while allowing custom input.

This distinction is required for provider-dependent fields such as image
aspect ratio, image size, router, and model.

### `OperationSchema`

An operation declaration:

```cpp
struct OperationSchema {
    std::string id;          // e.g. "pm://operations/image.transform/1"
    std::string title;
    std::string description;

    std::vector<FieldSchema> inputs;
    std::vector<FieldSchema> outputs;

    std::vector<Relationship> relationships;
    std::vector<std::string> tags;
};
```

Relationships express semantics that cannot be inferred from isolated fields:

- `model` choices depend on `provider`;
- `providerOptions` schema depends on `provider` and `model`;
- `outputPath` is optional and derived when empty;
- `input` is required for transform but absent for create;
- one of `prompt` or `presetId` must supply a prompt;
- output shape varies with another field.

Operation IDs are independent from C++ symbol names, CLI command paths, and
xBlox block kinds.

## Explicit native declaration

Native declarations use builders or plain data. They do not inspect C++ object
layout.

Example:

```cpp
OperationSchema image_transform_schema()
{
    return operation("pm://operations/image.transform/1")
        .title("AI image transform")
        .input(field("input", schema_ref("xblox://types/image.path/1"))
            .required())
        .input(field("prompt", string_schema())
            .kind("prompt")
            .required())
        .input(field("provider", string_schema())
            .options("pm://options/image.providers"))
        .input(field("model", string_schema())
            .options("pm://options/image.models")
            .depends_on("provider"))
        .input(field("aspect_ratio", string_schema())
            .suggest({"1:1", "16:9", "9:16", "4:3", "3:4", "21:9"}))
        .input(field("provider_options", object_schema())
            .dynamic_schema("pm://schemas/image.provider-options")
            .depends_on({"provider", "model"}))
        .output(field("result", schema_ref("xblox://types/image.transform-result/1")))
        .build();
}
```

The runtime implementation remains:

```cpp
TransformResult transform_image(
    const std::string& input_path,
    const std::string& output_path,
    const TransformOptions& options,
    TransformProgressFn progress,
    BatchControl* batch,
    std::function<void()> on_before_batch_pause);
```

The schema describes its public operation contract. It does not need to mirror
the C++ signature one-to-one. Progress callbacks, cancellation objects, host
handles, and internal credentials may not be public inputs.

## Runtime bindings

A schema does not by itself write values into a C++ object. Runtime binding is
an adapter owned by the operation implementation.

```cpp
struct OperationBinding {
    std::string operation_id;
    ApplyInputFn apply_input;
    ReadOutputFn read_output;
    ValidateFn validate;
};
```

Possible implementations include:

- existing manual mapping into `TransformOptions`;
- `apply_transform_options_from_json`;
- member-pointer tables;
- generated code from a checked-in schema;
- a dedicated request builder.

The binding mechanism is replaceable and is not serialized into the schema
bundle.

This separation permits:

- one schema field to populate several runtime fields;
- compatibility aliases;
- computed/defaulted values;
- fields backed by settings rather than a direct struct member;
- one C++ options type reused by several narrower operations;
- a future runtime DTO migration without changing public schema IDs.

## Focused operation requests

Focused request types are still preferred because they reduce adapter code, but
they are not schema owners.

Good:

```cpp
struct ImageTransformRequest {
    std::vector<std::string> inputs;
    std::string output_path;
    TransformOptions options;
};
```

For an agent operation, `AgentLaunchSpec` is only part of the request. A
complete public request also needs turn construction:

```cpp
struct AgentRunRequest {
    AgentLaunchSpec launch;
    std::string prompt;
    std::string system_prompt;
    std::string planner_prompt;
    std::vector<std::string> include_paths;
    std::vector<std::string> embed_paths;
    AgentToolPolicy tools;
    ContextReductionOptions context_reduction;
};
```

CLI terminal formatting, consent-window ownership, xBlox bindings, and result
storage remain surface concerns.

## Option sources

Dynamic values are first-class registry entries, not enums copied into schemas.

```cpp
struct OptionSourceDescriptor {
    std::string id;                // Stable URI.
    std::vector<std::string> dependencies;
    OptionSourceFn resolve;
    CachePolicy cache;
};
```

Examples:

```text
pm://options/llm.presets
pm://options/llm.routers
pm://options/llm.models?router={router}
pm://options/image.providers
pm://options/image.models?provider={provider}&collection={collection}
pm://options/agent.path-tools
pm://options/audio.input-devices
```

An option response should use one common envelope:

```json
{
  "options": [
    {
      "value": "openai/gpt-5.6",
      "label": "GPT 5.6",
      "description": "General-purpose model",
      "disabled": false,
      "meta": {}
    }
  ],
  "sourceVersion": "etag-or-cache-version",
  "stale": false
}
```

Rules:

- Manifest serialization records the option-source reference but does not
  resolve network-backed values.
- Option sources may return cached values and refresh asynchronously.
- Consumers must allow a current custom value even when it is absent from the
  latest option list unless the field has a true closed `enum`.
- Credentials and sensitive provider metadata must not be included.
- The same library resolver serves Win32, embedded webviews, CLI completion,
  xBlox, and settings.

## Dynamic schemas

Provider options are not option lists. They are schemas selected by other
fields.

```cpp
struct DynamicSchemaSourceDescriptor {
    std::string id;
    std::vector<std::string> dependencies;
    DynamicSchemaFn resolve;
};
```

Example:

```text
pm://schemas/image.provider-options
    dependencies: provider, model
    result: normalized JSON Schema properties for that model
```

The existing Replicate OpenAPI cache and
`xbloxProviderOptionsSchemaGet` path are an implementation of this idea.
The target architecture moves provider lookup and normalization behind a
shared library service; the xBlox RPC becomes one transport adapter.

`x-xblox.kind` overlays remain useful for choosing editors for provider fields,
but consumers that do not understand the extension still receive valid JSON
Schema.

## Surface projections

### xBlox projection

An xBlox overlay maps operation fields to block parameters:

```cpp
XbloxOperationProjection image_transform_block()
{
    return xblox_projection("imageTransform",
                            "pm://operations/image.transform/1")
        .param("input").name("input").previous().globs()
        .param("output_path").name("outputPath").group("output_file")
        .param("provider").group("model")
        .param("model").group("model")
        .param("provider_options").name("providerOptions").advanced()
        .synthetic(pd_store_as())
        .build();
}
```

The projection may:

- rename canonical snake-case fields to document-compatible camelCase names;
- add binding syntax and PREVIOUS compatibility;
- add deep variable resolution;
- group or hide advanced fields;
- add synthetic block-language fields;
- omit operation fields unavailable in that block;
- narrow an operation for a specialized block.

The projected `ParamDef` retains a link to the operation field ID for
documentation, compatibility checks, and future migrations.

### CLI11 projection

A CLI overlay maps operation fields to command syntax:

```cpp
CliOperationProjection image_transform_cli()
{
    return cli_projection("transform",
                          "pm://operations/image.transform/1")
        .option("input").positional()
        .option("prompt", "-p,--prompt")
        .option("provider", "--provider")
        .option("model", "--model")
        .option("aspect_ratio", "--aspect-ratio")
        .option("reference_images", "-r,--reference").repeatable()
        .synthetic_flag("--json")
        .synthetic_flag("--job-ui")
        .build();
}
```

The adapter may register CLI11 directly or only enrich the existing
registration. CLI introspection should include the linked operation and field
IDs so downstream UIs can recover semantics without name heuristics:

```json
{
  "name": "--model",
  "typeName": "TEXT",
  "operationId": "pm://operations/image.transform/1",
  "fieldId": "model",
  "optionSource": {
    "id": "pm://options/image.models",
    "dependsOn": ["provider"]
  }
}
```

CLI11 remains responsible for parsing command syntax. The operation schema
supplies semantic metadata and shared validation.

### Tool projection

LLM and MCP tools project only public operation fields:

- exclude API keys and host-only controls by policy;
- include structured input/output schemas;
- resolve aliases to canonical wire names;
- retain descriptions where token budget allows;
- preserve dynamic-provider fields as schema, not UI controls.

Tool execution maps validated JSON through the same `OperationBinding` used by
other programmatic callers.

## Naming and aliases

One operation field may have several surface names:

```text
canonical field ID     aspect_ratio
canonical wire name   aspect_ratio
xBlox document name   aspectRatio
CLI flag              --aspect-ratio
legacy alias          aspect
```

Rules:

1. Field ID is stable within an operation major version.
2. Wire name is the preferred programmatic JSON name.
3. Surface names live in projections.
4. Compatibility aliases are accepted on input but never emitted as canonical
   output.
5. Aliases must not be used to join unrelated fields by spelling alone.

This removes provider/model/path heuristics from generic UI code.

## Constraints, validation, and suggestions

Schemas must distinguish four categories:

### Contract constraints

Values outside the constraint are invalid everywhere:

- integer minimum/maximum;
- required property;
- true closed enum;
- object required fields;
- mutually exclusive alternatives.

These project to JSON Schema and shared runtime validation.

### Suggested values

Common values offered by a UI, but custom values remain valid:

- image aspect-ratio shortcuts;
- common image sizes;
- recently used models;
- common output formats accepted by extensible providers.

These do not become `enum`.

### Dynamic availability

Values currently available from a provider or host:

- installed local models;
- live router models;
- audio/video devices;
- Replicate models in a collection.

These use option sources and can change without a schema version bump.

### Runtime policy

Values may be structurally valid but disallowed by deployment policy,
credentials, feature flags, or capabilities. Policy errors remain runtime
errors and are not encoded as value-shape constraints.

## Defaults

Defaults have several origins and must not be flattened into one ambiguous
string:

```cpp
enum class DefaultKind {
    Literal,
    AppSettings,
    Provider,
    Derived,
    None,
};
```

Examples:

- `resize_width = 0`: literal sentinel;
- model empty: resolve from app settings;
- output path empty: derive from input and prompt;
- Replicate model empty: provider fallback;
- CLI flag absent: do not override a preset.

The schema can expose a literal JSON Schema `default` only when the value is
actually a stable literal. Other behavior uses an extension:

```json
{
  "type": "string",
  "x-pm.default": {
    "kind": "appSettings",
    "path": "chat.image_model"
  }
}
```

Consumers may explain such defaults but must not eagerly materialize them into
saved documents.

## Structured outputs

Operation outputs and xBlox output slots use the same reusable `TypeSchema`
registry.

Example:

```json
{
  "$id": "xblox://types/image.transform-result/1",
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "format": "path",
      "x-xblox.kind": "image_path"
    },
    "aiText": { "type": "string" },
    "providerUrl": {
      "type": "string",
      "format": "uri"
    }
  },
  "required": ["path"]
}
```

CLI JSON output, xBlox `result`, tool results, and HTTP responses may reference
the same type when their envelopes genuinely match. Similar-looking envelopes
must not share an ID if their compatibility guarantees differ.

## Registration and composition

The registry accepts several source classes:

```cpp
registry.add_type(native_type_schema());
registry.add_operation(native_operation_schema());
registry.add_option_source(native_option_source());
registry.add_dynamic_schema_source(native_dynamic_schema_source());

registry.merge_bundle(project_bundle, project_policy);
registry.merge_bundle(openapi_bundle, provider_policy);
```

Registration validates:

- unique stable IDs;
- valid supported JSON Schema subset;
- resolvable local `$ref`s;
- unique field IDs within operations;
- valid relationship dependency names;
- valid option/dynamic-schema source references;
- no unauthorized replacement of trusted native contracts.

Composition must retain provenance. A normalized schema should still report
whether it came from native code, a project bundle, OpenAPI, MCP, or imported
developer tooling.

## Versioning

The rules from `xblox-schemas.md` apply:

- published `$id`s are stable and versioned;
- incompatible shape changes require a new version;
- runtime samples never update contracts;
- ambient schemas cannot silently replace native contracts.

Operation schemas follow the same rule:

```text
pm://operations/image.transform/1
pm://operations/image.transform/2
```

Changing a CLI spelling or xBlox group does not require an operation version
bump. Changing the public field contract may.

Recommended gates:

1. Golden normalized schema snapshots.
2. Compatibility checks between adjacent versions.
3. A registry test that every projection references an existing operation and
   field.
4. A runtime test that emitted structured outputs validate against declared
   schemas in debug/test mode.

## Compatibility with existing code

### Existing `ParamDef`

Keep `ParamDef` and `BlockDescriptor`. Add optional links:

```cpp
std::string operation_id;
std::string operation_field_id;
std::string schema_ref;
```

Blocks without links continue to work as native xBlox-only blocks.

### Existing CLI11 introspection

Keep the current CLI schema fields. Add optional semantic data when a CLI
projection exists:

```text
operationId
fieldId
schema / schemaRef
suggestedValues
optionSource
sensitive
```

Generic command editors consume semantic data first and retain current
type/name heuristics only as a legacy fallback.

### Existing JSON option mappers

Functions such as `apply_transform_options_from_json` remain valid
`OperationBinding` implementations. Move aliases, validation, and default
descriptions into shared operation metadata incrementally rather than rewriting
all mappers at once.

### Existing tool catalogs

Hand-maintained tool schemas remain accepted declaration sources. Over time,
tool entries linked to an operation should project from `OperationSchema` so
their field constraints and descriptions stop drifting.

### Existing provider RPCs

Keep current host methods as transport compatibility:

```text
xbloxUiOptionsGet
xbloxProviderOptionsSchemaGet
settingsCliCommandSchemaTreeGet
```

Route them through shared option and dynamic-schema registries. New consumers
should address stable source IDs rather than hard-coded RPC method details.

## Image transform example

Current shared runtime:

```text
CLI transform ─────┐
xBlox imageTransform ──► TransformOptions ─► transform_image()
LLM image_transform ─┘
```

Target declarations:

```text
OperationSchema: pm://operations/image.transform/1
Runtime binding: TransformOptions + transform_image()
Option sources:
  pm://options/image.providers
  pm://options/image.models (depends on provider/collection)
Dynamic schema:
  pm://schemas/image.provider-options (depends on provider/model)
Surface projections:
  CLI transform
  xBlox imageTransform
  LLM/MCP image_transform
```

Important differences remain explicit:

- xBlox supports PREVIOUS and `storeAs`;
- CLI supports batch `--src`, `--job-ui`, and `--preset-id`;
- the LLM tool may exclude API keys and host paths;
- provider options may be available only where the caller can render or submit
  the dynamic schema.

Shared operation metadata covers only their genuine common contract.

## LLM agent example

Current shared runtime:

```text
CLI llm agent ─────┐
xBlox llmAgent ───────► resolve_agent_launch() ─► run_turn()
chat send ─────────┘
```

The public operation is wider than `AgentLaunchSpec`, so its schema should not
be mechanically derived from that struct.

Target declarations:

```text
OperationSchema: pm://operations/agent.run/1
Runtime binding:
  AgentRunRequest builder
  resolve_agent_launch()
  apply_launch_policy_to_turn()
  run_turn()
Option sources:
  pm://options/llm.presets
  pm://options/llm.routers
  pm://options/llm.models (depends on router)
  pm://options/agent.path-tools
Surface projections:
  CLI llm agent
  xBlox llmAgent
  chat send
```

The CLI may expose external runner, terminal, scheduler, consent-owner, voice,
and session controls that do not belong to the common `agent.run` operation.
Those remain CLI-specific or become separate operation schemas if reusable.

## Security and trust

Schema metadata is descriptive, not authorization.

- `sensitive` controls redaction and editor behavior but does not grant secret
  access.
- An option source may enumerate public IDs but must not expose credentials.
- Ambient schemas cannot enable native execution or relax command policy.
- Imported descriptions are untrusted text and must be length-capped before
  LLM inclusion.
- Dynamic schema and option resolvers use the host's existing network,
  authentication, cache, and policy boundaries.
- A schema claiming a path or URL kind does not permit filesystem or network
  access.

## Performance

Schema work is control-plane work, not hot-path execution.

- Normalize native schemas once per process or registry generation.
- Cache serialized manifests by registry version.
- Resolve dynamic options only when requested.
- Cache provider schemas by provider/model/source version.
- Compile operation bindings independently from descriptive schema JSON.
- Do not parse CLI11 or inspect block manifests on every operation invocation.

Runtime execution continues to use focused C++ request/options types.

## Incremental implementation

### Phase 0 — registry vocabulary

- [ ] Define `TypeSchema`, `FieldSchema`, `OperationSchema`, `OptionSourceRef`,
  and dynamic-schema source JSON shapes.
- [ ] Define stable URI namespaces and provenance/trust fields.
- [ ] Document `enum` versus `suggestedValues`.
- [ ] Add normalization and registry validation tests.

### Phase 1 — dynamic option services

- [ ] Consolidate duplicate router/model responders behind one library option
  source.
- [ ] Register LLM presets, routers, models, path tools, image providers,
  Replicate collections, and Replicate models.
- [ ] Route existing host RPCs through the shared source registry.
- [ ] Preserve custom values missing from a current catalog.

This phase directly improves command property editors without waiting for full
operation schemas.

### Phase 2 — image transform pilot

- [ ] Declare `pm://operations/image.transform/1`.
- [ ] Link CLI `transform` flags to operation field IDs.
- [ ] Link xBlox `imageTransform` params to the same fields.
- [ ] Keep CLI-only and xBlox-only fields in projections.
- [ ] Link Replicate provider options to the dynamic-schema source.
- [ ] Add one structured transform-result schema.

### Phase 3 — agent run

- [ ] Define the common `AgentRunRequest` boundary.
- [ ] Declare `pm://operations/agent.run/1`.
- [ ] Link preset/router/model/tool fields.
- [ ] Keep CLI-only session/console/consent controls separate.
- [ ] Replace UI name heuristics with operation field metadata.

### Phase 4 — projection generation

- [ ] Add helpers that create baseline `ParamDef`s from operation fields.
- [ ] Add helpers that enrich or register CLI11 options from CLI projections.
- [ ] Generate LLM/MCP tool schemas for linked operations.
- [ ] Generate operation reference documentation.

Generation is optional per projection. Manual projections remain valid when a
surface needs custom behavior.

### Phase 5 — broader coverage

- [ ] Resize, create image, metadata, audio, video, and filesystem operations.
- [ ] Replace duplicated output envelope descriptions with reusable type IDs.
- [ ] Add OpenAPI and project-bundle operation imports where useful.
- [ ] Require known structured outputs to declare a schema or `opaqueReason`.

## Testing

### Registry tests

- IDs and field IDs are unique.
- `$ref`s and source references resolve.
- dependency graphs contain no invalid field names or cycles where prohibited.
- trusted contracts cannot be silently replaced.

### Projection tests

- CLI and xBlox aliases map to the intended canonical field.
- synthetic surface fields do not leak into tool schemas.
- required/default semantics survive projection.
- suggested values remain open inputs.
- sensitive fields are omitted or redacted by policy.

### Runtime contract tests

- accepted canonical and alias input names bind to the same runtime request;
- invalid closed constraints fail consistently;
- provider-dependent schemas are selected from current dependency values;
- debug/test output validation catches declared/emitted shape mismatches.

### Compatibility tests

- unlinked CLI commands retain current introspection behavior;
- unlinked blocks retain existing `ParamDef` behavior;
- old host RPC methods return compatible envelopes;
- documents do not gain materialized app-setting defaults merely by loading.

## Open decisions

| Decision | Recommendation |
| --- | --- |
| Native builder representation | Thin typed builder over `nlohmann::json` and explicit descriptor structs |
| Schema ownership | Stable registry IDs, not C++ types, CLI options, or block fields |
| Runtime binding | Explicit per-operation adapter; member-pointer helpers optional |
| Open value lists | `suggestedValues`, not JSON Schema `enum` |
| Dynamic choices | Stable option-source registry |
| Dynamic object fields | Stable dynamic-schema source registry |
| CLI generation | Incremental; enrich existing options before replacing registration |
| xBlox generation | Baseline projection plus explicit block overlay |
| UI metadata | Surface projection; portable semantic kinds only in schema extensions |
| Imported schemas | Normalize to the same bundle and retain provenance/trust |
| Reflection | Not required and not an architectural dependency |

## Resulting ownership

```text
Library operation
    owns execution and runtime binding

Type/operation registry
    owns stable public contracts and semantic relationships

xBlox block descriptor
    owns block-language behavior and editor projection

CLI projection / CLI11
    owns command spelling and parsing behavior

Provider services
    own live options and dynamic provider schemas

JSON Schema bundle
    is the portable interchange consumed by UI, docs, LLM, MCP, and projects
```

This preserves the central decision in `xblox-schemas.md` while allowing native
types, operation boundaries, dynamic providers, CLI commands, blocks, and
external schemas to evolve independently.
