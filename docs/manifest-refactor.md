# Manifest and ambient parameter UI refactor

This is a UI/declaration subset of `xblox-schemas.md`. It adopts the canonical
schema bundle and ambient declaration model without implementing the broader
xBlox runtime, wiring, output-schema, importer, or tool-projection work.

## Scope

- [ ] Normalize native manifests and ambient declarations into one canonical,
  serializable bundle consumed by shared UI code.
- [ ] Load data-only project ambient declarations from `.xblox.schemas.json`.
- [ ] Allow ambient declarations to augment parameter schemas and UI metadata
  without recompiling C++.
- [ ] Use one shared parameter renderer in Settings and the xblox property panel.
- [ ] Preserve declaration provenance and make overrides explicit.
- [ ] Keep `apps/shared/xblox/manifest.ts` a typed mirror and normalization
  layer, not a second source of declarations.

## Not in this refactor

- [ ] Do not change xblox execution, coercion, binding, PREVIOUS, or validation.
- [ ] Do not change video-source runtime precedence or introduce new executable
  block fields.
- [ ] Do not implement structured output wiring, jq accessors, graph interfaces,
  LLM/MCP projection, runtime observations, tracing, or optimization.
- [ ] Do not implement TypeScript, Zod, or OpenAPI importers yet.
- [ ] Do not execute project code while loading declarations.

## 1. Define the canonical bundle subset

- [ ] Add shared TypeScript types for the implemented subset:
  - `bundleVersion`;
  - reusable `schemas`;
  - block declarations;
  - parameter augmentations;
  - parameter UI metadata;
  - declaration provenance and override state.
- [ ] Use the `xblox-schemas.md` canonical bundle envelope:

```json
{
  "bundleVersion": 1,
  "schemas": {},
  "blocks": {}
}
```

- [ ] Extend each block declaration with a `params` map keyed by stable manifest
  parameter name.
- [ ] Allow a parameter declaration to contain:
  - inline JSON Schema via `schema`;
  - reusable schema reference via `schemaRef`;
  - `x-xblox.kind` when a specific existing widget kind is required;
  - xblox `ui` metadata;
  - composite-control membership metadata.
- [ ] Keep structural value shape in JSON Schema and editor behavior in xblox
  extensions; do not encode React component names as JSON Schema types.
- [ ] Version published `$id` values and never mutate their meaning silently.
- [ ] Reject unsupported future `bundleVersion` values with a useful diagnostic.

## 2. Define ambient parameter declarations

- [ ] Specify `.xblox.schemas.json` as the initial project declaration file.
- [ ] Add a concrete parameter-augmentation example:

```json
{
  "bundleVersion": 1,
  "schemas": {},
  "blocks": {
    "videoCapture": {
      "params": {
        "input": {
          "ui": {
            "control": "video_input",
            "composite": "capture-source"
          }
        }
      }
    },
    "llmAgent": {
      "params": {
        "preset": {
          "ui": {
            "control": "select",
            "options_path": "agent.presets",
            "composite": "llm-agent",
            "composite_role": "preset"
          }
        },
        "router": {
          "ui": {
            "control": "select",
            "options_path": "agent.routers",
            "composite": "llm-agent",
            "composite_role": "router"
          }
        },
        "model": {
          "ui": {
            "control": "select",
            "options_path_template": "agent.models.{preset}.{router}",
            "composite": "llm-agent",
            "composite_role": "model"
          }
        }
      }
    }
  }
}
```

- [ ] Define ambient declarations as augmentation by default.
- [ ] Do not let ambient declarations silently replace trusted native fields.
- [ ] Require `overrideNative: true` for an intentional replacement and retain
  that fact in resolved provenance.
- [ ] Do not allow an ambient declaration to imply executable support for a
  field absent from the native manifest in this phase.
- [ ] Preserve unknown extension keys when reading and writing the bundle.

## 3. Implement deterministic merge and provenance

- [ ] Implement the declaration precedence from `xblox-schemas.md`:
  1. explicit graph/interface declaration, when available;
  2. project ambient declaration;
  3. trusted native block manifest;
  4. opaque/generic fallback.
- [ ] For this refactor, implement project ambient + native manifest + fallback;
  reserve the explicit graph/interface layer in the API.
