#pragma once
// block_variables.hpp  -  Variable interpolation for xblox block arguments.
//
// Calls `media::commands::make_variable_map` for the system/folder variables
// (YYYY, CWD, CURRENT_SELECTION, ${ENV:...}, ${KNOWNFOLDER:...}, ...), then folds
// in every top-level entry of the xblox execution context so that user-defined
// variables (PREVIOUS, myVar, loop.index, ...) are also reachable as ${myVar}.
//
// Typical use inside a block handler:
//
//   #include "blocks/block_variables.hpp"
//
//   const std::string path = bv::resolve(fs_jstr(block, "path"), runtime);
//
// The function is intentionally cheap  -  the VariableMap is built fresh on
// every call but is small (< 50 entries in typical usage).  Call it only on
// fields that should support ${...} interpolation, not on expression/condition
// fields (which have their own evaluator).

#include "blocks/block_types.hpp"   // BlockRuntime (not the umbrella  -  avoids a cycle)
#include "core/command_variables.hpp"
#include "xblox_expressions.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace media::xblox::bv {
namespace detail {

inline std::string json_to_string(const nlohmann::json& v)
{
    // Shared scalar formatting (whole floats print cleanly, booleans as
    // true/false, ...)  -  same rules as nested-reference interpolation.
    return expr::json_to_display_string(v);
}

} // namespace detail

// Build a VariableMap that combines:
//   1. System variables from the command_context (dates, CWD, selection, ...)
//   2. Every top-level key of the runtime context JSON (user/scope vars)
inline media::commands::VariableMap make_block_variable_map(const blocks::BlockRuntime& runtime)
{
    media::commands::VariableMap vars = media::commands::make_variable_map(runtime.options.command_context);

    // Overlay xblox context variables so ${PREVIOUS} / ${myVar} / ${loop.index} work.
    // We iterate the top-level JSON context object; nested objects are skipped
    // (their children are accessible via "parent.child" dot-path in the evaluator,
    //  which is a different concern).
    if (runtime.context.is_object()) {
        for (auto it = runtime.context.begin(); it != runtime.context.end(); ++it) {
            const auto& val = it.value();
            // Arrays and objects are JSON-serialised so ${myList} / ${myObj} expand
            // to their JSON text when interpolated into a string field (e.g. fsWrite
            // content, log message).  Structured access to the underlying value still
            // works through context_value / jq / Parse  -  the variable map is only for
            // ${} string substitution.
            if (val.is_object() || val.is_array())
                vars.insert_or_assign(it.key(), val.dump());
            else
                vars.insert_or_assign(it.key(), detail::json_to_string(val));
        }
    }
    return vars;
}

// Resolve ${...} placeholders in a single string field.
// Returns the original string unchanged when it contains no '${'.
//
// Each pass is two stages: nested references first (`${doc.items[0].name}`  - 
// keys with dots/brackets that the flat resolver cannot parse), then the flat
// system/context variable map (`${PREVIOUS}`, `${ENV:...}`, ...).
//
// `max_passes` > 1 (ParamResolve::Deep, `.deep()`) re-expands ${..} tokens
// that arrive *inside* substituted variable values  -  e.g. a context variable
// holding "${KNOWNFOLDER:Desktop}/scans/*.png". Capped at kMaxDeepPasses;
// a circular reference (a -> ${b}, b -> ${a}) aborts the expansion with a
// warning and returns the input unchanged  -  no half-expanded states. The
// default of 1 keeps the historical single-pass semantics: substituted
// values are data, never templates.
//
// `quiet` suppresses unresolved-key warnings: old-style handlers that
// re-resolve a *declared* param (log/stdout `message`) pass true, because the
// central param pipeline already resolved the same string and warned once.
inline constexpr int kMaxDeepPasses = 5;

inline std::string resolve(std::string_view value, const blocks::BlockRuntime& runtime,
                           bool quiet = false, int max_passes = 1)
{
    if (value.find("${") == std::string_view::npos)
        return std::string(value);

    const auto vars = make_block_variable_map(runtime);
    const int passes = std::clamp(max_passes, 1, kMaxDeepPasses);

    std::string cur(value);
    std::vector<std::string> seen;
    for (int pass = 0; pass < passes; ++pass) {
        if (cur.find("${") == std::string::npos)
            break;
        // Only the first pass reports unresolved keys; re-passes stay quiet so
        // a token that never resolves warns once, not once per pass.
        const bool q = quiet || pass > 0;
        const std::string pre = expr::interpolate_references(cur, runtime.context, q);
        std::string next = media::commands::resolve_variables(pre, vars, nullptr, q);
        if (next == cur)
            break;  // stable: fully resolved, or unresolvable tokens left literal
        if (std::find(seen.begin(), seen.end(), next) != seen.end()) {
            blocks::block_log("warn", "[variables] circular variable reference while resolving \"" +
                                      std::string(value) + "\"  -  expansion aborted");
            return std::string(value);
        }
        seen.push_back(std::move(cur));
        cur = std::move(next);
    }
    return cur;
}

// Convenience: resolve a string field from a block JSON object.
inline std::string resolve_field(const nlohmann::json& block, const char* key,
                                 const blocks::BlockRuntime& runtime)
{
    if (!block.is_object() || !block.contains(key) || !block[key].is_string())
        return {};
    return resolve(block[key].get<std::string>(), runtime);
}

} // namespace media::xblox::bv
