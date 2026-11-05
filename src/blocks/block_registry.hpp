#pragma once
// block_registry.hpp  -  descriptors, builders, and the registry interface.
//
// A BlockDescriptor is the full self-describing manifest for a block kind
// (params + child lists + exec flags + handler), serialised to the host->UI RPC
// via to_manifest(). `bdb()`/`block_descriptor()` build them (accepting native
// or legacy handlers transparently via HandlerArg). The free functions at the
// bottom are the registry lookup surface the executor uses.
//
// Split out of the former monolithic builtin_blocks.hpp (see docs/xblox-next.md
// §10.49).

#include "blocks/block_types.hpp"
#include "blocks/block_params.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace media::xblox::blocks {

// ─────────────────────────────────────────────────────────────────────────────
// ExecFlags  -  block-type-level execution-model declarations
// ─────────────────────────────────────────────────────────────────────────────

struct ExecFlags {
    bool supports_background = false; // block may fire-and-forget
    bool supports_cancel = false;     // block polls cancel_requested
    bool is_blocking = true;          // chain waits for completion
    bool is_container = false;        // block has child lists (if/for/while/switch)
    bool singleton = false;           // at most one instance running at a time
    int  min_duration_hint_ms = 0;    // rough expected duration (0 = instant)
    // Touches the world outside the in-memory variable store (files, processes,
    // network, devices, external commands). Defaults true so unknown/new blocks
    // are conservatively suppressed under simulate; pure dataflow/control blocks
    // opt out via `.pure()`. Container blocks always run regardless (they only
    // orchestrate their children).
    bool has_side_effects = true;
    // Iterable producer: when the block's result is a JSON array, the executor
    // runs its `items` children once per element (PREVIOUS = element, plus a
    // numeric `index`) instead of once over the whole result. Field projection
    // is left to a child `Parse` block (e.g. `.path`), so no pluck lives here.
    // Must NOT be combined with is_container (native containers own their items).
    bool iterable = false;

