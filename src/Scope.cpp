#include "Scope.h"

#include "Context.h"
#include "ParameterOut.h"
#include "../enums.h" // NHP_GUID_FLOAT

#include "muParser.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace media::xblox {

// ─────────────────────────────────────────────────────────────────────────────
// Scope
// ─────────────────────────────────────────────────────────────────────────────

Scope::Scope(NH_Context* ctx, Scope* parent, ScopeExit policy, unsigned long long* seq)
    : m_ctx(ctx), m_parent(parent), m_policy(policy), m_seq(seq) {}

Scope::~Scope() {
    for (auto& [name, param] : m_vars)
        delete param;
    m_vars.clear();
}

double* Scope::local(const std::string& name) {
    const auto it = m_vars.find(name);
    if (it == m_vars.end() || !it->second)
        return nullptr;
    return it->second->GetScalarPtr();
}

bool Scope::hasLocal(const std::string& name) const {
    return m_vars.find(name) != m_vars.end();
}

double Scope::localValue(const std::string& name) const {
    const auto it = m_vars.find(name);
    return (it != m_vars.end() && it->second) ? it->second->GetScalar() : 0.0;
}

void Scope::clearLocals() {
    for (auto& [name, param] : m_vars)
        delete param;
    m_vars.clear();
    m_order.clear();
    if (m_seq) ++*m_seq;
}

double* Scope::declareLocal(const std::string& name, double value) {
    if (double* existing = local(name)) {
        *existing = value;
        return existing;
    }
    NH_ParameterOut* param = m_ctx
        ? m_ctx->CreateParameterOut(name.c_str(), NHP_GUID_FLOAT)
        : new NH_ParameterOut(0, name.c_str());
    param->SetScalar(value);
    m_vars.emplace(name, param);
    m_order.push_back(name);
    if (m_seq) ++*m_seq; // a new binding invalidates cached expression bytecode
    return param->GetScalarPtr();
}

double* Scope::find(const std::string& name) {
    for (Scope* s = this; s; s = s->m_parent) {
        if (double* p = s->local(name))
            return p;
    }
    return nullptr;
}

double* Scope::findOrDeclare(const std::string& name) {
    if (double* p = find(name))
        return p;
    return declareLocal(name, 0.0);
}

void Scope::set(const std::string& name, double value) {
    *findOrDeclare(name) = value;
}