- [ ] Merge block parameters by stable block kind and parameter name.
- [ ] Merge nested `ui` metadata by key instead of replacing the entire object.
- [ ] Resolve `schemaRef` against the merged bundle's `schemas` map.
- [ ] Detect missing references, duplicate `$id` values, incompatible kinds,
  and forbidden native replacements.
- [ ] Attach source information to each resolved field:
  - native;
  - project ambient;
  - explicit override;
  - fallback.
- [ ] Return diagnostics separately from the usable resolved bundle so one bad
  declaration does not blank the whole editor.

## 4. Add safe bundle loading

- [ ] Locate `.xblox.schemas.json` from the active project/workspace.
- [ ] Read and parse it as data only.
- [ ] Add size, depth, and diagnostic limits before exposing content to an
  embedded UI.
- [ ] Cache by project path and file modification identity.
- [ ] Invalidate the cache when the declaration file changes.
- [ ] Return the native-only bundle when the project file is absent.
- [ ] Return the native-only bundle plus diagnostics when ambient JSON is invalid.
- [ ] Do not evaluate JavaScript/TypeScript, load modules, or probe networks.

## 5. Expose one resolved declaration payload

- [ ] Add one host response shape containing:
  - merged canonical bundle;
  - resolved block parameter declarations;
  - provenance;
  - non-fatal diagnostics.
- [ ] Preserve existing manifest fields when projecting native descriptors into
  the bundle.
- [ ] Avoid consumer-specific payload pruning.
- [ ] Make Settings and xblox request the same resolved declaration shape.
- [ ] Keep dynamic option values separate from declarations; declarations carry
  only paths/templates and the host supplies the values.

## 6. Complete the shared parameter renderer

- [ ] Make `apps/shared/xblox/ManifestParamFields.tsx` the canonical renderer for
  resolved parameter declarations.
- [ ] Centralize:
  - visibility via `ui.visible_when`;
  - labels, required markers, descriptions, and examples;
  - defaults and placeholders;
  - `options_path` and `options_path_template`;
  - path picker mode;
  - variable-enabled text, numeric, and select controls;
  - generic JSON fallback for unsupported structured schemas.
- [ ] Add host callbacks for loading options and invoking native pickers/actions.
- [ ] Keep host chrome injectable so Settings can use `FieldRow` and xblox can
  retain its property-panel groups.
- [ ] Add a shared registry for specialized/composite controls.
- [ ] Select controls from resolved declaration metadata, never from block or
  parameter names in React.
- [ ] Render an unknown control through a usable generic fallback.

## 7. Share composite controls

- [ ] Add typed UI metadata for:
  - `composite`;
  - `composite_role`;
  - `composite_order`.
- [ ] Group fields only from resolved metadata.
- [ ] Move `VideoInputPickerControl` into `apps/shared`.
- [ ] Keep video device loading and interactive picking behind host callbacks.
- [ ] Render any declared video facets from schema/UI metadata; do not infer
  `camera`, `screen`, or `source` from names.
- [ ] Use the shared `LlmAgentPicker` for declarations grouped as `llm-agent`.
- [ ] Remove `llmGroupFields` and other hardcoded group-name sets.
- [ ] Preserve current option values even when absent from the latest catalog.

## 8. Migrate both consumers

- [ ] Inventory every `ParamKind` and `ui.control` currently rendered by
  `xblox-property-panel.tsx` and `ManifestParamFields.tsx`.
- [ ] Migrate ordinary xblox fields to `ManifestParamFields` first.
- [ ] Migrate flags, argument lists, shortcuts, colors, API keys, editable
  selects, video input, and LLM/provider controls incrementally.
- [ ] Keep only block state, grouping, queries, and host adapters in the xblox
  property panel.
- [ ] Use the same resolved declarations in `XbloxVariablesEditor`, command
  editing, and the run-parameter dialog.
- [ ] Pass complete sibling values for option templates and visibility.
- [ ] Remove duplicate option-template, label, tooltip, visibility, and
  value-formatting implementations after migration.

## 9. Declaration/UI tests

- [ ] Add a canonical-bundle parser fixture.
- [ ] Add native-only, ambient-only, merged, invalid-reference, invalid-version,
  and forbidden-override fixtures.
- [ ] Verify ambient metadata augments a native parameter without dropping
  unrelated native fields.
- [ ] Verify diagnostics do not prevent valid fields from rendering.
- [ ] Verify Settings and xblox select the same widget for the same resolved
  declaration.
