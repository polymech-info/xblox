#pragma once
// block_resolver.hpp  -  schema-driven input sourcing + resolution.
//
// The resolver turns a block's declared ParamDefs into finished, typed inputs
// before the handler runs: `source_param` does the sourcing stage (field ->
// PREVIOUS/variable -> default); `resolve_block_inputs` (implemented in
// builtin_blocks.cpp) layers transform/cast + ${var} interpolation + constraint
// validation on top, producing a ResolvedInputs frame. The out-of-line
// BlockIO::def/resolved live here because they need the complete ParamDef.
//
// Split out of the former monolithic builtin_blocks.hpp (see docs/xblox-next.md
// §10.49).

#include "blocks/block_types.hpp"
#include "blocks/block_params.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::xblox::blocks {

struct ParamBindingResolution {
    bool present = false;
    const nlohmann::json* value = nullptr;
    std::string diagnostic_code;
    std::string message;
};

inline ParamBindingResolution resolve_param_binding(const nlohmann::json& block,
                                                    const ParamDef& d,
                                                    const BlockRuntime& rt)
{
    ParamBindingResolution out;
    if (!block.is_object()) return out;
    const auto bindings_it = block.find("bindings");
    if (bindings_it == block.end() || !bindings_it->is_object()) return out;
    const auto binding_it = bindings_it->find(d.name);
    if (binding_it == bindings_it->end()) return out;
    out.present = true;

    if (!binding_it->is_object()) {
        out.diagnostic_code = "invalid_binding";
        out.message = "binding must be an object";
        return out;
    }

    const std::string kind = binding_it->value("kind", std::string{});
    ParamBindingKind required = ParamBindingKind::None;
    if (kind == "blockOutput")             required = ParamBindingKind::BlockOutput;
    else if (kind == "scopeVariable")      required = ParamBindingKind::ScopeVariable;
    else if (kind == "previous")           required = ParamBindingKind::Previous;
    else if (kind == "parameterOperation") required = ParamBindingKind::ParameterOperation;
    else {
        out.diagnostic_code = "unknown_binding_kind";
        out.message = "unknown binding kind: " + kind;
        return out;
    }

    if (!binding_has(d.accepted_bindings, required)) {
        out.diagnostic_code = "binding_not_supported";
        out.message = "binding kind " + kind + " is not accepted by this parameter";
        return out;
    }

    if (required == ParamBindingKind::ParameterOperation) {
        out.diagnostic_code = "unsupported_parameter_operation";
        out.message = "parameter operations are declared but evaluator dispatch is not available";
        return out;
    }
    if (required == ParamBindingKind::Previous) {
        out.value = rt.context_value
            ? rt.context_value(rt.user_data, "PREVIOUS")
            : nullptr;
    } else if (required == ParamBindingKind::ScopeVariable) {
        const std::string name = binding_it->value("name", std::string{});
        if (name.empty()) {
            out.diagnostic_code = "invalid_scope_variable_binding";
            out.message = "scopeVariable binding requires name";
            return out;
        }
        out.value = rt.context_value ? rt.context_value(rt.user_data, name) : nullptr;
    } else {
        const std::string block_id = binding_it->value("blockId", std::string{});
        const std::string slot = binding_it->value("output", std::string{"result"});
        if (block_id.empty() || slot.empty()) {
            out.diagnostic_code = "invalid_block_output_binding";
            out.message = "blockOutput binding requires blockId and output";
            return out;
        }
        out.value = rt.block_output_value
            ? rt.block_output_value(rt.user_data, block_id, slot)
            : nullptr;
        if (!out.value) {
            // Disabled source blocks never publish outputs. Ignore the binding
            // so the consumer can fall back to its literal field / default
            // (e.g. videoCapture.input while an optional picker is disabled).
            bool enabled = true;
            if (rt.block_enabled &&
                rt.block_enabled(rt.user_data, block_id, &enabled) &&
                !enabled) {
                out = {};
                return out;
            }
        }
    }

    if (!out.value) {
        out.diagnostic_code = "unavailable_binding_source";
        out.message = "binding source is unavailable at this execution point";
    }
    return out;
}

// ── Schema-driven input sourcing (shared by BlockIO + the executor's resolver) ─
//
// Sourcing stage only: returns the *raw* json for a param from
//   explicit field -> declared source (PREVIOUS / variable) -> default.
// The transform/cast stage runs on top of this (see resolve_block_inputs).
// Value-sourcing is skipped for variable_ref kinds (they carry a *name*).
inline const nlohmann::json* source_param(const nlohmann::json& block,
                                          const ParamDef&       d,
                                          const BlockRuntime&   rt) {
    const ParamBindingResolution binding = resolve_param_binding(block, d, rt);
    if (binding.present)
        return binding.value;

    const nlohmann::json* f = nullptr;
    if (block.is_object()) {
        const auto it = block.find(d.name);
        if (it != block.end()) f = &(*it);
    }
    const bool has_field = f && !f->is_null();
    const bool sourced = d.kind != ParamKind::variable_ref &&
        (d.uses_previous || d.source == ParamSource::Previous ||
         (d.source == ParamSource::Variable && !d.source_ref.empty()));
    const bool empty_str = has_field && f->is_string() && f->get<std::string>().empty();

    // 1) An explicit, non-empty field wins. An empty string on a sourced param
    //    defers to the source (the documented "leave empty to use PREVIOUS").
    if (has_field && !(empty_str && sourced))
        return f;

    // 2) Declared source.
    if (sourced && rt.context_value) {
        if (d.uses_previous || d.source == ParamSource::Previous) {
            if (const nlohmann::json* p = rt.context_value(rt.user_data, "PREVIOUS"); p && !p->is_null())
                return p;
        } else if (const nlohmann::json* v = rt.context_value(rt.user_data, d.source_ref); v && !v->is_null()) {
            return v;
        }
    }

    // 3) Declared default, else fall back to the (possibly empty) literal field.
    if (!d.default_value.is_null())
        return &d.default_value;
    return has_field ? f : nullptr;
}

// BlockIO::def / BlockIO::resolved are defined in block_resolver.cpp.
// They stay out of line so TUs that only include block_types.hpp can call them.

// Build the typed input frame for a block: source every declared ParamDef
// (field -> PREVIOUS/variable -> default), interpolate ${var}, run the
// kind-specific transform (cast + enum/range validation), and check declared
// preconditions. Pure read of context (except the lone effectful CreateParents
// constraint), so it is safe to run ahead of (or instead of) the handler.
ResolvedInputs resolve_block_inputs(const nlohmann::json&        block,
                                    const std::vector<ParamDef>& defs,
                                    BlockRuntime&                rt);

} // namespace media::xblox::blocks
