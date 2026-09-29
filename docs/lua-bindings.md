\# XBlox Lua + C++26 Reflection



\## Goal



Add a Lua representation for XBlox workflows with reliable round-tripping, strong editor support, and minimal duplicated schema/binding code.



The intended direction is:



```text

XBlox

&#x20; ↕

XBlox structural IR

&#x20; ↕

Lua

```



C++26 reflection should become the lowest structural layer for native C++ APIs, while the existing XBlox/UI schema remains responsible for presentation and runtime/domain semantics.



\---



\## Lua representation



Do not emit large positional calls.



XBlox blocks with many parameters should map to Lua table arguments:



```lua

videoCapture({

&#x20;   action = "Record",

&#x20;   input = target,



&#x20;   width = 1080,

&#x20;   height = 1080,

&#x20;   fps = 24,

&#x20;   bitrateKbps = 18000,



&#x20;   audioSource = "desktop",



&#x20;   follow = "cursor",

&#x20;   followDeadzone = 3,

&#x20;   followSpeed = 0.01,



&#x20;   interactiveZoom = true,

&#x20;   zoomMax = 3,

&#x20;   zoomStep = 0.15,



&#x20;   outputPath = "${KNOWNFOLDER:Videos}/screen-recordings/${DD}-${SS}.mp4"

})

```



Prefer omitting schema defaults in Lua.



XBlox serialization remains complete and deterministic; Lua is the concise human-editable representation.



\---



\## Outputs and variables



Do not expose `storeAs` literally when converting XBlox to Lua.



XBlox:



```json

{

&#x20; "kind": "picker",

&#x20; "storeAs": "pickResult"

}

```



should become:



```lua

local pickResult = picker({

&#x20;   highlight = true,

&#x20;   timeoutMs = 60000

})

```



A consuming block then uses the variable directly:



```lua

videoCapture({

&#x20;   input = pickResult

})

```



This is preferable to:



```lua

input = "pickResult"

```



because the latter is only a string and loses semantic/type information.



The Lua representation should use normal Lua assignment and scoping wherever possible.



\---



\## Typed outputs



Native operations should expose meaningful return types.



Conceptually:



```cpp

CaptureTarget picker(PickerArgs);

CaptureResult videoCapture(VideoCaptureArgs);

```



Then Lua naturally becomes:



```lua

local target = picker({

&#x20;   highlight = true

})



local result = videoCapture({

&#x20;   input = target

})

```



The editor can infer:



```text

target : CaptureTarget

result : CaptureResult

```



This should drive type-aware completion.



\---



\## Lua editor support



The Lua editor should consume the same XBlox schema/type information as the visual editor.



Example:



```lua

videoCapture({

&#x20;   input =

})

```



Completion should prioritize values compatible with `CaptureTarget`:



```text

target

pickResult

screen.primary

screen.secondary

window.active

```



Similarly:



```lua

videoCapture({

&#x20;   fol

})

```



should suggest:



```text

follow

followDeadzone

followSpeed

```



and:



```lua

audioSource =

```



should suggest valid enum values:



```text

"desktop"

"microphone"

"both"

"none"

```



\---



\## Runtime variable expansion



Editor hover/inspection should be able to show runtime values without rewriting the source.



Example:



```lua

input = pickResult

```



Hover:



```text

pickResult

CaptureTarget



Produced by:

picker(...)



Current value:

screen:0

hwnd=2034552

title="Tanit Viewer"

```



Likewise:



```lua

outputPath = ctx.capturePath

```



could expose the current resolved value.



This is especially useful for XBlox context variables and previous block outputs.



\---



\## Monaco vs Scintilla



Both can support Lua.



\### Scintilla



Best fit when the Lua surface is a lightweight native code pane:



\- native Win32 integration

\- low footprint

\- fast startup

\- built-in Lua lexer

\- folding/highlighting

\- completion/calltips can be supplied by XBlox



