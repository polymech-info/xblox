# XBlox for Android — Architecture, runtime and open-source references

Background material for desktop-authored workflows deployed to Android, with optional execution on a paired desktop or server.

This document combines architectural direction, investigation candidates and platform background. Project descriptions supplied for the October 2026 investigation are not dependency audits. Chip information reflects manufacturer announcements checked in November 2026; performance claims are not independent benchmarks.

## Product direction

- Retain XBlox’s workflow format.
- Make execution targets explicit: phone, paired desktop or server.
- Use Kotlin, Jetpack Compose and Material 3 for the Android host.
- Reuse portable C++ engine code where it provides shared functionality or useful performance.
- Keep triggers, conditions, branches and loops deterministic.
- Use LLM blocks where interpretation or generation is needed.
- Prioritize portable AI and multimedia workflows.
- Separate workflow execution from UI rendering.
- Treat permissions, lifecycle, cancellation and execution diagnostics as runtime concerns.
- Validate maintenance, licensing and real-device behavior before adopting external code.

## Projects to investigate

| Project | Relevance to XBlox | Priority |
|---|---|---|
| [Easymatic](https://easymatic.app/) | Android visual editor, triggers, AI providers, widgets, plugins and inter-app invocation. Advertises MIT licensing. | First |
| [OpenTasker](https://github.com/SysAdminDoc/OpenTasker) | Native Android automation runtime, conditions/actions, permission handling, simulation and diagnostics. | First |
| [Node-RED](https://nodered.org/about/) | Browser-based editor, separate execution runtime, deployment, reusable flows and extensible nodes. | Architecture |
| [Activepieces](https://github.com/activepieces/activepieces) | Service connectors, AI workflows and server-side integrations. Community Edition is MIT; enterprise features use separate licensing. | Integrations |
| [Droidline](https://github.com/KnifeLemon/Droidline) | Desktop-to-phone control through SDKs, HTTP and MCP. Advertises MIT licensing; investigate maturity and device coverage. | Explore |

These are investigation candidates, not audited dependencies.

### Product benchmarks

- [Automate](https://llamalab.com/automate/): Android flowchart UX, broad block catalog, shared workflows and Locale/Tasker plugin support.
- [IFTTT](https://ifttt.com/): onboarding, templates and simple trigger/action composition. Not an open-source foundation.
- Consider Locale/Tasker plugin compatibility to access existing integrations.

### What to inspect

- Easymatic: mobile editing, plugin boundaries and invocation.
- OpenTasker: native execution, permissions, simulation and diagnostics.
- Node-RED: editor/runtime separation and deployment.
- Activepieces: connector contracts and credential handling.
- Droidline: desktop-to-phone control and capability exposure.

## Android host and portable engine

Suggested ownership:

| Layer | Responsibilities |
|---|---|
| Kotlin / Compose host | Screens, dynamic surfaces, permissions, Android services, notifications and lifecycle integration |
| Portable C++ engine | Reusable execution logic, protocols, VFS logic, parsers, DSP and supported inference backends |
| Platform adapters | Android storage, camera, microphone, codecs, scheduling and hardware-specific execution |
| JNI boundary | Small interface for requests, buffers, events, cancellation and results |
| Remote execution transport | Paired desktop/server invocation, capability negotiation, authentication and result delivery |

C++ is valuable primarily for shared implementation and control over expensive processing. It is not automatically faster for UI, ordinary application logic or calls into existing Android services.

Native code has the same application permissions and background-execution restrictions as managed code. It does not provide unrestricted filesystem access or keep a process alive.

### JNI boundary

- Prefer complete operations and batches over many small calls.
- Keep ownership and buffer lifetimes explicit.
- Avoid passing individual pixels, audio samples or file entries through repeated calls.
- Do not retain `JNIEnv*` across threads.
- Use appropriate global references for managed objects retained by native code.
- Attach native-created threads before calling managed code.
- Define shutdown and cancellation behavior.
- Translate failures at the boundary; do not allow C++ exceptions to escape through JNI.
- A native crash can terminate the entire host process. JNI is not process isolation.

## CPU architecture and compilation

Android is not exclusively ARM.

| ABI | Relevance |
|---|---|
| `arm64-v8a` | Main target for modern Android phones and tablets |
| `armeabi-v7a` | Older 32-bit ARM devices, if deliberately supported |
| `x86_64` | Intel/AMD emulators and some ChromeOS environments |
| `x86` | Older 32-bit Intel environments |

Start with `arm64-v8a` and `x86_64` unless product requirements justify other targets. Build native dependencies separately for every supported ABI.

### Kotlin and C++ ultimately execute machine instructions

| Source | Execution path |
|---|---|
| C++ | NDK compiler produces machine code for the selected ABI |
| Kotlin | Compiler produces DEX bytecode; ART interprets it or compiles it into machine code |

ARM CPUs execute ARM instructions, not DEX bytecode. Kotlin does not need to become C++ first.

On an ARM64 device, compiled Kotlin and C++ can both use AArch64 arithmetic, load/store, branch and floating-point instructions.

Differences arise from:

- Compiler optimizations.
- Bounds checks and runtime checks.
- Object allocation and indirection.
- Memory layout.
- Vectorization.
- Explicit SIMD intrinsics available to native code.

C++ offers direct access to ARM NEON intrinsics. Kotlin relies on runtime compiler optimizations or optimized libraries. A simple loop can produce similar code in both languages; a specialized native kernel may perform substantially less work.

Linux-compatible C++ source is useful, but Android still requires an NDK build. Android uses Bionic rather than glibc, and platform APIs and filesystem assumptions differ.

## Memory allocation and ownership

Both Kotlin and C++ use the device’s physical RAM. They differ in allocation interfaces and lifetime management.

| Allocation | Ownership and lifetime |
|---|---|
| Kotlin `ByteArray` | Managed object; ART and the garbage collector manage reclamation |
| C++ `malloc` | Native allocation; must be released with `free` |
| C++ RAII container | Native storage released by its owning object |
| Direct `ByteBuffer` | Managed view over directly accessible backing storage; lifetime depends on its allocation and ownership |
| `HardwareBuffer` / `AHardwareBuffer` | Hardware-compatible allocation with explicit usage, access and lifetime rules |

Neither managed allocation nor `malloc` necessarily requests fresh memory from the operating system on every call. Allocators can reuse memory they already manage.

A Java/Kotlin byte array exposed to native code may require pinning or copying. A suitable direct buffer can provide access to the same backing memory without copying its contents.

Wrapping externally allocated native memory does not transfer ownership automatically. The owner must keep it alive until every user has finished.

### Shared CPU/GPU memory

Typical Android phone SoCs use shared physical RAM for the CPU, GPU and media hardware, broadly similar to Apple Silicon’s unified-memory architecture.

This does not imply that every allocation is immediately usable by every processor.

| Buffer | CPU access | GPU / media hardware |
|---|---|---|
| Kotlin byte array | Yes | Not automatically a usable hardware buffer |
| C++ allocation | Yes | Not automatically a usable hardware buffer |
| Compatible hardware buffer | If its usage and format permit CPU mapping | Can be shared by supported components |

Sharing requires compatible formats, usage flags and synchronization.

Hardware buffers may use tiled, compressed or implementation-specific layouts. Shared physical memory does not guarantee a simple linear pixel array or identical virtual addresses.

### Video example

An efficient display path can work as follows:

1. Decoder writes a frame into a hardware buffer.
2. The graphics/display system receives a handle to that buffer.
3. The consumer reads the shared allocation.
4. Synchronization confirms completion.
5. The buffer returns to the pool.

Passing the handle does not copy the image.

Requesting a CPU-readable RGBA array can introduce conversion or copying. Moving to C++ does not remove those operations automatically.

A 1920 × 1080 RGBA frame contains approximately 8.3 MB. One full-frame copy at 60 fps represents approximately 498 MB/s of copied payload, with additional memory traffic for reads and writes.

For multimedia blocks, track:

- Buffer location and format.
- Ownership and lifetime.
- CPU mapping requirements.
- Copies and conversions.
- Synchronization.
- Queue depth and backpressure.

### Android compatibility

Support 16 KB memory pages across all native libraries, including third-party binaries. Avoid hardcoded assumptions that the system page size is 4096 bytes.

## AI hardware background

“AI hardware” includes several distinct execution resources.

| Resource | Purpose | XBlox implication |
|---|---|---|
| CPU | General execution and portable inference | Baseline implementation and fallback |
| GPU | Parallel numerical processing | Useful for supported inference and image workloads |
| GPU matrix accelerators | Specialized matrix operations within the GPU | Requires a backend using the relevant APIs |
| NPU / Neural Engine | Efficient execution of supported neural-network operations | Requires compatible models, operators and runtime |
| Low-power sensing hardware | Continuous sensor/audio analysis | Availability to third-party apps must be verified |
| ISP and media engines | Camera processing and video encode/decode | Use platform pipelines rather than reproducing them on the CPU |

An NPU does not execute ordinary ARM C++ code. A runtime submits supported operations to its hardware-specific execution system.

The presence of a low-power sensing processor does not establish that XBlox can deploy an arbitrary wake-word or whistle model to it.

### Last two flagship chip generations

Manufacturer information checked in November 2026. Android examples cover Qualcomm and MediaTek; this is not an exhaustive survey of Tensor or Exynos.

| Family | 2025 generation | 2026 generation |
|---|---|---|
| Apple iPhone Pro | **A19 Pro:** 16-core Neural Engine; neural accelerators inside its six GPU cores | **A20 Pro:** dual 16-core Neural Engines, seven-core GPU and claimed 50% greater memory bandwidth |
| Qualcomm | **Snapdragon 8 Elite Gen 5:** Hexagon NPU with scalar, vector and tensor acceleration; claimed 37% NPU performance increase over its predecessor | **Snapdragon 8 Elite Gen 6 / Extreme Gen 6:** updated Hexagon NPU; Extreme advertises AI-dedicated GPU cores and support for 30B+ mixture-of-experts models |
| MediaTek | **Dimensity 9500:** NPU 990, low-power compute-in-memory NPU and advertised BitNet 1.58-bit support | **Dimensity 9600 Pro:** NPU 1090, updated low-power NPU, LPDDR6 support; claimed 51% faster LLM prefill and 55% greater token generation per watt |

These figures use different workloads and baselines. They must not be used as a cross-vendor performance ranking.

Claims about supported model sizes do not establish usable speed or memory consumption. At four bits per stored parameter, 30 billion weights alone require approximately 15 GB, before metadata, activations, context caches and runtime overhead. MoE active parameter count and total stored parameter count are different quantities.

### Software access

- Ordinary C++ inference does not automatically use an NPU.
- A model file format does not guarantee acceleration.
- Operator coverage, quantization, tensor shapes and runtime versions matter.
- Partial acceleration may introduce transfers and synchronization that reduce the benefit.
- Shared RAM does not eliminate backend-specific layout conversions or duplicate allocations.
- Validate the actual execution backend; do not infer it from successful inference.

Android’s LiteRT `CompiledModel` API provides CPU/GPU/NPU execution paths. Qualcomm supplies an AI Engine Direct delegate. Support remains dependent on the runtime, device and model.

NNAPI was deprecated in Android 15. Do not make it the foundation of new acceleration work.

Apple exposes GPU neural accelerators through Metal tensor APIs. Neural Engine execution is a separate backend concern.

### XBlox inference direction

- Keep model execution behind a backend interface.
- Preserve a portable CPU path.
- Add GPU/NPU execution for validated combinations.
- Record selected device, backend and fallback reason.
- Benchmark startup, time to first result, sustained throughput, memory and energy use.
- Distinguish short interactive jobs from continuous processing.
- Route unsuitable workloads to a paired desktop or server according to explicit policy.
- Do not silently upload local inputs when local acceleration fails.

Small classifiers, embeddings and tool-routing models are useful initial workloads. Their suitability should be established with the actual model and representative devices.

## Execution targets and capabilities

A block’s meaning should remain stable across targets. Its implementation may differ.

| Concern | Required behavior |
|---|---|
| Target | Explicit phone, desktop or server selection |
| Capabilities | Discover supported operations and backends |
| Permissions | Request through the appropriate host before execution |
| Cancellation | Propagate through local and remote work |
| Retry | Account for idempotency and external side effects |
| State | Separate durable flow state from transient buffers and handles |
| Diagnostics | Report execution target, backend, timings and failures |
| Data transfer | Make movement of files, audio and images explicit |
| Lifecycle | Define behavior when the app stops, disconnects or loses permission |

Hardware handles and process pointers are local resources. Remote execution requires an explicit transport representation.

## Dynamic UI

The Android host is Kotlin and Jetpack Compose with Material 3, shared with the Tanit chat client.

A flow or model describes a surface. The app renders it with a fixed component catalog. It does not compile generated Kotlin or display model-generated HTML in a WebView.

### Two producers, one document

| Producer | Output | Use |
|---|---|---|
| XBlox blocks | UI tree populated with the existing reference grammar, such as `${doc.field}` | Layout and copy are known ahead of time |
| LLM block | The same tree, constrained to its supplied catalog | Layout or copy depends on the turn |

Actions are names such as `save` and `retry`. The host converts a tap into a flow event.

Branches, loops and device calls remain in blocks. A document cannot start an arbitrary Android intent or call an unlisted tool.

### Property editor versus runtime surface

The desktop property panel in `ui-schema.md` describes block parameters through `ui.control`. It is an editor schema, not the phone surface.

Reuse control names where appropriate, such as `text`, `select`, `switch` and `slider`. Do not send the property-panel document to the device as a runtime surface.

Material 3 Adaptive addresses window-size layouts for app screens. It is separate from the document emitted by a block or model.

### Initial catalog

A Kotlin registry maps component names to composables. Colors and typography come from `MaterialTheme`, including dynamic color.

Documents choose semantic roles such as `primary` and `titleLarge`, rather than arbitrary styling values.

| Name | Implementation | Notes |
|---|---|---|
| `Column`, `Row` | Compose layout primitives | Spacing tokens: `sm`, `md`, `lg` |
| `Card` | Material 3 `Card` | Groups forms or results |
| `List` | `LazyColumn` | Repeated rows from an array binding |
| `Text` | Material 3 `Text` | `variant` maps to a typography role |
| `Image` | Coil `AsyncImage` or host image adapter | Validate URLs and access to local flow-produced files |
| `TextField` | `OutlinedTextField` | Updates scoped state on change or submit |
| `Switch`, `Slider` | Material 3 controls | Bound to scoped variables |
| `Button` | `Button` / `TextButton` | `action` names a flow event |
| `Progress` | `LinearProgressIndicator` | Bound progress or indeterminate |

`Column`, `Row` and `LazyColumn` are Compose layout primitives; `AsyncImage` is not a Material 3 component.

Unknown names produce an inline error and a diagnostic. They must not crash the surface or fall through to code execution.

Validate component types, property types, bindings, actions, depth, node count and payload size before rendering.

### Renderer candidates

The following preserves the October 2026 investigation direction. Recheck exact versions, licensing and platform support before adoption.

| Project | Fit | Status / caveat | Priority |
|---|---|---|---|
| [A2UI](https://github.com/a2ui-project/a2ui) and [Compose renderer](https://developer.android.com/develop/ui/compose/agentic) | Catalog-based surfaces, incremental updates and actions returned to the producer | Apache-2.0; verify protocol compatibility and AndroidX artifact maturity | First for streaming LLM surfaces |
| [DivKit](https://divkit.tech/docs/en/) | Rich JSON cards, variables, expressions, states and actions; [Compose integration](https://divkit.tech/docs/en/quickstart/compose) | Apache-2.0; uses its own styling system rather than `MaterialTheme` | First for richer block-authored cards |
| [Ketoy](https://github.com/KetoyDev/ketoy) | Kotlin DSL and Compose Material 3 renderer | Early; investigate wire format and minimum SDK | Reference for a Material registry |
| [json-render](https://json-render.dev/docs/catalog) | Component/action catalog and catalog-generated model prompt | Mobile renderer is React Native rather than Kotlin | Reuse catalog-to-prompt concept |
| [OpenUI Lang](https://github.com/thesysdev/openui) | Compact streaming syntax for model-generated interfaces | Verify Kotlin/Compose support; previously tracked through [PR #614](https://github.com/thesysdev/openui/pull/614) | Watch |
| [Adaptive Cards](https://adaptivecards.io/) | JSON cards and Android rendering | Check source versus binary licensing; Android renderer is not Compose-native | Consider for cards, not screens |
| [Beagle](https://github.com/zup-archive/beagle-android) | Older Kotlin server-driven UI | Archived repository | Skip |

### Direction

- Own the XBlox surface document.
- Use the existing expression grammar for text, visibility and bindings.
- Keep the first payload as one validated tree per surface.
- Consider A2UI messages when incremental rendering becomes necessary.
- Use DivKit only if richer block-authored cards justify the additional renderer.
- Map all renderer actions into the same flow-event mechanism.
- Generate the LLM prompt from the supported catalog.
- Reject unsupported components and actions; request repair or apply an explicit drop policy.
- Keep generated code and arbitrary intents outside the surface format.
- Do not introduce React Native solely to obtain a renderer.

### Sketch

A deterministic block emits:

```json
{
  "kind": "ui",
  "surface": "main",
  "catalog": "material3",
  "tree": {
    "type": "Column",
    "children": [
      {
        "type": "Text",
        "text": "${vision.caption}",
        "variant": "titleLarge"
      },
      {
        "type": "Button",
        "label": "Save",
        "action": "save"
      }
    ]
  }
}
```

An LLM block receives the same catalog and returns a tree in that shape.

The host binds `save` to an allowed event for that surface. A tap resumes the associated flow with the event name and validated input values. The model does not receive or implement the click handler.

## Suggested investigation order

1. Establish the Android host / portable engine boundary.
2. Inspect Easymatic and OpenTasker with a small executable workflow.
3. Study Node-RED deployment and runtime separation.
4. Implement the minimal Material surface registry.
5. Validate one audio and one video workflow, including buffer ownership and cancellation.
6. Benchmark one small inference model on CPU.
7. Add one accelerated backend and compare complete workload behavior.
8. Test remote execution, disconnects and recovery.
9. Revisit richer rendering and streaming protocols when concrete flows require them.

## References

### Android runtime and memory

- [Android ABIs](https://developer.android.com/ndk/guides/abis)
- [Android Runtime](https://source.android.com/docs/core/runtime)
- [JNI tips](https://developer.android.com/ndk/guides/jni-tips)
- [MediaCodec](https://developer.android.com/reference/android/media/MediaCodec)
- [AHardwareBuffer](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [BufferQueue and Gralloc](https://source.android.com/docs/core/graphics/arch-bq-gralloc)
- [16 KB page sizes](https://developer.android.com/guide/practices/page-sizes)

### AI runtimes and hardware

- [LiteRT for Android](https://developers.google.com/edge/litert/android)
- [LiteRT NPU delegates](https://developers.google.com/edge/litert/android/npu/overview)
- [NNAPI deprecation](https://developer.android.com/ndk/guides/neuralnetworks)
- [Apple GPU neural accelerators](https://developer.apple.com/videos/play/tech-talks/111432/)
- [Apple A19 Pro announcement](https://www.apple.com/es/newsroom/2025/09/apple-unveils-iphone-17-pro-and-iphone-17-pro-max-the-most-powerful-and-advanced-pro-models-ever/)
- [Apple A20 Pro announcement](https://www.apple.com/newsroom/2026/09/apple-debuts-iphone-18-pro-and-iphone-18-pro-max/)
- [Qualcomm Hexagon](https://www.qualcomm.com/processors/hexagon)
- [Snapdragon 8 Elite Gen 5](https://www.qualcomm.com/news/releases/2025/09/snapdragon-8-elite-gen-5--the-world-s-fastest-mobile-system-on-a)
- [Snapdragon Gen 6 announcement](https://www.qualcomm.com/news/releases/2026/09/snapdragon-leads-the-agentic-ai-age-with-two-of-the-world-s-fast)
- [MediaTek Dimensity 9500](https://www.mediatek.com/products/smartphones/mediatek-dimensity-9500)
- [MediaTek Dimensity 9600 Pro](https://www.mediatek.com/press-room/mediatek-dimensity-9600-pro-sets-new-standard-for-flagship-smartphone-chips)

### Agent execution

- [Code execution with MCP — Anthropic, "Code Mode"](https://www.anthropic.com/research/code-execution-with-mcp)