    nlohmann::json to_json() const {
        return {
            {"supports_background",   supports_background},
            {"supports_cancel",       supports_cancel},
            {"is_blocking",           is_blocking},
            {"is_container",          is_container},
            {"singleton",             singleton},
            {"min_duration_hint_ms",  min_duration_hint_ms},
            {"has_side_effects",      has_side_effects},
            {"iterable",              iterable},
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ChildListDef  -  declares child block lists for container blocks
// ─────────────────────────────────────────────────────────────────────────────

struct ChildListDef {
    std::string key;              // JSON field name on the block
    std::string label;            // "Body", "Then", "Else", etc.
    std::string role;             // "body"|"consequent"|"alternate"|"case"
    std::vector<std::string> accepts = {"*"}; // block kinds accepted; "*" = all
    int min_items = 0;
    int max_items = -1;           // -1 = unlimited

    nlohmann::json to_json() const {
        return {
            {"key",       key},
            {"label",     label},
            {"role",      role},
            {"accepts",   accepts},
            {"min_items", min_items},
            {"max_items", max_items},
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// OutputDef - stable runtime output slots exposed to bindings and host UIs.
// ─────────────────────────────────────────────────────────────────────────────

enum class PreviousBehavior {
    Unknown,
    Writes,
    Preserves,
    Clears,
};

inline const char* previous_behavior_str(PreviousBehavior behavior) noexcept
{
    switch (behavior) {
    case PreviousBehavior::Writes:    return "writes";
    case PreviousBehavior::Preserves: return "preserves";
    case PreviousBehavior::Clears:    return "clears";
    case PreviousBehavior::Unknown:   return "unknown";
    default:                          return "unknown";
    }
}

struct OutputDef {
    std::string slot; // Stable binding id, normally "result" for the primary.
    std::string label;
    ParamKind kind = ParamKind::json_value;
    bool primary = false;
    bool writes_previous = false;
    bool scope_export = false; // Writes a fixed variable named by `export_name`.
    std::string export_name;
    bool conditional = false;
    bool dynamic = false;
    std::vector<ParamKind> supported_kinds;
    std::string export_from_param; // Dynamic scope-export base, e.g. storeAs.
    std::string export_suffix;     // Literal suffix appended to that base.

    nlohmann::json to_json() const
    {
        nlohmann::json j = {
            {"slot", slot},
            {"kind", param_kind_str(kind)},
            {"primary", primary},
            {"writesPrevious", writes_previous},
            {"scopeExport", scope_export},
            {"conditional", conditional},
            {"dynamic", dynamic},
        };
        if (!label.empty())       j["label"] = label;
        if (!export_name.empty()) j["exportName"] = export_name;
        if (!export_from_param.empty()) j["exportFromParam"] = export_from_param;
        if (!export_suffix.empty())     j["exportSuffix"] = export_suffix;
        if (!supported_kinds.empty()) {
            nlohmann::json kinds = nlohmann::json::array();
            for (const ParamKind supported : supported_kinds)
                kinds.push_back(param_kind_str(supported));
            j["supportedKinds"] = std::move(kinds);
        }
        return j;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BlockDescriptor
// ─────────────────────────────────────────────────────────────────────────────

struct BlockDescriptor {
    std::string kind;
    std::string label;
    std::string description;
    std::string group;
    std::string color;                  // hex for palette/tree badge
    std::string icon;                   // icon name (future)
    nlohmann::json default_block = nlohmann::json::object();

    // Typed param definitions (new). When non-empty, to_manifest() uses these.
    std::vector<ParamDef>      param_defs;
    std::vector<OutputDef>     output_defs;
    std::vector<ChildListDef>  child_lists;
    ExecFlags                  exec_flags;
    PreviousBehavior           previous_behavior = PreviousBehavior::Unknown;

    // Legacy raw JSON params  -  kept for backward compat (app_blocks, modbus_blocks).
    // Falls back to this when param_defs is empty.
    nlohmann::json params = nlohmann::json::array();
    nlohmann::json flags  = nlohmann::json::object();

    std::uint32_t platform_mask = 0xffffffffu;
    bool palette_visible = true;
    BlockHandler handler = nullptr;

    // Serialise the full rich manifest for the host -> UI RPC payload.
    nlohmann::json to_manifest() const {
        nlohmann::json j = {
            {"kind",         kind},
            {"label",        label.empty() ? kind : label},
            {"description",  description},
            {"group",        group},
            {"block",        default_block},
            {"platformMask", platform_mask},
            {"paletteVisible", palette_visible},
            {"execFlags",    exec_flags.to_json()},
            {"manifestVersion", 2},
            {"manifestCapabilities", {
                "typed_bindings",
                "output_descriptors",
                "parameter_operations",
            }},
            {"previousBehavior", previous_behavior_str(previous_behavior)},
        };
        if (!color.empty()) j["color"] = color;
        if (!icon.empty())  j["icon"]  = icon;

        // Params: prefer typed defs; fall back to legacy JSON array
        if (!param_defs.empty()) {
            auto arr = nlohmann::json::array();
            for (const auto& pd : param_defs)
                arr.push_back(pd.to_json());
            j["params"] = std::move(arr);
        } else {
            j["params"] = params;
        }

        // Child lists
        if (!child_lists.empty()) {
            auto arr = nlohmann::json::array();
            for (const auto& cl : child_lists)
                arr.push_back(cl.to_json());
            j["childLists"] = std::move(arr);
        }

        auto output_arr = nlohmann::json::array();
        for (const auto& output : output_defs)
            output_arr.push_back(output.to_json());
        j["outputs"] = std::move(output_arr);

        return j;
    }
};

using BlockRegistry = std::unordered_map<std::string, BlockDescriptor>;

// ── Legacy builder (keeps raw JSON params for backward compat) ────────────────

inline BlockDescriptor block_descriptor(std::string kind,
                                        HandlerArg handler,
                                        std::string label = {},
                                        std::string group = {},
                                        std::string description = {},
                                        nlohmann::json default_block = nlohmann::json::object(),
                                        nlohmann::json params = nlohmann::json::array(),
                                        nlohmann::json flags = nlohmann::json::object(),
                                        std::uint32_t platform_mask = 0xffffffffu,
                                        bool palette_visible = true)
{
    BlockDescriptor d;
    d.kind          = std::move(kind);
    d.label         = std::move(label);
    d.description   = std::move(description);
    d.group         = std::move(group);
    d.default_block = std::move(default_block);
    d.params        = std::move(params);
    d.flags         = std::move(flags);
    d.platform_mask = platform_mask;
    d.palette_visible = palette_visible;
    d.handler       = handler;
    return d;
}

// ── Typed builder (uses ParamDef, ExecFlags, ChildListDef) ───────────────────

struct BlockDescriptorBuilder {
    BlockDescriptor d;

    BlockDescriptorBuilder(std::string kind, HandlerArg handler,
                           std::string label, std::string group,
                           std::string description,
                           nlohmann::json default_block)
    {
        d.kind          = std::move(kind);
        d.handler       = handler;
        d.label         = std::move(label);
        d.group         = std::move(group);
        d.description   = std::move(description);
        d.default_block = std::move(default_block);
    }

    BlockDescriptorBuilder& params(std::vector<ParamDef> ps)      { d.param_defs  = std::move(ps); return *this; }
    BlockDescriptorBuilder& children(std::vector<ChildListDef> cl){ d.child_lists = std::move(cl); return *this; }
    BlockDescriptorBuilder& exec(ExecFlags ef)                    { d.exec_flags  = ef;            return *this; }
    BlockDescriptorBuilder& primary_output(ParamKind kind = ParamKind::json_value,
                                           std::string label = {},
                                           bool conditional = false) {
        d.output_defs.push_back({
            "result", std::move(label), kind, true, true,
            false, {}, conditional,
        });
        d.previous_behavior = PreviousBehavior::Writes;
        return *this;
    }
    BlockDescriptorBuilder& dynamic_primary_output(
        ParamKind fallback,
        std::vector<ParamKind> supported,
        std::string label = {},
        bool conditional = false) {
        d.output_defs.push_back({
            "result", std::move(label), fallback, true, true,
            false, {}, conditional, true, std::move(supported),
        });
        d.previous_behavior = PreviousBehavior::Writes;
        return *this;
    }
    BlockDescriptorBuilder& output(std::string slot, ParamKind kind,
                                   std::string label = {},
                                   std::string export_name = {},
                                   bool conditional = false) {
        const bool exports = !export_name.empty();
        d.output_defs.push_back({
            std::move(slot), std::move(label), kind, false, false,
            exports, std::move(export_name), conditional,
        });
        return *this;
    }
    BlockDescriptorBuilder& output_from_param(
        std::string slot, ParamKind kind, std::string label,
        std::string export_from_param, std::string export_suffix = {},
        bool conditional = false) {
        d.output_defs.push_back({
            std::move(slot), std::move(label), kind, false, false,
            true, {}, conditional, false, {},
            std::move(export_from_param), std::move(export_suffix),
        });
        return *this;
    }
    BlockDescriptorBuilder& preserves_previous() {
        d.previous_behavior = PreviousBehavior::Preserves;
        return *this;
    }
    BlockDescriptorBuilder& clears_previous() {
        d.previous_behavior = PreviousBehavior::Clears;
        return *this;
    }
    // Mark a leaf block as side-effect-free (in-memory only) so it still runs
    // under simulate. Container blocks need not call this.
    BlockDescriptorBuilder& pure()                                { d.exec_flags.has_side_effects = false; return *this; }
    // Mark a producer iterable: the executor fans its `items` children out over
    // each element of an array result (PREVIOUS = element + numeric `index`).
    BlockDescriptorBuilder& iterable()                            { d.exec_flags.iterable = true;  return *this; }
    BlockDescriptorBuilder& color(std::string c)                  { d.color       = std::move(c);  return *this; }
    BlockDescriptorBuilder& icon(std::string i)                   { d.icon        = std::move(i);  return *this; }
    BlockDescriptorBuilder& hidden()                              { d.palette_visible = false;     return *this; }
    BlockDescriptorBuilder& plat(std::uint32_t mask)              { d.platform_mask = mask;        return *this; }

    BlockDescriptor build() { return std::move(d); }
    // Implicit conversion so registry[...] = bdb(...).params({...}) works directly
    operator BlockDescriptor() { return std::move(d); }
};

inline BlockDescriptorBuilder bdb(std::string kind, HandlerArg handler,
                                  std::string label, std::string group,
                                  std::string description,
                                  nlohmann::json default_block = nlohmann::json::object())
{
    return {std::move(kind), handler,
            std::move(label), std::move(group), std::move(description),
            std::move(default_block)};
}

// ─────────────────────────────────────────────────────────────────────────────
// Registry interface
// ─────────────────────────────────────────────────────────────────────────────

BlockHandler builtin_block_handler(const std::string& kind);
// Declared param schema for a block kind (stable pointer into the cached
// registry), or nullptr. Used by the executor to feed BlockRuntime::param_defs.
const std::vector<ParamDef>* builtin_block_param_defs(const std::string& kind);
// Full descriptor contract (params, outputs, execution/scoping behavior).
const BlockDescriptor* builtin_block_descriptor(const std::string& kind);

// Execution-model flags for a block kind (stable pointer into the cached
// registry), or nullptr. The executor consults has_side_effects/is_container
// to decide simulate-mode suppression.
const ExecFlags* builtin_block_exec_flags(const std::string& kind);

void register_builtin_blocks(BlockRegistry& registry);
std::vector<BlockDescriptor> registered_block_definitions(bool include_hidden = false);

} // namespace media::xblox::blocks
