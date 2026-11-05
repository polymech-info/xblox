#pragma once

#include "runtime.hpp"
#include "Scope.h"
#include "blocks/block_types.hpp"
#include "muParser.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace media::xblox {

struct OutputFrameValue {
    std::string kind;
    nlohmann::json value;
};

// Per-execution stable block-id/output-slot values used by document-v2
// bindings. Detached background FlowStates naturally receive an independent
// store and therefore cannot publish values back into their parent flow.
struct OutputFrameStore {
    std::unordered_map<std::string,
        std::unordered_map<std::string, OutputFrameValue>> by_block;

    void publish(const std::string& block_id, const std::string& slot,
                 std::string kind, const nlohmann::json& value)
    {
        if (block_id.empty() || slot.empty()) return;
        by_block[block_id][slot] = {std::move(kind), value};
    }

    const OutputFrameValue* find(const std::string& block_id,
                                 const std::string& slot) const
    {
        const auto block_it = by_block.find(block_id);
        if (block_it == by_block.end()) return nullptr;
        const auto slot_it = block_it->second.find(slot);
        return slot_it == block_it->second.end() ? nullptr : &slot_it->second;
    }

    void erase(const std::string& block_id)
    {
        if (!block_id.empty()) by_block.erase(block_id);
    }

    void clear() { by_block.clear(); }
};

struct FlowState {
    bool break_requested = false;
    bool cancel_emitted = false;
    // Set when a handler returns BlockResult::Fatal: unwinds the whole run. We
    // also set break_requested alongside it so the existing builtin for/while
    // loops (which only poll break_requested) stop without modification.
    bool abort_requested = false;
    // S3 scope frames: per-run stack threaded through every execution path.
    // Null in detached background runs (scoping there is fire-and-forget).
    ScopeStack* scopes = nullptr;
    // Run-scoped resource cache (RunScope) owned by the PassEngine: persists
    // across loop passes so handlers can cache an expensive instance keyed by
    // identity. Null in detached background runs.
    RunScope* run_scope = nullptr;
    // Sibling if/elseIf/else chain of the list currently executing. Each list
    // runner installs its own ChainState and restores the parent's on exit, so
    // chains never leak across nesting levels (docs/xblox/language.md D1).
    blocks::ChainState* chain = nullptr;
    // Depth of enclosing for/while/iterable loops: `break` outside any loop
    // warns instead of killing the rest of the pass (docs/xblox/language.md D3).
    int loop_depth = 0;
    OutputFrameStore output_frames;
    // Document body roots for the active pass (blockId → enabled lookups while
    // resolving bindings). Null in detached/background runs.
    const nlohmann::json* document_roots = nullptr;
};

struct NumericContextStore {
    std::unordered_map<std::string, double> values;
    std::unordered_map<std::string, size_t> slots;
    std::vector<double> slot_values;
};

struct RuntimeBridge {
    nlohmann::json& context;
    NumericContextStore& numeric_context;
    const ExecutionOptions& options;
    ExecutionResult& result;
    FlowState& flow;
    std::string block_id;
    bool primary_output_published = false;
};

struct CachedMuExpression {
    mu::Parser parser;
    std::map<std::string, double> vars;

    // Bytecode-reuse state (scope path). muParser compiles an expression to
    // bytecode on the first Eval() and reuses it on subsequent Eval()s, but
    // SetExpr/DefineVar/ClearVar each ReInit() and throw that bytecode away.
    const void*        bound_stack = nullptr;
    unsigned long long bound_seq   = 0;
    bool               expr_set    = false;
    double             prev_slot   = 0.0;
    double             now_slot    = 0.0;
    double             sel_slot    = 0.0;
    std::vector<double> ref_slots;

    // Snapshot of the scope name->address bindings this expression was last
    // compiled against. A bumped sequence (from an unrelated inner scope
    // push/pop) re-derives the live bindings and compares against this: if the
    // addresses are unchanged, muParser's bytecode is still valid and we skip
    // the SetExpr/DefineVar/ClearVar calls that would force a recompile.
    std::map<std::string, double*> bound_vars;
};

enum class CompiledKind {
    Unknown,
    SetVariable,
    GetVariable,
    If,
    While,
    Log,
};

enum class ExprOp : uint8_t {
    PushLiteral, PushSlot, PushNow,
    Add, Sub, Mul, Div, Neg,
    Gt, Ge, Lt, Le, Eq, Ne,
};

struct ExprToken {
    ExprOp op;
    double literal = 0.0;
    size_t slot = 0;
};

struct CompiledExpr {
    std::string source;
    std::vector<ExprToken> rpn;
    bool empty_expr = true;
    bool fallback = false;
};

struct CompiledBlock {
    CompiledKind kind = CompiledKind::Unknown;
    const nlohmann::json* source = nullptr;
    std::string name;
    size_t name_slot = 0;
    std::string target;
    size_t target_slot = 0;
    bool has_value = false;
    nlohmann::json value;
    CompiledExpr expression;
    CompiledExpr condition;
    std::string level;
    std::string message;
    int loop_limit = 1;
    std::vector<CompiledBlock> items;
    std::vector<CompiledBlock> consequent;
};

// Holds the per-run scope/context/program. A single run is prepare() then one
// run_pass(); a persisting loop is prepare() once then run_pass() per iteration.
struct PassEngine {
    nlohmann::json context = nlohmann::json::object();
    NumericContextStore numeric_context;
    ScopeSession scope_session;
    std::vector<CompiledBlock> compiled;
    bool use_compiled = false;
    bool compiled_checked = false;
    RunScope run_scope;

    void prepare(const nlohmann::json& roots, const nlohmann::json& ctx, const ExecutionOptions& options);
    void run_pass(const nlohmann::json& roots, const ExecutionOptions& options, ExecutionResult& result);
    void apply_set(const std::string& name, const nlohmann::json& value);
    nlohmann::json snapshot_vars() const;
};

} // namespace media::xblox