- [ ] Verify sibling changes update `visible_when` and
  `options_path_template` identically in both surfaces.
- [ ] Verify LLM preset/router/model render from declaration metadata without
  frontend field-name detection.
- [ ] Verify video input uses the shared control and host adapter.
- [ ] Verify unknown schemas and controls receive generic fallbacks.
- [ ] Verify the ambient loader never executes code.
- [ ] Check edited TypeScript files with the linter.
- [ ] Build embedded apps only when explicitly requested.

## 10. Affected files

### Documentation

- [ ] `docs/xblox/manifest-refactor.md` — implementation checklist and scope.
- [ ] `docs/xblox/xblox-schemas.md` — canonical bundle and ambient declaration
  contract; update only when this subset makes a new shared decision.
- [ ] `docs/xblox/ui-schema.md` — align the existing dynamic UI pipeline with
  resolved ambient/native declarations.
- [ ] `docs/xblox/type-schemas.md` — keep terminology and ownership boundaries
  consistent; do not pull its larger operation-schema work into this refactor.

### Shared declaration types and normalization

- [ ] `apps/shared/xblox/manifest.ts` — mirror the bundle, schema, augmentation,
  provenance, diagnostics, and composite metadata types.
- [ ] `apps/shared/xblox/contextVarParams.ts` — project resolved declarations
  onto scope-variable fields.
- [ ] `apps/shared/xblox/paramLabel.ts` — resolve labels from merged metadata.
- [ ] `apps/shared/xblox/paramHelpText.ts` — resolve descriptions/examples from
  merged metadata.
- [ ] `apps/shared/xblox/paramFieldTooltip.tsx` — display resolved declaration
  help and optional provenance.
- [ ] `apps/shared/web/hostBridge.ts` — type the resolved declaration payload
  shared by embedded consumers.

### Shared parameter rendering

- [ ] `apps/shared/xblox/ManifestParamFields.tsx` — canonical field renderer and
  composite-control dispatch.
- [ ] `apps/shared/xblox/VariablesEditor.tsx` — render resolved variable ParamDefs
  through the canonical renderer.
- [ ] `apps/shared/customCommands/VariableBuilder.tsx` — preserve field identity
  and declaration metadata when creating variables.
- [ ] `apps/shared/components/FlagDisplay.tsx` — shared flags control.
- [ ] `apps/shared/components/provider/LlmAgentPicker.tsx` — shared LLM composite.
- [ ] `apps/shared/components/provider/ProviderModelPicker.tsx` — shared
  provider/model composite.
- [ ] `apps/shared/components/provider/llmCatalog.ts` — host-neutral catalog
  adapter used by the LLM composite.
- [ ] `apps/shared/components/provider/index.ts` — export shared controls.

### Shared command editors

- [ ] `apps/shared/customCommands/types.ts` — resolved declaration fields on
  command/catalog types.
- [ ] `apps/shared/customCommands/cliArgs.ts` — declaration/options loading
  helpers used by Settings.
- [ ] `apps/shared/customCommands/CommandPropsEditor.tsx` — use resolved
  declarations and canonical fields.
- [ ] `apps/shared/customCommands/CommandPickers.tsx` — use canonical fields for
  app-command parameters.

### xblox application

- [ ] `apps/xblox/src/PrototypeApp.tsx` — consume the resolved canonical bundle
  from bootstrap/host data.
- [ ] `apps/xblox/src/xblox/hostBridge.ts` — expose declaration loading and
  invalidation.
- [ ] `apps/xblox/src/xblox/rpc/gateway.ts` — add the resolved-declaration RPC
  client method.
- [ ] `apps/xblox/src/xblox/rpc/client.ts` — cache/query keys for declaration
  payloads.
- [ ] `apps/xblox/src/xblox/react/xblox-property-panel.tsx` — delegate rendering
  to shared fields and remove local video/LLM/name-based branches.
- [ ] `apps/xblox/src/xblox/react/Variables.tsx` — pass resolved declarations and
  sibling values.
- [ ] `apps/xblox/src/xblox/react/BlockModel.ts` — index resolved block
  declarations rather than UI-local shapes.
- [ ] `apps/xblox/src/xblox/react/BlockHelpPanel.tsx` — read merged help/schema
  metadata.
- [ ] `apps/xblox/src/xblox/react/xblox-property-contrib.tsx` — move reusable
  field controls to the shared registry.