Use the XBlox schema/symbol engine to provide richer completion than ordinary SciTE `.api` files.



\### Monaco



Prefer if the Lua surface evolves toward a real IDE:



\- completion providers

\- hover

\- diagnostics

\- semantic tokens

\- go-to-definition

\- references

\- rename

\- LSP integration

\- diff/editor tooling



Do not rely on generic Lua support alone. XBlox should supply domain-specific intelligence.



\---



\## C++ bindings



The underlying Lua C API remains broadly the traditional model:



```text

lua\_State\*

stack values

userdata

metatables

registered C functions

```



Avoid manually duplicating bindings for every XBlox block.



Do not expose the full C++ class model unless a runtime object genuinely benefits from object semantics.



Prefer declarative operations:



```lua

videoCapture({

&#x20;   input = target,

&#x20;   fps = 24

})

```



over:



```lua

local recorder = VideoRecorder.new()

recorder:setInput(target)

recorder:setFPS(24)

recorder:start()

```



unless `VideoRecorder` really represents a useful persistent runtime object.



\---



\# C++26 Reflection



\## Purpose



Use C++26 reflection to derive structural metadata from native C++ declarations.



Example:



```cpp

struct VideoCaptureArgs {

&#x20;   CaptureTarget input;



&#x20;   int width = 1920;

&#x20;   int height = 1080;

&#x20;   int fps = 30;



&#x20;   AudioSource audioSource = AudioSource::desktop;

&#x20;   FollowMode follow = FollowMode::cursor;



&#x20;   double zoom = 1.0;

&#x20;   double zoomMax = 3.0;

};

```



Reflection should provide most of:



```text

field names

field types

default values

enum information

nested structures

function parameter types

return types

```



This removes significant manual duplication.



\---



\## Do not replace the existing UI schema



C++26 reflection is the structural layer, not the presentation layer.



Current conceptual split:



```text

data/type schema

&#x20;   ├─ types

&#x20;   ├─ enums

&#x20;   ├─ defaults

&#x20;   ├─ required

&#x20;   └─ validation



UI schema

&#x20;   ├─ widgets

&#x20;   ├─ labels

&#x20;   ├─ groups

&#x20;   ├─ ordering

&#x20;   ├─ visibility

&#x20;   ├─ advanced/basic

&#x20;   ├─ help

&#x20;   └─ layout

```



Keep that separation.



Reflection should answer:



```text

fps : int

default = 30

```



The UI schema should answer:



```text

label = "Frame rate"

group = "Video"

widget = slider

order = 3

advanced = false

```



\---



\## Recommended architecture



```text

&#x20;                 C++ declarations

&#x20;                       │

&#x20;                C++26 reflection

&#x20;                       │

&#x20;                       ▼

&#x20;             Structural Descriptor

&#x20;                       │

&#x20;      ┌────────────────┼────────────────┐

&#x20;      │                │                │

&#x20;      ▼                ▼                ▼

&#x20;JSON Schema        Lua Binding       XBlox Ports

&#x20;      │                │                │

&#x20;      │                ▼                │

&#x20;      │          Lua completion          │

&#x20;      │          hover/diagnostics       │

&#x20;      │                                 │

&#x20;      └────────────────┬────────────────┘

&#x20;                       ▼

&#x20;                 Existing UI Schema

&#x20;                       │

&#x20;             widgets / layout / UX

```



The structural descriptor should be the stable intermediate representation.



Do not have each backend query `std::meta` independently.



\---



\## Structural descriptor



Conceptually:



```cpp

struct FieldInfo {

&#x20;   std::string\_view name;

&#x20;   TypeId type;



&#x20;   Value defaultValue;



&#x20;   bool required = false;



&#x20;   std::optional<Range> range;

&#x20;   std::vector<EnumValue> enumValues;

};

```



and:



```cpp

struct TypeInfo {

&#x20;   TypeId id;

&#x20;   std::string\_view name;

&#x20;   std::vector<FieldInfo> fields;

};

```