bool Scope::get(const std::string& name, double& out) {
    if (double* p = find(name)) {
        out = *p;
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScopeStack
// ─────────────────────────────────────────────────────────────────────────────

ScopeStack::ScopeStack(NH_Context* ctx) : m_ctx(ctx) {
    // Root frame: Inherit so top-level writes simply persist.
    m_top = new Scope(m_ctx, nullptr, ScopeExit::Inherit, &m_seq);
    m_depth = 1;
}

ScopeStack::~ScopeStack() {
    while (m_top) {
        Scope* parent = m_top->parent();
        delete m_top;
        m_top = parent;
    }
}

Scope* ScopeStack::push(ScopeExit policy) {
    m_top = new Scope(m_ctx, m_top, policy, &m_seq);
    ++m_depth;
    ++m_seq;
    return m_top;
}

void ScopeStack::pop(std::unordered_map<std::string, double>* collected) {
    if (!m_top || !m_top->parent())
        return; // never pop the root frame

    Scope* frame  = m_top;
    Scope* parent = frame->parent();
    const ScopeExit policy = frame->policy();

    // PREVIOUS always hoists, regardless of policy.
    if (frame->hasLocal(kPreviousVar))
        parent->set(kPreviousVar, frame->localValue(kPreviousVar));

    for (const std::string& name : frame->localNames()) {
        if (name == kPreviousVar)
            continue;
        const double v = frame->localValue(name);
        switch (policy) {
        case ScopeExit::Inherit:
            parent->set(name, v);
            break;
        case ScopeExit::Collect:
            if (collected)
                (*collected)[name] = v;
            break;
        case ScopeExit::Isolate:
            break; // discard
        }
    }

    m_top = parent;
    --m_depth;
    ++m_seq; // popped frame's bindings are gone  -  invalidate cached bytecode
    delete frame;
}

void ScopeStack::resetRoot() {
    while (m_top && m_top->parent()) {
        Scope* parent = m_top->parent();
        delete m_top;
        m_top = parent;
        --m_depth;
    }
    if (m_top)
        m_top->clearLocals();
    m_depth = m_top ? 1 : 0;
    ++m_seq;
}

void ScopeStack::set(const std::string& name, double value) {
    if (m_top) m_top->set(name, value);
}

bool ScopeStack::get(const std::string& name, double& out) {
    return m_top ? m_top->get(name, out) : false;
}

double* ScopeStack::resolveForParser(const char* name) {
    if (!m_top || !name)
        return nullptr;
    return m_top->findOrDeclare(name);
}

double* scope_var_factory(const char* name, void* userData) {
    auto* stack = static_cast<ScopeStack*>(userData);
    return stack ? stack->resolveForParser(name) : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScopeSession
// ─────────────────────────────────────────────────────────────────────────────

ScopeSession::ScopeSession()
    : m_ctx(std::make_unique<NH_Context>()) {
    m_ctx->init();
    m_stack = std::make_unique<ScopeStack>(m_ctx.get());
}

ScopeSession::~ScopeSession() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Self-test
// ─────────────────────────────────────────────────────────────────────────────

int scope_selftest(std::string& report_json) {
    nlohmann::json checks = nlohmann::json::array();
    int failures = 0;
    auto check = [&](const char* label, bool ok) {
        checks.push_back({{"label", label}, {"ok", ok}});
        if (!ok) ++failures;
    };
    auto approx = [](double a, double b) { return std::abs(a - b) < 1e-9; };

    NH_Context ctx;
    ctx.init();

    // 1) child reads parent var via the chain.
    {
        ScopeStack stack(&ctx);
        stack.set("a", 10.0);
        stack.set("b", 5.0);
        stack.push(ScopeExit::Isolate);
        double a = 0.0;
        check("child resolves parent var a==10", stack.get("a", a) && approx(a, 10.0));
        stack.pop();
    }

    // 2) Isolate discards body-local; PREVIOUS still hoists.
    {
        ScopeStack stack(&ctx);
        stack.push(ScopeExit::Isolate);
        stack.set("loopLocal", 200.0);
        stack.set(kPreviousVar, 42.0);
        stack.pop();
        double v = 0.0;
        check("Isolate: loopLocal discarded", !stack.get("loopLocal", v));
        double prev = 0.0;
        check("Isolate: PREVIOUS hoisted==42", stack.get(kPreviousVar, prev) && approx(prev, 42.0));
    }

    // 3) Inherit merges locals up.
    {
        ScopeStack stack(&ctx);
        stack.push(ScopeExit::Inherit);
        stack.set("merged", 7.0);
        stack.pop();
        double v = 0.0;
        check("Inherit: local merged up==7", stack.get("merged", v) && approx(v, 7.0));
    }

    // 4) Collect harvests locals into a map; parent does not see them.
    {
        ScopeStack stack(&ctx);
        stack.push(ScopeExit::Collect);
        stack.set("row0", 1.0);
        stack.set("row1", 2.0);
        std::unordered_map<std::string, double> harvest;
        stack.pop(&harvest);
        check("Collect: harvested 2 locals", harvest.size() == 2 && approx(harvest["row1"], 2.0));
        double v = 0.0;
        check("Collect: parent does not see row0", !stack.get("row0", v));
    }

    // 5) muParser SetVarFactory binds to the scope chain; cached pointer tracks writes.
    {
        ScopeStack stack(&ctx);
        stack.set("a", 10.0);
        stack.set("b", 5.0);
        try {
            mu::Parser parser;
            parser.SetVarFactory(&scope_var_factory, &stack);
            parser.SetExpr("a + b*2");
            const double first = parser.Eval();
            check("muParser factory: a + b*2 == 20", approx(first, 20.0));

            // Mutate `a` through the stack; muParser caches the factory pointer,
            // so a re-Eval must reflect the new value without re-binding.
            stack.set("a", 100.0);
            const double second = parser.Eval();
            check("muParser factory: cached ptr tracks a:=100 -> 110", approx(second, 110.0));
        } catch (const std::exception& e) {
            check((std::string("muParser threw: ") + e.what()).c_str(), false);
        }
    }

    nlohmann::json report = {
        {"ok", failures == 0},
        {"failures", failures},
        {"checks", std::move(checks)},
    };
    report_json = report.dump(2);
    return failures == 0 ? 0 : 1;
}

} // namespace media::xblox