- [ ] `apps/xblox/src/xblox/react/builder/components/BuilderShell.tsx` — pass the
  resolved bundle and host adapters into the property panel.
- [ ] `apps/xblox/src/xblox/react/builder/useBuilderController.tsx` — retain
  declaration state and sibling values.
- [ ] `apps/xblox/src/xblox/react/builder/builderStore.ts` — remove
  property-panel-owned declaration/control types.
- [ ] `apps/xblox/src/xblox/react/builder/builderTypes.ts` — use shared
  declaration and host-adapter types.
- [ ] `apps/xblox/src/xblox/prototype/mockHostBridge.ts` — provide a resolved
  bundle in standalone development.
- [ ] `apps/xblox/rspack.config.js` — inspect only if new shared module paths
  require bundler changes.

### Settings application

- [ ] `apps/settings/src/settings/customCommands/CommandEditor.tsx` — replace
  ad-hoc parameter/LLM rendering with resolved shared declarations.
- [ ] `apps/settings/src/settings/customCommands/CommandParameterDialog.tsx` —
  use the same declarations and controls as the property panel.
- [ ] `apps/settings/src/settings/customCommands/CustomCommandsSettingsPanel.tsx`
  — load/pass the resolved declaration payload.
- [ ] `apps/settings/src/settings/customCommands/FormControls.tsx` — retain only
  Settings chrome supplied to the shared renderer.
- [ ] `apps/settings/src/main.tsx` — initialize declaration loading if it is not
  included in the command payload.
- [ ] `apps/shared/settings/hostRpc.ts` — add the Settings declaration RPC.
- [ ] `apps/shared/settings/commandsClient.ts` — preserve resolved declarations
  in command catalog responses.
- [ ] `apps/shared/settings/devRpc.ts` — mock declaration responses.
- [ ] `apps/settings/rspack.config.js` — inspect only if new shared module paths
  require bundler changes.

### Native declaration serialization and host delivery

- [ ] `src/xblox/blocks/block_registry.hpp` — serialize native descriptors into
  the canonical bundle subset without dropping fields.
- [ ] `src/xblox/blocks/block_types.hpp` — add declaration/schema types only if
  they cannot remain JSON payload types.
- [ ] `src/xblox/blocks/block_params.hpp` — preserve schema references and
  composite UI metadata in `ParamDef` serialization.
- [ ] `src/xblox/blocks/block_params_ui.hpp` — declaration/options host API
  declarations.
- [ ] `src/xblox/blocks/block_params_ui.cpp` — keep dynamic option paths aligned
  with declarations; do not add execution behavior.
- [ ] `src/xblox/blocks/llm_blocks.cpp` — native LLM declaration metadata where
  it should be trusted rather than ambient.
- [ ] `src/xblox/blocks/video_blocks.cpp` — native video declaration metadata
  where it should be trusted rather than ambient.
- [ ] `src/win/ui_next/CBlockView.hpp` — ambient loader/cache and RPC declarations.
- [ ] `src/win/ui_next/CBlockView.cpp` — locate `.xblox.schemas.json`, merge it
  with native declarations, and return provenance/diagnostics.
- [ ] `src/win/ui_next/CSettingsWebView.cpp` — route the same declaration payload
  to Settings.
- [ ] `src/win/web/CWebViewManager.cpp` — inspect/update shared WebView RPC
  dispatch if the declaration method is registered centrally.
- [ ] `src/core/web_command_host.hpp` — type declaration payload additions if
  they travel with command catalogs.
- [ ] `src/core/web_command_host.cpp` — preserve declaration payload additions.
- [ ] `src/win/custom_commands_host.hpp` — preserve declarations in the Windows
  command-host wrapper.
- [ ] `src/commands/parameter_def.hpp` — keep app-command parameter serialization
  compatible with the shared renderer.
- [ ] `src/commands/command_registry.hpp` — expose compatible command parameter
  declarations.
- [ ] `src/commands/command_registry.cpp` — preserve declarations in command
  catalog serialization.
- [ ] `src/commands/value_kind.hpp` — keep app `ValueKind` fallback mapping
  compatible with `ParamKind`.
- [ ] `src/cli/pm_image_cmd_info.cpp` — expose the same bundle subset through
  xblox information output if that output is part of the declaration API.
- [ ] `src/cli/pm_image_cmd_xblox.cpp` — preserve the canonical declaration
  payload in xblox CLI responses.