Reflection populates this.



Consumers then operate on the descriptor:



```text

JSON Schema generator

Lua decoder

Lua encoder

XBlox block generator

Monaco completion

Scintilla completion

serializer

validator

documentation

```



This keeps the rest of XBlox insulated from compiler-specific reflection APIs.



\---



\## Reflection + annotations



Reflection alone cannot infer domain/UI metadata such as:



```text

range

unit

widget type

group

help

advanced/basic

visibility rules

migration aliases

deprecated names

```



Use annotations or the existing schema layer for these.



Example conceptual metadata:



```cpp

struct VideoCaptureArgs {

&#x20;   CaptureTarget input;



&#x20;   // range: 1..240

&#x20;   int fps = 30;



&#x20;   AudioSource audioSource = AudioSource::desktop;

};

```



Enums should be real C++ enums where practical:



```cpp

enum class AudioSource {

&#x20;   desktop,

&#x20;   microphone,

&#x20;   both,

&#x20;   none

};

```



rather than:



```cpp

std::string audioSource;

```



This automatically improves:



```text

reflection

validation

Lua autocomplete

JSON Schema

XBlox UI

documentation

```



\---



\## Existing XBlox/UI schema remains authoritative for domain semantics



Reflection does not replace application-specific concepts such as:



```text

runtime-defined types

UUID/name-based type identity

parent/child types

explicit type casts

runtime schema stored in JSON

user-defined structures

migration aliases

UI visibility rules

conditional controls

```



Those remain part of XBlox's schema/runtime system.



C++ reflection primarily improves native C++ integration.



\---



\## Canonical flow



Before:



```text

C++ struct

&#x20; ↕ manual glue

JSON schema

&#x20; ↕ manual glue

XBlox block

&#x20; ↕ manual glue

Lua binding

&#x20; ↕ manual glue

editor metadata

```



Target:



```text

C++ declaration

&#x20;     │

&#x20;C++26 reflection

&#x20;     │

&#x20;     ▼

structural descriptor

&#x20;     │

&#x20;┌────┼───────────────┬───────────────┐

&#x20;▼    ▼               ▼               ▼

JSON  Lua            XBlox           Editor

&#x20;    binding         ports           metadata

&#x20;     │

&#x20;     ▼

existing runtime + UI schema

```



\---



\# Round-tripping



Desired pipeline:



```text

XBlox

&#x20; ↓

IR / structural representation

&#x20; ↓

Lua AST

&#x20; ↓

Lua text



Lua text

&#x20; ↓

Lua parser

&#x20; ↓

Lua AST

&#x20; ↓

XBlox IR

&#x20; ↓

XBlox

```



Do not generate Lua directly from UI block classes.



The intermediate representation should preserve:



```text

block type

parameter values

output type

variable identity

source spans

optional block IDs

comments / source metadata

```



\---



\## Unsupported Lua



Arbitrary Lua will eventually contain constructs XBlox cannot represent visually.



Use three levels:



```text

1\. Native XBlox representation

2\. Generic structured Lua AST block

3\. Opaque Lua source block

```



Never silently destroy unsupported Lua during Lua → XBlox → Lua round-trip.



\---



\## Block identity



Avoid polluting normal Lua with block IDs unless necessary.



Preferred:



```lua

local target = picker({

&#x20;   highlight = true

})

```



Internal source metadata can retain:



```text

xbloxBlockId = picker-1qiosj

sourceSpan = ...

```



If persistent identity across arbitrary text editing is required, optionally support:



```lua

\-- xblox:id=picker-1qiosj

local target = picker({

&#x20;   highlight = true

})

```



but do not make this mandatory for normal source.



\---



\# Context variables



XBlox context may remain available as an explicit Lua namespace:



```lua

ctx.capturePath

ctx.test

```



Example:



