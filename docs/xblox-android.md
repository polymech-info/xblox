# XBlox for Android — Open-source references

## Projects to investigate

| Project | Relevance to XBlox | Priority |
|---|---|---|
| [Easymatic](https://easymatic.app/) | Android visual editor, triggers, AI providers, widgets, plugins and inter-app invocation. Advertises MIT licensing. | First |
| [OpenTasker](https://github.com/SysAdminDoc/OpenTasker) | Native Android automation runtime, conditions/actions, permission handling, simulation and diagnostics. | First |
| [Node-RED](https://nodered.org/about/) | Browser-based editor, separate execution runtime, deployment, reusable flows and extensible nodes. | Architecture |
| [Activepieces](https://github.com/activepieces/activepieces) | Service connectors, AI workflows and server-side integrations. Community Edition is MIT; enterprise features use separate licensing. | Integrations |
| [Droidline](https://github.com/KnifeLemon/Droidline) | Desktop-to-phone control through SDKs, HTTP and MCP. MIT; early-stage with limited device validation. | Explore |

These are investigation candidates, not audited dependencies.

## Product benchmarks

- [Automate](https://llamalab.com/automate/): Android flowchart UX, 400+ blocks, shared workflows and Locale/Tasker plugin support.
- [IFTTT](https://ifttt.com/): still active, including Android integrations and AI workflows. Useful for onboarding, templates and trigger/action simplicity; not an open-source foundation.
- Consider supporting Locale/Tasker plugins to access existing integrations.

## Recommended direction

- Inspect Easymatic for the mobile editor and OpenTasker for native execution.
- Study Node-RED’s editor/runtime separation for desktop-to-phone deployment.
- Retain XBlox’s workflow format.
- Make execution targets explicit: phone, paired desktop or server.
- Prioritize portable AI and multimedia workflows.
- Keep triggers, conditions, branches and loops deterministic.
- Use LLM blocks where interpretation or generation is needed.
- Validate maintenance, licenses and real-device behavior before adopting code.

## Dynamic UI

The Android host is Kotlin and Jetpack Compose with Material 3, the same client as Tanit chat. A flow or a model may describe a surface. The app renders that description with a fixed set of Material components. It does not compile or execute generated Kotlin, and it does not show model HTML in a WebView.

Two producers, one document:

| Producer | What it emits | When |
|---|---|---|
| XBlox blocks | A UI tree, filled with the existing reference grammar (`${doc.field}`, conditions) | Layout and copy are known ahead of time |
| LLM block | The same tree, limited to the catalog in its prompt | Layout or copy depends on the turn |

Actions are names (`save`, `retry`). The host turns a tap back into a flow event. Branches, loops and device calls stay in blocks. The document cannot start an arbitrary Android intent or call an unlisted tool.

The desktop property panel in [ui-schema.md](./ui-schema.md) describes block parameters (`ui.control`). That schema is an editor. It is not the phone surface. Reuse control names where they match (`text`, `select`, `switch`, `slider`). Do not send the property-panel document to the device.

Material 3 Adaptive (`androidx.compose.material3.adaptive`) is window-size layout for screens we write by hand. It is a separate problem from a document the flow or a model emits.

### Catalog for now

A Kotlin registry maps a component name to a Material 3 composable. Color and type come from the app `MaterialTheme`, including dynamic color. The document picks a role (`primary`, `titleLarge`), not a raw color.

| Name | Material 3 | Notes |
|---|---|---|
| `Column`, `Row` | `Column`, `Row` | Spacing is a token (`sm`, `md`, `lg`), not a dp value |
| `Card` | `Card` | Groups a form or a result |
| `List` | `LazyColumn` | Repeated row from an array binding |
| `Text` | `Text` | `variant` maps to a Material type role |
| `Image` | `AsyncImage` | Host allow-list for URLs; flow-produced files are local |
| `TextField` | `OutlinedTextField` | Writes a scope variable on submit or change |
| `Switch`, `Slider` | `Switch`, `Slider` | Bound to a variable |
| `Button` | `Button` / `TextButton` | `action` is a flow event name |
| `Progress` | `LinearProgressIndicator` | Bound or indeterminate |

Unknown names render an inline error and are reported back to the producer. They never crash the surface and never fall through to code execution.

### Options

Checked October 2026. Investigation candidates, not dependencies.

| Project | Fit | License / maturity | Priority |
|---|---|---|---|
| [A2UI](https://github.com/a2ui-project/a2ui) + [Compose renderer](https://developer.android.com/develop/ui/compose/agentic) | Agent sends JSON messages (`createSurface`, `updateComponents`, `updateDataModel`, `deleteSurface`). Client renders a catalog. AndroidX maps the basic catalog to Material 3 (`androidx.a2ui.compose`, `androidx.compose.material3:material3-a2ui`). User actions return to the agent. Malformed or unknown components are rejected. | Apache-2.0. Protocol v0.9 is stable; v0.9.1 is what the Compose renderer speaks; v1.0 is a release candidate. Compose artifacts are `1.0.0-alpha01` (23 Sep 2026). | First for LLM surfaces that stream |
| [DivKit](https://divkit.tech/docs/en/) | Server or tool sends a JSON card. Android renders it natively, including a [Jetpack Compose module](https://divkit.tech/docs/en/quickstart/compose) (`com.yandex.div:compose`). Variables, expressions, states and custom actions are built in. A Kotlin DSL can author cards off-device. | Apache-2.0. Production-used. Compose renderer is real. Styling is DivKit's own, not `MaterialTheme`; a Material palette has to be injected as variables. | First for rich cards authored by blocks |
| [Ketoy](https://github.com/KetoyDev/ketoy) | Kotlin DSL compiles to a wire payload and renders as Compose Material 3 (`Column`, `Button`, `Card`, `Scaffold`, theme tokens, navigation). | Apache-2.0. Early. minSdk 26. The format is Ketoy's binary, not a model-friendly JSON catalog. | Reference for a Material registry |
| [json-render](https://json-render.dev/docs/catalog) | Catalog of components and actions is the guardrail. `catalog.prompt()` turns that catalog into the model prompt. Same JSON spec, many renderers. | Apache-2.0. Mobile renderer is React Native (`@json-render/react-native`), not Kotlin. | Copy the catalog-to-prompt idea only |
| [OpenUI Lang](https://github.com/thesysdev/openui) | Compact streaming syntax aimed at models. Official runtimes are React, Vue, Svelte and Angular. | Kotlin / Compose support is an unmerged pull request ([#614](https://github.com/thesysdev/openui/pull/614), June 2026). | Watch |
| [Adaptive Cards](https://adaptivecards.io/) | JSON cards with an Android renderer (`io.adaptivecards:adaptivecards-android`). Good for a bubble, weak for an app screen. | Source is MIT. The published Android binary is under the Adaptive Cards binary EULA. Renderer targets Views and Material components, not Compose. | Skip for screens |
| [Beagle](https://github.com/zup-archive/beagle-android) | Older Kotlin server-driven UI. | Apache-2.0. Repository archived; last real work predates Compose as the host UI. | Skip |

### Direction

- Own the document. A block writes it, or an LLM block writes it after catalog validation. Kotlin only renders the closed list above.
- Bind text, visibility and field values with the expression grammar already used by templates and conditions. Do not invent a second template language on the phone.
- Keep the first payload one tree per surface. Move to A2UI messages when a chat turn must paint the surface while tokens are still arriving. Until then, do not take the alpha AndroidX artifacts as a dependency.
- Use DivKit only if block-authored cards outgrow the table above (galleries, states, timers). Map its actions into the same flow events. Do not adopt its theme.
- Read Ketoy for how a Material 3 registry and theme tokens look. Do not adopt its wire format.
- Generate the LLM system prompt from the catalog, the way json-render does. Reject trees that name a component or action outside that list, then ask the model to repair or drop the node.
- Do not generate Kotlin, do not embed a WebView for model HTML, and do not pull in React Native to gain a renderer.

### Sketch

A deterministic block:

```json
{
  "kind": "ui",
  "surface": "main",
  "catalog": "material3",
  "tree": {
    "type": "Column",
    "children": [
      { "type": "Text", "text": "${vision.caption}", "variant": "titleLarge" },
      { "type": "Button", "label": "Save", "action": "save" }
    ]
  }
}
```

An LLM block receives the same `catalog` and must return a `tree` in that shape. A tap with `"action": "save"` resumes the flow with that name in scope, the same way a condition reads `PREVIOUS`. The model does not see the click handler.
