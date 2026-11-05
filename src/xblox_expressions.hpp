#pragma once
// xblox_expressions.hpp  -  the central expression / reference surface for XBlox.
//
// One reference grammar everywhere:
//
//     name            simple context variable
//     a.b.c           nested object access
//     a.b[0].c        array indexing  -  `a.b[0]` and `a.b.0` are equivalent
//
// The same paths work in template fields (`${doc.items[0].name}`), in
// expression / condition fields (`doc.items[0].count > 3`), and in Parse-style
// consumers. This module owns:
//
//   1. Reference paths    -  split / resolve against the JSON context, with
//                          numeric coercion and array `length` / `count`
//                          pseudo-fields.
//   2. Template pre-pass  -  `interpolate_references()` resolves `${path}`
//                          tokens that the flat `${VAR}` resolver cannot
//                          (dots / brackets), before it runs.
//   3. Expression front-end  -  `prepare_expression()` normalizes an expression
//                          for the cached muParser evaluator: legacy aliases,
//                          `${ref}` unwrapping, string equality folding
//                          (`status == "ready"` -> 1/0 against the context),
//                          and rewriting nested references to synthetic
//                          variable slots the evaluator binds per call.
//
// The muParser cache itself stays in xblox_commands.cpp (it is coupled to the
// scope stack and numeric-context store); everything string/path-shaped that
// feeds it lives here.

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace media::xblox::expr {

// ── reference paths ─────────────────────────────────────────────────────────

// Split a reference path into segments. `a.b.0` and `a.b[0]` produce the same
// segments  -  '[' / ']' act as separators just like '.'.
std::vector<std::string> split_reference(const std::string& path);

// Walk `root` along `path`. Object keys match by name; array elements match
// all-digit segments. Returns nullptr when any hop fails.
const nlohmann::json* resolve_reference(const nlohmann::json& root, const std::string& path);
nlohmann::json*       resolve_reference(nlohmann::json& root, const std::string& path);

// Resolve a reference to a double: numbers as-is, booleans as 1/0, strings
// that parse fully as numbers. Also serves array sizes through the
// `path.length` / `path.count` pseudo-fields (and the legacy flattened
// `path_count` / `path_length` spellings).
bool reference_number(const nlohmann::json& root, const std::string& path, double& out);

// Resolve a reference to its display string (same formatting as template
// interpolation). Returns false when the path does not resolve.
bool reference_string(const nlohmann::json& root, const std::string& path, std::string& out);

// Shared scalar formatting: strings verbatim, whole floats without trailing
// zeros, booleans as true/false, null as "null", containers as compact JSON.
std::string json_to_display_string(const nlohmann::json& v);

// ── muParser identifier sanitation ───────────────────────────────────────────

// Map an arbitrary name to a muParser-safe identifier ('.' etc -> '_',
// leading digit prefixed with '_').
std::string mu_var_name(std::string s);

// Deterministic synthetic variable name for a reference path ("_p_...").
std::string reference_slot_name(const std::string& path);

// ── template pre-pass ────────────────────────────────────────────────────────

// Resolve `${path}` tokens whose key contains '.' or '[' (the flat resolver
// only understands `${NAME}` / `${ENV:...}` keys). Resolved values are spliced
// in via json_to_display_string(); unresolved paths warn (unless `quiet`) and
// degrade to the bare key text. Simple `${NAME}` tokens pass through untouched.
std::string interpolate_references(std::string_view text, const nlohmann::json& context,
                                   bool quiet = false);

// ── expression front-end ─────────────────────────────────────────────────────

struct ReferenceSlot {
    std::string name; // synthetic muParser variable, deterministic per path
    std::string path; // original reference text ("PREVIOUS.data[0]")
};

struct PreparedExpression {
    std::string text;                 // normalized source (cache key for muParser)
    std::vector<ReferenceSlot> refs;  // slots to bind+resolve per evaluation
    bool changed = false;             // text differs from the trimmed input
};

// Full normalization pipeline for expression / condition fields:
//   trim -> legacy aliases (this., hasQueuedWork(), selection...)
//        -> `${ref}` unwrap (template syntax tolerated in expressions)
//        -> string comparison folding against `context` (==/!= with a quoted
//          literal on either side collapses to 1/0)
//        -> nested references rewritten to ReferenceSlot names
//        -> remaining identifiers sanitized for muParser.
PreparedExpression prepare_expression(const std::string& source, const nlohmann::json& context);

} // namespace media::xblox::expr
