#pragma once
// block_validation.hpp  -  declared param preconditions.
//
// `ParamConstraint` is the extensible bitmask a ParamDef carries; the executor's
// resolver enforces it (see validate_constraints in builtin_blocks.cpp) and
// records findings as `Diagnostic`s (in block_types.hpp). Severity-2 findings
// make the executor skip the handler. Kept separate from block_params.hpp so the
// constraint vocabulary can grow (incl. future async/host-mediated capabilities)
// without churning the parameter-definition surface.
//
// Split out of the former monolithic builtin_blocks.hpp (see docs/xblox-next.md
// §10.49).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace media::xblox::blocks {

// ─────────────────────────────────────────────────────────────────────────────
// ParamConstraint  -  declared preconditions on a resolved value, checked by the
// executor's resolver (see validate_constraints) and surfaced as Diagnostics.
//
// An extensible bitmask: most are cheap, read-only probes (existence/type) that
// run in both normal and simulate mode; CreateParents is the one that *acts* and
// is suppressed under simulate. The high bits are reserved for constraints that
// may be host-mediated / asynchronous (e.g. RequiresPermission)  -  those are not
// resolved by the synchronous probe but by a future capability gate in the
// executor, which can await a host RPC before dispatch.
// ─────────────────────────────────────────────────────────────────────────────

enum class ParamConstraint : std::uint32_t {
    None          = 0,
    NonEmpty      = 1u << 0,   // resolved string must be non-empty (gives req() teeth)
    MustExist     = 1u << 1,   // path must already exist
    Readable      = 1u << 2,   // path must be readable
    Writable      = 1u << 3,   // path (or its parent) must be writable
    IsFile        = 1u << 4,   // existing path must be a regular file
    IsDir         = 1u << 5,   // existing path must be a directory
    ParentExists  = 1u << 6,   // parent directory must already exist
    CreateParents = 1u << 7,   // ...or create it (effectful; skipped in simulate)
    Absolute      = 1u << 8,   // path must be absolute (advisory)
    // ── reserved: resolved by an async/host capability gate, not the probe ──
    RequiresPermission = 1u << 16,
};

inline ParamConstraint  operator|(ParamConstraint a, ParamConstraint b) { return static_cast<ParamConstraint>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)); }
inline ParamConstraint& operator|=(ParamConstraint& a, ParamConstraint b) { a = a | b; return a; }
inline bool             constraint_has(ParamConstraint set, ParamConstraint f) { return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0; }

// Constraint tokens for the manifest surface. Returns a plain string list; JSON
// is only materialized at the ParamDef::to_json() boundary (nlohmann converts it).
inline std::vector<std::string> param_constraint_tokens(ParamConstraint c) {
    std::vector<std::string> t;
    const std::pair<ParamConstraint, const char*> names[] = {
        {ParamConstraint::NonEmpty, "nonEmpty"}, {ParamConstraint::MustExist, "mustExist"},
        {ParamConstraint::Readable, "readable"}, {ParamConstraint::Writable, "writable"},
        {ParamConstraint::IsFile, "isFile"}, {ParamConstraint::IsDir, "isDir"},
        {ParamConstraint::ParentExists, "parentExists"}, {ParamConstraint::CreateParents, "createParents"},
        {ParamConstraint::Absolute, "absolute"}, {ParamConstraint::RequiresPermission, "requiresPermission"},
    };
    for (const auto& [flag, label] : names)
        if (constraint_has(c, flag)) t.emplace_back(label);
    return t;
}

} // namespace media::xblox::blocks