- [ ] `CMakeLists.txt` — register new native declaration-loader sources.

### Planned new shared files

- [ ] `apps/shared/xblox/schemaBundle.ts` — canonical JSON Schema/bundle subset
  types and bundle-version validation.
- [ ] `apps/shared/xblox/normalizeNativeManifest.ts` — native manifest to
  canonical bundle normalization.
- [ ] `apps/shared/xblox/mergeDeclarations.ts` — deterministic native/ambient
  merge and explicit override policy.
- [ ] `apps/shared/xblox/resolveDeclarations.ts` — `$ref` resolution, provenance,
  and non-fatal diagnostics.
- [ ] `apps/shared/xblox/controlRegistry.tsx` — ordinary/composite control
  registration and generic fallback.
- [ ] `apps/shared/xblox/CompositeParamGroup.tsx` — metadata-driven grouping.
- [ ] `apps/shared/xblox/VideoInputPickerControl.tsx` — host-neutral shared video
  control.
- [ ] `apps/shared/xblox/JsonSchemaParamEditor.tsx` — generic structured-value
  fallback.

### Planned new native files

- [ ] `src/xblox/blocks/schema_bundle.hpp` — canonical bundle data contract.
- [ ] `src/xblox/blocks/schema_bundle.cpp` — native manifest bundle projection.
- [ ] `src/xblox/blocks/ambient_schemas.hpp` — safe ambient file loader API.
- [ ] `src/xblox/blocks/ambient_schemas.cpp` — parsing, limits, path identity,
  and cache invalidation.
- [ ] `src/xblox/blocks/declaration_merge.hpp` — native/ambient merge API if the
  host performs merging.
- [ ] `src/xblox/blocks/declaration_merge.cpp` — merge, provenance, and
  diagnostics if the host performs merging.
- [ ] Choose exactly one merge implementation boundary: native host or shared
  TypeScript. Do not maintain independent merge algorithms in both.

### Tests and fixtures

- [ ] `apps/shared/xblox/__tests__/mergeDeclarations.test.ts` — merge,
  precedence, references, overrides, and diagnostics.
- [ ] `apps/shared/xblox/__tests__/ManifestParamFields.test.tsx` — widget parity
  and fallback behavior.
- [ ] `tests/xblox/schemas/native-only.json` — native projection fixture.
- [ ] `tests/xblox/schemas/ambient-only.json` — ambient declaration fixture.
- [ ] `tests/xblox/schemas/merged.json` — expected merged bundle.
- [ ] `tests/xblox/schemas/invalid-ref.json` — unresolved `$ref` diagnostic.
- [ ] `tests/xblox/schemas/invalid-version.json` — unsupported version diagnostic.
- [ ] `tests/xblox/schemas/forbidden-override.json` — native replacement policy.
- [ ] `tests/xblox/.xblox.schemas.json` — project ambient integration fixture.
- [ ] `tests/xblox/video-capture-cam.xblox` — shared video-control smoke fixture.
- [ ] `tests/xblox/ai/agent-local.xblox` — shared LLM-control smoke fixture.
- [ ] `tests/orchestrator/test-xblox-next.mjs` — canonical payload and ambient
  merge integration assertions.
- [ ] `tests/orchestrator/test-xblox-language.mjs` — xblox info declaration
  serialization smoke.
- [ ] `tests/orchestrator/test-xblox-ui.mjs` — property-panel and Settings widget
  parity smoke.
- [ ] `tests/orchestrator/test-xblox-controllers.mjs` — declaration kind/control
  registration checks where still applicable.
- [ ] `tests/orchestrator/test-sandbox.mjs` — update xblox information snapshots
  only if the payload changes.
- [ ] `tests/xblox-ui-dump-sample.json` — update only if rendered structure
  intentionally changes.
- [ ] `package.json` — add or extend the focused declaration/UI test command.

## Completion criteria

- [ ] `.xblox.schemas.json` can augment native parameter schema/UI declarations
  without recompiling C++.
- [ ] Native and ambient sources normalize into the canonical bundle envelope
  defined by `xblox-schemas.md`.
- [ ] Every resolved declaration records its source and override state.
- [ ] Settings and xblox render a resolved parameter through the same shared code.
- [ ] Adding ordinary declaration metadata requires no Settings- or
  property-panel-specific rendering branch.
- [ ] No xblox execution or runtime behavior is changed by this refactor.
