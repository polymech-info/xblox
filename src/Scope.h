#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// XBlox scope frames (S1).
//
// A Scope is a lexical frame layered on the runtime's already-nested execution
// (containers run child block-lists). Each frame owns its numeric variables as
// NH_ParameterOut instances created through NH_Context, so a variable's value
// lives in the parameter's inline scalar (stable double* via GetScalarPtr)  - 
// exactly what muParser binds to through SetVarFactory (see ScopeStack).
//
// Name resolution walks child -> parent (lexical). On pop, the frame applies an
// exit policy declared by the owning container:
//
//   Inherit   -  merge locals up into the parent          (if / sequences)
//   Isolate   -  discard locals                            (for counter, temps)
//   Collect   -  gather locals into a result map           (for / map harvest)
//
// PREVIOUS always hoists to the parent regardless of policy  -  it is the block's
// "last result" and the next sibling expects it.
// ─────────────────────────────────────────────────────────────────────────────

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class NH_Context;
class NH_ParameterOut;

namespace media::xblox {

enum class ScopeExit {
    Inherit,
    Isolate,
    Collect,
};

// Sentinel variable name that always hoists out of a child frame.
inline constexpr const char* kPreviousVar = "PREVIOUS";

// ─────────────────────────────────────────────────────────────────────────────
// Scope  -  a single frame. Owns its NH_ParameterOut variables.
// ─────────────────────────────────────────────────────────────────────────────
class Scope {
public:
    // `seq` (optional) points at the owning ScopeStack's structural-change
    // counter; declareLocal bumps it when a *new* variable is created so the
    // expression layer knows when a cached muParser binding is still valid.
    Scope(NH_Context* ctx, Scope* parent, ScopeExit policy, unsigned long long* seq = nullptr);
    ~Scope();

    Scope*    parent() const { return m_parent; }
    ScopeExit policy() const { return m_policy; }

    // Resolve walking child -> parent. Returns a stable double* or nullptr.
    double* find(const std::string& name);

    // Resolve for write: returns the stable pointer of the nearest frame that
    // already defines `name`; if none does, declares it in THIS frame.
    double* findOrDeclare(const std::string& name);

    // Declare (or fetch) a variable in THIS frame; returns its stable pointer.
    double* declareLocal(const std::string& name, double value = 0.0);

    // set: write to nearest defining frame, else declare locally here.
    void set(const std::string& name, double value);
    // get: read via the chain.
    bool get(const std::string& name, double& out);

    bool hasLocal(const std::string& name) const;
    const std::vector<std::string>& localNames() const { return m_order; }
    double localValue(const std::string& name) const;
    void clearLocals();

private:
    double* local(const std::string& name);

    NH_Context* m_ctx;
    Scope*      m_parent;
    ScopeExit   m_policy;
    unsigned long long* m_seq = nullptr; // owning stack's structural counter
    std::unordered_map<std::string, NH_ParameterOut*> m_vars;
    std::vector<std::string> m_order; // declaration order for stable harvest
};

// ─────────────────────────────────────────────────────────────────────────────
// ScopeStack  -  frame stack + muParser variable factory binding.
// ─────────────────────────────────────────────────────────────────────────────
class ScopeStack {
public:
    explicit ScopeStack(NH_Context* ctx);
    ~ScopeStack();

    Scope* current() { return m_top; }

    // Monotonic counter bumped on every structural change (push, pop, or a new
    // variable declaration). Value-only writes do NOT bump it. The expression
    // layer caches muParser bytecode and reuses it while this is unchanged for a
    // given stack  -  see eval_mu_expression.
    unsigned long long sequence() const { return m_seq; }

    // Push a child frame with the given exit policy; returns the new top.
    Scope* push(ScopeExit policy);

    // Pop the top frame, applying its exit policy. PREVIOUS always hoists.
    // When policy == Collect and `collected` is non-null, the frame's locals
    // (except PREVIOUS) are copied into it before discarding.
    void pop(std::unordered_map<std::string, double>* collected = nullptr);

    // Return to a clean root frame while keeping the NH_Context allocation alive.
    void resetRoot();

    // Pass-throughs to the top frame.
    void set(const std::string& name, double value);
    bool get(const std::string& name, double& out);

    // muParser SetVarFactory target: resolve `name` against the chain, lazily
    // declaring it in the top frame when unknown. Always returns a stable ptr.
    double* resolveForParser(const char* name);

private:
    NH_Context* m_ctx;
    Scope*      m_top = nullptr;
    int         m_depth = 0;
    unsigned long long m_seq = 1; // structural-change counter (0 = "never bound")
};

// ─────────────────────────────────────────────────────────────────────────────
// ScopeSession  -  owns an NH_Context + ScopeStack as a single unit.
//
// pimpl over NH_Context so translation units that only need a scope stack
// (e.g. the executor, which also pulls <windows.h>) don't have to include the
// NH headers  -  `types.h` defines E_FAIL/E_NOTIMPL/... which collide with Win32
// HRESULT macros.
// ─────────────────────────────────────────────────────────────────────────────
class ScopeSession {
public:
    ScopeSession();
    ~ScopeSession();
    ScopeSession(const ScopeSession&) = delete;
    ScopeSession& operator=(const ScopeSession&) = delete;

    ScopeStack& stack() { return *m_stack; }

private:
    std::unique_ptr<NH_Context> m_ctx;
    std::unique_ptr<ScopeStack> m_stack;
};

// facfun_type-compatible trampoline: userData must be a ScopeStack*.
double* scope_var_factory(const char* name, void* userData);

// Self-test exercising the chain, exit policies, PREVIOUS hoist, and the
// muParser SetVarFactory binding. Writes a JSON summary; returns 0 on success.
int scope_selftest(std::string& report_json);

} // namespace media::xblox