```lua

videoCapture({

&#x20;   input = target,

&#x20;   outputPath = ctx.capturePath

})

```



Windows paths can use Lua long strings where useful:



```lua

ctx.capturePath = \[\[C:\\Users\\zx\\Videos\\screen-recordings\\12-23.mp4]]

```



rather than heavily escaped strings.



\---



\# Implementation direction



\## Phase 1



Create the structural descriptor independent of C++26 reflection.



Adapt existing schema information into it first.



Consumers:



```text

Lua generator

Lua importer

editor completion

```



This allows work to proceed before compiler reflection support is production-ready.



\## Phase 2



Add C++26 reflection backend:



```text

C++ type

&#x20;   ↓

std::meta reflection

&#x20;   ↓

Structural Descriptor

```



Existing consumers remain unchanged.



\## Phase 3



Generate more infrastructure from the descriptor:



```text

Lua bindings

JSON Schema

XBlox native block descriptors

validation

documentation

Monaco/Scintilla metadata

```



\## Phase 4



Add runtime integration:



```text

typed output variables

hover values

block ↔ source navigation

Lua diagnostics

runtime inspection

```



\---



\# Core design rules



1\. \*\*C++ declarations are the native structural source of truth.\*\*



2\. \*\*The existing XBlox/UI schema remains the runtime and presentation source of truth.\*\*



3\. \*\*C++26 reflection feeds a compiler-independent structural descriptor.\*\*



4\. \*\*Lua uses normal Lua semantics wherever possible.\*\*



5\. \*\*XBlox `storeAs` maps to Lua assignment, not string indirection.\*\*



6\. \*\*Large block parameter sets use Lua tables.\*\*



7\. \*\*Lua output should generally omit schema defaults.\*\*



8\. \*\*The editor uses XBlox's own type/schema engine for completion.\*\*



9\. \*\*Do not mirror the entire C++ object hierarchy into Lua.\*\*



10\. \*\*Do not couple Lua generation directly to visual block classes.\*\*



11\. \*\*Preserve unsupported Lua instead of rejecting or destroying it.\*\*



12\. \*\*Keep compiler reflection behind an adapter so XBlox can ship before full MSVC/Clang support is reliable.\*\*



\---



\# Target example



XBlox:



```json

{

&#x20; "roots": \[

&#x20;   {

&#x20;     "kind": "picker",

&#x20;     "storeAs": "pickResult",

&#x20;     "highlight": true,

&#x20;     "timeoutMs": 60000

&#x20;   },

&#x20;   {

&#x20;     "kind": "videoCapture",

&#x20;     "input": "pickResult",

&#x20;     "fps": 24,

&#x20;     "width": 1080,

&#x20;     "height": 1080,

&#x20;     "audioSource": "desktop"

&#x20;   }

&#x20; ]

}

```



Lua:



```lua

local pickResult = picker({

&#x20;   highlight = true,

&#x20;   timeoutMs = 60000

})



videoCapture({

&#x20;   input = pickResult,



&#x20;   width = 1080,

&#x20;   height = 1080,

&#x20;   fps = 24,



&#x20;   audioSource = "desktop"

})

```



C++:



```cpp

struct PickerArgs {

&#x20;   bool highlight = false;

&#x20;   int timeoutMs = 60000;

};



struct VideoCaptureArgs {

&#x20;   CaptureTarget input;



&#x20;   int width = 1920;

&#x20;   int height = 1080;

&#x20;   int fps = 30;



&#x20;   AudioSource audioSource = AudioSource::desktop;

};



CaptureTarget picker(PickerArgs);

CaptureResult videoCapture(VideoCaptureArgs);

```



Reflection/descriptor derives enough information for:



```text

XBlox block ports

Lua bindings

Lua autocomplete

type checking

JSON serialization/schema

default handling

documentation

```



while the existing UI schema continues to define:



```text

layout

groups

labels

widgets

visibility

advanced/basic presentation

help

```



That is the intended architecture.

