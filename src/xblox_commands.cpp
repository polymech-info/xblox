#include "xblox_commands.hpp"

#include "Scope_Variables.hpp"
#include "xblox_json.hpp"
#include "xblox_strings.hpp"
#include "xblox_types.hpp"
#include "xblox_constants.hpp"
#include "xblox_perf.hpp"
#include "xblox_expressions.hpp"

#include "blocks/builtin_blocks.hpp"
#include "cli/cli_ui_launch.hpp"
#include "core/app_exe_directory.hpp"
#include "core/command_variables.hpp"
#include "pm_branding_config.hpp"
#include "process/process_helpers.hpp"
#include "logging/logger.h"
#include "muParser.h"
#include "Scope.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif
#include "string/string_utils.hpp"

namespace fs = std::filesystem;

namespace media::xblox {

namespace {

bool block_profile_enabled()
{
    static const bool enabled = std::getenv("TANIT_XBLOX_PROFILE_BLOCKS") != nullptr;
    return enabled;
}

struct BlockProfileTotals {
    std::mutex mutex;
    std::map<std::string, std::pair<unsigned long long, unsigned long long>> by_kind;

    ~BlockProfileTotals()
    {
        if (!block_profile_enabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        std::fprintf(stderr, "[xblox:profile] block totals\n");
        for (const auto& [kind, totals] : by_kind) {
            const auto count = totals.first;
            const auto micros = totals.second;
            const double avg = count ? static_cast<double>(micros) / static_cast<double>(count) : 0.0;
            std::fprintf(stderr, "[xblox:profile] kind=%s count=%llu total_us=%llu avg_us=%.2f\n",
                         kind.c_str(), count, micros, avg);
        }
    }
};

BlockProfileTotals& block_profile_totals()
{
    static BlockProfileTotals totals;
    return totals;
}

struct BlockProfileScope {
    std::string kind;
    std::chrono::steady_clock::time_point start;

    explicit BlockProfileScope(std::string k)
        : kind(std::move(k)), start(std::chrono::steady_clock::now())
    {
    }

    ~BlockProfileScope()
    {
        if (!block_profile_enabled())
            return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        auto& totals = block_profile_totals();
        std::lock_guard<std::mutex> lock(totals.mutex);
        auto& item = totals.by_kind[kind.empty() ? "unknown" : kind];
        ++item.first;
        item.second += static_cast<unsigned long long>(std::max<long long>(0, elapsed));
    }
};

std::string action_name(const nlohmann::json& command)
{
    if (!json_string(command, "cliCommand").empty()) return "cli";
    if (!json_string(command, "appCommand").empty()) return "app";
    if (!json_string(command, "ribbonCommand").empty()) return "ribbon";
    if (command.contains("externalCommand") && command["externalCommand"].is_object()) return "external";
    if (!json_string(command, "url").empty()) return "url";
    if (!json_string(command, "path").empty()) return "path";
    return "metadata";
}

bool is_action_command(const nlohmann::json& command)
{
    return command.is_object() && (
        command.contains("cliCommand") || command.contains("appCommand") ||
        command.contains("ribbonCommand") || command.contains("externalCommand") ||
        command.contains("url") || command.contains("path"));
}

const nlohmann::json* groups_from_commands_doc(const nlohmann::json& doc)
{
    if (doc.is_object() && doc.contains("ribbon") && doc["ribbon"].is_object() &&
        doc["ribbon"].contains("groups") && doc["ribbon"]["groups"].is_array())
        return &doc["ribbon"]["groups"];
    if (doc.is_object() && doc.contains("groups") && doc["groups"].is_array())
        return &doc["groups"];
    if (doc.is_array())
        return &doc;
    return nullptr;
}

bool find_command_in_items(const nlohmann::json& items, std::string_view id, nlohmann::json& out)
{
    if (!items.is_array())
        return false;
    for (const auto& item : items) {
        if (!item.is_object())
            continue;
        if (item.value("enabled", true) == false || item.value("visible", true) == false)
            continue;
        if (json_string(item, "id") == id) {
            out = item;
            return true;
        }
        if (find_command_in_items(item.value("items", nlohmann::json::array()), id, out))
            return true;
    }
    return false;
}

bool find_command_by_id(const nlohmann::json& doc, std::string_view id, nlohmann::json& out)
{
    const nlohmann::json* groups = groups_from_commands_doc(doc);
    if (!groups)
        return false;
    for (const auto& group : *groups) {
        const nlohmann::json* items = nullptr;
        if (group.is_object() && group.contains("items"))
            items = &group["items"];
        else if (group.is_array())
            items = &group;
        if (items && find_command_in_items(*items, id, out))
            return true;
    }
    return false;
}

bool context_selection_has_files(const nlohmann::json& context)
{
    if (!context.is_object() || !context.contains("selection") || !context["selection"].is_object())
        return false;
    const auto& files = context["selection"].value("files", nlohmann::json::array());
    return files.is_array() && !files.empty();
}

bool simple_context_name(const std::string& name)
{
    return !name.empty() && name.find('.') == std::string::npos;
}

size_t numeric_slot_for(NumericContextStore& numeric_context, const std::string& name)
{
    const auto found = numeric_context.slots.find(name);
    if (found != numeric_context.slots.end())
        return found->second;
    const size_t slot = numeric_context.slot_values.size();
    numeric_context.slots[name] = slot;
    numeric_context.slot_values.push_back(0.0);
    return slot;
}

bool numeric_slot_value(const NumericContextStore& numeric_context, size_t slot, double& out)
{
    if (slot >= numeric_context.slot_values.size())
        return false;
    out = numeric_context.slot_values[slot];
    return true;
}

void set_numeric_slot(NumericContextStore& numeric_context, const std::string& name, double value)
{
    numeric_context.values[name] = value;
    numeric_context.slot_values[numeric_slot_for(numeric_context, name)] = value;
}

void remember_numeric_value(NumericContextStore& numeric_context, const std::string& name, const nlohmann::json& value)
{
    if (!simple_context_name(name))
        return;
    double number = 0.0;
    if (json_number(value, number)) {
        set_numeric_slot(numeric_context, name, number);
    } else {
        numeric_context.values.erase(name);
    }
}

void seed_numeric_context(const nlohmann::json& context, NumericContextStore& numeric_context)
{
    numeric_context.values.clear();
    std::fill(numeric_context.slot_values.begin(), numeric_context.slot_values.end(), 0.0);
    if (!context.is_object())
        return;
    for (auto it = context.begin(); it != context.end(); ++it)
        remember_numeric_value(numeric_context, it.key(), it.value());
}

bool context_number(const nlohmann::json& context, const std::string& name, double& out, const NumericContextStore* numeric_context = nullptr)
{
    if (name == "nowMs") {
        out = steady_now_ms();
        return true;
    }
    if (numeric_context && simple_context_name(name)) {
        const auto it = numeric_context->values.find(name);
        if (it != numeric_context->values.end()) {
            out = it->second;
            return true;
        }
        const auto slot = numeric_context->slots.find(name);
        if (slot != numeric_context->slots.end() && numeric_slot_value(*numeric_context, slot->second, out))
            return true;
    }
    const nlohmann::json* value = context_value_ptr(context, name);
    return value && json_number(*value, out);
}

int context_selection_file_count(const nlohmann::json& context)
{
    if (!context.is_object() || !context.contains("selection") || !context["selection"].is_object())
        return 0;
    const auto& files = context["selection"].value("files", nlohmann::json::array());
    return files.is_array() ? static_cast<int>(files.size()) : 0;
}

// Identifier sanitation lives in xblox_expressions; the full expression
// normalization happens via expr::prepare_expression in eval_mu_expression.
std::string mu_var_name(std::string s)
{
    return expr::mu_var_name(std::move(s));
}

void collect_numeric_vars(const nlohmann::json& value, const std::string& prefix, std::map<std::string, double>& vars)
{
    if (prefix.empty()) {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it)
                collect_numeric_vars(it.value(), mu_var_name(it.key()), vars);
        }
        return;
    }
    if (value.is_boolean()) {
        vars[prefix] = value.get<bool>() ? 1.0 : 0.0;
    } else if (value.is_number()) {
        vars[prefix] = value.get<double>();
    } else if (value.is_string()) {
        try {
            size_t consumed = 0;
            const double parsed = std::stod(value.get<std::string>(), &consumed);
            if (consumed == value.get<std::string>().size())
                vars[prefix] = parsed;
        } catch (...) {
        }
    } else if (value.is_array()) {
        vars[prefix + "_count"] = static_cast<double>(value.size());
        vars[prefix + "_length"] = static_cast<double>(value.size());
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it)
            collect_numeric_vars(it.value(), prefix + "_" + mu_var_name(it.key()), vars);
    }
}

CachedMuExpression& cached_mu_expression(const std::string& expr)
{
    constexpr size_t kMaxCachedExpressions = 128;
    thread_local std::unordered_map<std::string, std::unique_ptr<CachedMuExpression>> cache;
    if (cache.size() > kMaxCachedExpressions)
        cache.clear();

    auto it = cache.find(expr);
    if (it == cache.end()) {
        auto cached = std::make_unique<CachedMuExpression>();
        it = cache.emplace(expr, std::move(cached)).first;
    }
    return *it->second;
}

// Resolve a single trimmed token to a number: a full numeric literal, or a
// simple variable name read from the scope/numeric/JSON context. Anything else
// (operators, parens, dotted/compound forms) fails so the caller falls back.
bool fast_operand(const std::string& token, const nlohmann::json& context, const NumericContextStore* numeric_context, double& out)
{
    if (token.empty())
        return false;
    char* end = nullptr;
    const double literal = std::strtod(token.c_str(), &end);
    if (end && *end == '\0') {
        out = literal;
        return true;
    }
    if (simple_context_name(token))
        return context_number(context, token, out, numeric_context);
    return false;
}

// Minimal single-binary-operator fast path for loop counters / conditions:
//   "<var|num> {>=,<=,==,!=,>,<} <var|num>"   (comparisons -> 1.0/0.0)
//   "<var> {+,-,*,/} <var|num>"               (arithmetic)
// Both operands must be a clean literal or simple variable. Anything with
// precedence, parentheses, multiple operators, or unrecognised tokens returns
// false and falls through to the cached muParser path  -  so this never changes
// results, it only skips muParser (and its per-eval scope rebind) for the
// common shapes that dominate `for`/`while`.
bool eval_simple_binary(const std::string& expr, const nlohmann::json& context, const NumericContextStore* numeric_context, double& out)
{
    static const struct { const char* tok; std::size_t len; int op; } kCmp[] = {
        {">=", 2, 1}, {"<=", 2, 2}, {"==", 2, 3}, {"!=", 2, 4}, {">", 1, 5}, {"<", 1, 6},
    };
    for (const auto& c : kCmp) {
        const std::size_t p = expr.find(c.tok);
        if (p == std::string::npos)
            continue;
        double a = 0.0, b = 0.0;
        if (!fast_operand(trim_ascii(expr.substr(0, p)), context, numeric_context, a) ||
            !fast_operand(trim_ascii(expr.substr(p + c.len)), context, numeric_context, b))
            return false; // a comparison op is present but it isn't a clean simple compare
        switch (c.op) {
        case 1: out = (a >= b) ? 1.0 : 0.0; return true;
        case 2: out = (a <= b) ? 1.0 : 0.0; return true;
        case 3: out = (std::abs(a - b) <= 1e-12) ? 1.0 : 0.0; return true;
        case 4: out = (std::abs(a - b) >  1e-12) ? 1.0 : 0.0; return true;
        case 5: out = (a >  b) ? 1.0 : 0.0; return true;
        case 6: out = (a <  b) ? 1.0 : 0.0; return true;
        }
        return false;
    }

    // arithmetic: a leading simple variable, one operator, then literal|var.
    std::size_t i = 0;
    while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
    const std::size_t name_begin = i;
    while (i < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_' || expr[i] == '.')) ++i;
    const std::string name = expr.substr(name_begin, i - name_begin);
    if (name.empty() || !simple_context_name(name))
        return false;
    while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
    if (i >= expr.size())
        return false;
    const char op = expr[i];
    if (op != '+' && op != '-' && op != '*' && op != '/')
        return false;
    double a = 0.0, b = 0.0;
    if (!context_number(context, name, a, numeric_context) ||
        !fast_operand(trim_ascii(expr.substr(i + 1)), context, numeric_context, b))
        return false;
    switch (op) {
    case '+': out = a + b; return true;
    case '-': out = a - b; return true;
    case '*': out = a * b; return true;
    case '/': if (std::abs(b) <= 1e-12) return false; out = a / b; return true;
    }
    return false;
}

bool eval_fast_expression(const std::string& expression, const nlohmann::json& context, const NumericContextStore* numeric_context, double& out)
{
    const std::string expr = trim_ascii(expression);
    if (expr.empty())
        return false;

    char* end = nullptr;
    const double literal = std::strtod(expr.c_str(), &end);
    if (end && *end == '\0') {
        out = literal;
        return true;
    }

    if (context_number(context, expr, out, numeric_context))
        return true;

    if (eval_simple_binary(expr, context, numeric_context, out))
        return true;

    // Compound shapes (multiple operators, parens, `nowMs` arithmetic) fall
    // through to the cached muParser path. The events-disabled benchmark runs
    // instead take the compiled RPN engine, so no benchmark-specific strings
    // need to live here.
    return false;
}

// Bind every variable visible in the scope chain to muParser, using each
// variable's stable inline scalar pointer (NH_Parameter). Walks child -> parent
// so a deeper frame shadows an outer one (lexical resolution); names are
// sanitized to match how expression tokens are normalized. This makes the scope
// stack the single source of truth for user/loop numerics  -  muParser no longer
// reads the JSON context mirror.
// Walk the live scope chain and collect the sanitized name -> stable address
// bindings muParser should see (a deeper frame shadows a shallower one). This
// is a cheap pointer walk; it does NOT touch muParser (which would force a
// bytecode recompile).
std::map<std::string, double*> collect_scope_bindings(ScopeStack& scopes)
{
    std::map<std::string, double*> bound; // sanitized name -> stable ptr (child wins)
    Scope* top = scopes.current();
    if (!top)
        return bound;
    for (Scope* s = top; s; s = s->parent()) {
        for (const std::string& raw : s->localNames()) {
            const std::string name = mu_var_name(raw);
            if (bound.count(name))
                continue; // a deeper frame already shadowed this name
            if (double* p = top->find(raw))
                bound.emplace(name, p);
        }
    }
    return bound;
}

void bind_scope_vars(ScopeStack& scopes, mu::Parser& parser)
{
    for (const auto& [name, ptr] : collect_scope_bindings(scopes))
        parser.DefineVar(name, ptr);
}

bool eval_mu_expression(const std::string& expression, const nlohmann::json& context, double& out, std::string* err = nullptr, const NumericContextStore* numeric_context = nullptr, ScopeStack* scopes = nullptr)
{
    ++g_xblox_perf.expr_calls;

    std::string resolved_expression;
    { ScopedPerfTimer _t(g_xblox_perf.resolve_ns);
      resolved_expression = media::commands::resolve_variables(expression, media::commands::VariableMap{}, nullptr, true); }

    if (eval_fast_expression(resolved_expression, context, numeric_context, out))
        return true;

    // Unified front-end (xblox_expressions): legacy aliases, `${ref}` unwrap,
    // string comparisons folded against the context, nested references
    // (`PREVIOUS.data[0]`) rewritten to synthetic slots resolved per eval.
    expr::PreparedExpression prep;
    { ScopedPerfTimer _t(g_xblox_perf.prepare_ns);
      prep = expr::prepare_expression(resolved_expression, context); }
    if (prep.text.empty()) {
        if (err) *err = "empty expression";
        return false;
    }

    // The pre-pass may reduce the source to a fast shape ("1", "flag > 0").
    if (prep.changed && eval_fast_expression(prep.text, context, numeric_context, out))
        return true;

    try {
        ScopedPerfTimer _t(g_xblox_perf.mu_eval_ns);
        CachedMuExpression& cached = cached_mu_expression(prep.text);

        // Same cache key ⇒ same slot count; sized once before any binding so
        // the addresses handed to DefineVar stay stable across evaluations.
        if (cached.ref_slots.size() != prep.refs.size())
            cached.ref_slots.assign(prep.refs.size(), 0.0);
        for (size_t i = 0; i < prep.refs.size(); ++i) {
            double value = 0.0;
            if (!expr::reference_number(context, prep.refs[i].path, value)) {
                if (err) *err = "unresolved reference: " + prep.refs[i].path;
                return false;
            }
            cached.ref_slots[i] = value;
        }

        if (scopes) {
            // Reuse muParser's compiled bytecode across evaluations. Any
            // SetExpr/DefineVar/ClearVar call forces muParser to recompile the
            // expression on the next Eval(), so we avoid them whenever the
            // bindings this expression sees are unchanged.
            //
            // The scope stack's sequence() bumps on *any* structural change  - 
            // including push/pop of inner if/while frames that this expression
            // doesn't reference. That coarse signal alone would recompile on
            // nearly every pass. So when the sequence moves we re-derive the
            // live name->address bindings (a cheap pointer walk) and only
            // recompile if an address actually changed; identical bindings keep
            // the cached bytecode and just refresh the seq stamp.
            const unsigned long long seq = scopes->sequence();
            bool needs_rebind = !cached.expr_set || cached.bound_stack != scopes;
            if (!needs_rebind && cached.bound_seq != seq) {
                std::map<std::string, double*> current = collect_scope_bindings(*scopes);
                if (current == cached.bound_vars) {
                    // Same names -> same stable addresses: bytecode still valid.
                    cached.bound_seq = seq; // skip the walk until the next change
                    ++g_xblox_perf.expr_prep_hits;
                } else {
                    cached.bound_vars = std::move(current);
                    needs_rebind = true;
                }
            } else if (!needs_rebind) {
                ++g_xblox_perf.expr_prep_hits;
            }
            if (needs_rebind) {
                cached.vars.clear();
                cached.parser.ClearVar();
                // Scope is the single source for user/loop numerics  -  bind stable
                // inline pointers straight from the frame chain (no JSON mirror).
                if (cached.bound_vars.empty() || cached.bound_stack != scopes)
                    cached.bound_vars = collect_scope_bindings(*scopes);
                for (const auto& [name, ptr] : cached.bound_vars)
                    cached.parser.DefineVar(name, ptr);
                // PREVIOUS is intentionally global (never frame-scoped), nowMs is
                // a clock input and selectionCount is derived from the context's
                // selection; all are bound once to stable slots in the cache
                // entry and refreshed by value (no rebind) before each Eval.
                cached.parser.DefineVar(kPreviousVar, &cached.prev_slot);
                cached.parser.DefineVar("nowMs", &cached.now_slot);
                cached.parser.DefineVar("selectionCount", &cached.sel_slot);
                // Nested-reference slots: bound to stable cache storage, the
                // values are refreshed before every Eval (above).
                for (size_t i = 0; i < prep.refs.size(); ++i)
                    cached.parser.DefineVar(prep.refs[i].name, &cached.ref_slots[i]);
                cached.parser.SetExpr(prep.text);
                cached.expr_set    = true;
                cached.bound_stack = scopes;
                cached.bound_seq   = seq;
            }
            cached.prev_slot = 0.0;
            if (numeric_context) {
                const auto it = numeric_context->values.find(kPreviousVar);
                if (it != numeric_context->values.end())
                    cached.prev_slot = it->second;
            }
            cached.now_slot = steady_now_ms();
            cached.sel_slot = static_cast<double>(context_selection_file_count(context));
            out = cached.parser.Eval();
            return true;
        }

        // Fallback (detached background runs without a scope stack): legacy
        // JSON-derived variable pool, rebuilt each eval.
        cached.vars.clear();
        cached.parser.ClearVar();
        collect_numeric_vars(context, {}, cached.vars);
        cached.vars["selectionCount"] = static_cast<double>(context_selection_file_count(context));
        cached.vars["hasQueuedWork"] = context.value("hasQueuedWork", true) ? 1.0 : 0.0;
        cached.vars["nowMs"] = steady_now_ms();
        for (size_t i = 0; i < prep.refs.size(); ++i)
            cached.vars[prep.refs[i].name] = cached.ref_slots[i];
        for (auto& [name, value] : cached.vars)
            cached.parser.DefineVar(name, &value);
        cached.parser.SetExpr(prep.text);
        cached.expr_set    = false; // legacy path leaves bindings unstable
        cached.bound_stack = nullptr;
        out = cached.parser.Eval();
        return true;
    } catch (const mu::Parser::exception_type& e) {
        if (err) *err = e.GetMsg();
        return false;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool condition_truthy(const std::string& condition, const nlohmann::json& context, const NumericContextStore* numeric_context = nullptr, ScopeStack* scopes = nullptr)
{
    double evaluated = 0.0;
    if (eval_mu_expression(condition, context, evaluated, nullptr, numeric_context, scopes))
        return std::abs(evaluated) > 1e-12;

    const std::string c = lower_compact(trim_ascii(condition));
    if (c.empty() || c == "false" || c == "0" || c == "null" || c == "undefined")
        return false;
    if (c.find("selection") != std::string::npos)
        return context_selection_has_files(context);
    if (c.find("hasqueuedwork") != std::string::npos)
        return context.value("hasQueuedWork", true);
    return true;
}

BlockRunSpec block_run_flags(const nlohmann::json& block)
{
    BlockRunSpec spec;
    if (!block.is_object())
        return spec;

    const std::string kind = json_string(block, "kind");
    if (kind == "command" || kind == "shell" || kind == "Shell")
        spec.set(BlockRunFlag::AbortOnError, false);

    spec.set(BlockRunFlag::Enabled,       json_bool_or(block, "enabled", spec.enabled()));
    spec.set(BlockRunFlag::AbortOnError,  json_bool_or(block, "abortOnError", spec.abort_on_error()));
    if (kind != "xbloxRun")
        spec.set(BlockRunFlag::Background, json_bool_or(block, "background", spec.background()));
    spec.set(BlockRunFlag::Cancellable,    json_bool_or(block, "cancellable", spec.cancellable()));
    spec.set(BlockRunFlag::ConsumeEvents,  json_bool_or(block, "consumeEvents", spec.consume_events()));
    if (block.contains("timeoutMs") && block["timeoutMs"].is_number_integer())
        spec.timeout_ms = std::max(0, block["timeoutMs"].get<int>());

    if (block.contains("continueOnError") && block["continueOnError"].is_boolean())
        spec.set(BlockRunFlag::AbortOnError, !block["continueOnError"].get<bool>());

    if (block.contains("runFlags") && block["runFlags"].is_object()) {
        const auto& run = block["runFlags"];
        spec.set(BlockRunFlag::AbortOnError,  json_bool_or(run, "abortOnError", spec.abort_on_error()));
        spec.set(BlockRunFlag::Enabled,       json_bool_or(run, "enabled", spec.enabled()));
        spec.set(BlockRunFlag::Background,     json_bool_or(run, "background", spec.background()));
        spec.set(BlockRunFlag::Cancellable,    json_bool_or(run, "cancellable", spec.cancellable()));
        spec.set(BlockRunFlag::ConsumeEvents,  json_bool_or(run, "consumeEvents", spec.consume_events()));
        if (run.contains("continueOnError") && run["continueOnError"].is_boolean())
            spec.set(BlockRunFlag::AbortOnError, !run["continueOnError"].get<bool>());
        if (run.contains("timeoutMs") && run["timeoutMs"].is_number_integer())
            spec.timeout_ms = std::max(0, run["timeoutMs"].get<int>());
    }

    const std::string on_error = lower_compact(json_string(block, "onError"));
    if (on_error == "continue")
        spec.set(BlockRunFlag::AbortOnError, false);
    else if (on_error == "abort" || on_error == "stop")
        spec.set(BlockRunFlag::AbortOnError, true);

    return spec;
}

std::string block_store_name(const nlohmann::json& block)
{
    if (!block.is_object())
        return {};
    std::string name = json_string(block, "storeAs");
    if (name.empty())
        name = json_string(block, "storeVariable");
    if (name.empty())
        name = json_string(block, "storeResult");
    if (name.empty() && block.contains("runFlags") && block["runFlags"].is_object()) {
        name = json_string(block["runFlags"], "storeAs");
        if (name.empty())
            name = json_string(block["runFlags"], "storeVariable");
    }
    return name;
}

bool context_numeric_value(const nlohmann::json& context, const std::string& expression, double& out, const NumericContextStore* numeric_context = nullptr, ScopeStack* scopes = nullptr)
{
    return eval_mu_expression(expression, context, out, nullptr, numeric_context, scopes);
}

std::string context_string_value(const nlohmann::json& context, std::string variable)
{
    variable = trim_ascii(std::move(variable));
    if (variable.rfind("this.", 0) == 0)
        variable.erase(0, 5);
    if (const nlohmann::json* value = context_value_ptr(context, variable)) {
        if (value->is_string())
            return value->get<std::string>();
        if (value->is_number_float())
            return std::to_string(value->get<double>());
        if (value->is_number_integer())
            return std::to_string(value->get<long long>());
        if (value->is_boolean())
            return value->get<bool>() ? "1" : "0";
    }
    return variable;
}

void emit(ExecutionResult& result, const ExecutionOptions& options, ExecutionEvent event);
bool cancel_requested(const ExecutionOptions& options);
blocks::BlockResult execute_block(const nlohmann::json& block,
                   const std::string& path,
                   nlohmann::json& context,
                   NumericContextStore& numeric_context,
                   const ExecutionOptions& options,
                   ExecutionResult& result,
                   FlowState& flow);

CompiledExpr compile_expr(std::string expression, NumericContextStore& numeric_context)
{
    CompiledExpr out;
    out.source = trim_ascii(std::move(expression));
    if (out.source.empty())
        return out; // empty_expr stays true
    out.empty_expr = false;

    // Shunting-yard: tokens -> RPN. On any unsupported construct, mark fallback
    // and let muParser handle the original source (never changes results).
    const auto give_up = [&]() { out.fallback = true; out.rpn.clear(); return out; };

    struct OpMark { int prec; bool right; bool paren; ExprOp op; };
    std::vector<OpMark> ops;
    const std::string& s = out.source;
    std::size_t i = 0;
    bool expect_operand = true; // start / after an operator / after '('

    const auto emit_op = [&](const OpMark& m) { out.rpn.push_back(ExprToken{m.op, 0.0, 0}); };
    const auto push_operator = [&](const OpMark& m) {
        while (!ops.empty() && !ops.back().paren &&
               (ops.back().prec > m.prec || (ops.back().prec == m.prec && !m.right))) {
            emit_op(ops.back());
            ops.pop_back();
        }
        ops.push_back(m);
    };

    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            char* end = nullptr;
            const double v = std::strtod(s.c_str() + i, &end);
            if (!end || end == s.c_str() + i)
                return give_up();
            out.rpn.push_back(ExprToken{ExprOp::PushLiteral, v, 0});
            i = static_cast<std::size_t>(end - s.c_str());
            expect_operand = false;
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            const std::string name = s.substr(i, j - i);
            i = j;
            if (i < s.size() && s[i] == '.')
                return give_up(); // dotted/compound name  -  not modelled
            if (name == "nowMs")
                out.rpn.push_back(ExprToken{ExprOp::PushNow, 0.0, 0});
            else
                out.rpn.push_back(ExprToken{ExprOp::PushSlot, 0.0, numeric_slot_for(numeric_context, name)});
            expect_operand = false;
            continue;
        }

        if (c == '(') {
            ops.push_back(OpMark{0, false, true, ExprOp::PushLiteral});
            ++i; expect_operand = true; continue;
        }
        if (c == ')') {
            bool matched = false;
            while (!ops.empty()) {
                if (ops.back().paren) { ops.pop_back(); matched = true; break; }
                emit_op(ops.back()); ops.pop_back();
            }
            if (!matched)
                return give_up();
            ++i; expect_operand = false; continue;
        }

        // Operators (two-char comparisons first).
        const char c2 = (i + 1 < s.size()) ? s[i + 1] : '\0';
        if (c == '>' && c2 == '=') { push_operator({1, false, false, ExprOp::Ge}); i += 2; expect_operand = true; continue; }
        if (c == '<' && c2 == '=') { push_operator({1, false, false, ExprOp::Le}); i += 2; expect_operand = true; continue; }
        if (c == '=' && c2 == '=') { push_operator({1, false, false, ExprOp::Eq}); i += 2; expect_operand = true; continue; }
        if (c == '!' && c2 == '=') { push_operator({1, false, false, ExprOp::Ne}); i += 2; expect_operand = true; continue; }
        if (c == '>')              { push_operator({1, false, false, ExprOp::Gt}); ++i; expect_operand = true; continue; }
        if (c == '<')              { push_operator({1, false, false, ExprOp::Lt}); ++i; expect_operand = true; continue; }
        if (c == '+') {
            if (!expect_operand) push_operator({2, false, false, ExprOp::Add});
            // unary plus: no-op
            ++i; expect_operand = true; continue;
        }
        if (c == '-') {
            if (expect_operand) push_operator({4, true, false, ExprOp::Neg});
            else                push_operator({2, false, false, ExprOp::Sub});
            ++i; expect_operand = true; continue;
        }
        if (c == '*') { push_operator({3, false, false, ExprOp::Mul}); ++i; expect_operand = true; continue; }
        if (c == '/') { push_operator({3, false, false, ExprOp::Div}); ++i; expect_operand = true; continue; }

        return give_up(); // unsupported character/operator
    }

    while (!ops.empty()) {
        if (ops.back().paren)
            return give_up(); // unbalanced '('
        emit_op(ops.back());
        ops.pop_back();
    }

    if (out.rpn.empty())
        return give_up();
    return out;
}

bool eval_compiled_expr(const CompiledExpr& expression,
                        nlohmann::json& context,
                        NumericContextStore& numeric_context,
                        double& out)
{
    if (expression.empty_expr)
        return false;
    if (expression.fallback)
        return eval_mu_expression(expression.source, context, out, nullptr, &numeric_context);

    double stack[64];
    int sp = 0;
    for (const ExprToken& t : expression.rpn) {
        switch (t.op) {
        case ExprOp::PushLiteral:
            if (sp >= 64) return false;
            stack[sp++] = t.literal;
            break;
        case ExprOp::PushNow:
            if (sp >= 64) return false;
            stack[sp++] = steady_now_ms();
            break;
        case ExprOp::PushSlot: {
            double v = 0.0;
            numeric_slot_value(numeric_context, t.slot, v); // unset slot reads 0
            if (sp >= 64) return false;
            stack[sp++] = v;
            break;
        }
        case ExprOp::Neg:
            if (sp < 1) return false;
            stack[sp - 1] = -stack[sp - 1];
            break;
        default: {
            if (sp < 2) return false;
            const double b = stack[--sp];
            const double a = stack[--sp];
            double r = 0.0;
            switch (t.op) {
            case ExprOp::Add: r = a + b; break;
            case ExprOp::Sub: r = a - b; break;
            case ExprOp::Mul: r = a * b; break;
            case ExprOp::Div: if (std::abs(b) <= 1e-12) return false; r = a / b; break;
            case ExprOp::Gt:  r = (a >  b) ? 1.0 : 0.0; break;
            case ExprOp::Ge:  r = (a >= b) ? 1.0 : 0.0; break;
            case ExprOp::Lt:  r = (a <  b) ? 1.0 : 0.0; break;
            case ExprOp::Le:  r = (a <= b) ? 1.0 : 0.0; break;
            case ExprOp::Eq:  r = (std::abs(a - b) <= 1e-12) ? 1.0 : 0.0; break;
            case ExprOp::Ne:  r = (std::abs(a - b) >  1e-12) ? 1.0 : 0.0; break;
            default: return false;
            }
            stack[sp++] = r;
            break;
        }
        }
    }
    if (sp != 1)
        return false;
    out = stack[0];
    return true;
}

bool compare_numbers(double lhs, const std::string& comparator, double rhs)
{
    if (comparator == "!=" || comparator == "!==" || comparator == "<>")
        return std::abs(lhs - rhs) > 1e-12;
    if (comparator == ">" || comparator == "gt")
        return lhs > rhs;
    if (comparator == ">=" || comparator == "gte")
        return lhs >= rhs;
    if (comparator == "<" || comparator == "lt")
        return lhs < rhs;
    if (comparator == "<=" || comparator == "lte")
        return lhs <= rhs;
    return std::abs(lhs - rhs) <= 1e-12;
}

const nlohmann::json& json_array_or_empty_local(const nlohmann::json& o, const char* key)
{
    static const nlohmann::json empty = nlohmann::json::array();
    if (!o.is_object() || !o.contains(key) || !o[key].is_array())
        return empty;
    return o[key];
}

void compile_block_list(const nlohmann::json& blocks, NumericContextStore& numeric_context, std::vector<CompiledBlock>& out);

CompiledBlock compile_block(const nlohmann::json& block, NumericContextStore& numeric_context)
{
    CompiledBlock compiled;
    compiled.source = &block;
    const std::string kind = json_string(block, "kind");
    if (kind == "setVariable") {
        compiled.kind = CompiledKind::SetVariable;
        compiled.name = json_string(block, "name");
        if (!compiled.name.empty())
            compiled.name_slot = numeric_slot_for(numeric_context, compiled.name);
        if (block.contains("expression") && block["expression"].is_string())
            compiled.expression = compile_expr(block["expression"].get<std::string>(), numeric_context);
        else if (block.contains("value")) {
            compiled.has_value = true;
            compiled.value = block["value"];
        }
    } else if (kind == "getVariable") {
        compiled.kind = CompiledKind::GetVariable;
        compiled.name = json_string(block, "name");
        compiled.target = json_string(block, "target");
        if (!compiled.name.empty())
            compiled.name_slot = numeric_slot_for(numeric_context, compiled.name);
        if (!compiled.target.empty() && compiled.target != kPreviousVar)
            compiled.target_slot = numeric_slot_for(numeric_context, compiled.target);
    } else if (kind == "if") {
        compiled.kind = CompiledKind::If;
        compiled.condition = compile_expr(json_string(block, "condition"), numeric_context);
        compile_block_list(json_array_or_empty_local(block, "consequent"), numeric_context, compiled.consequent);
    } else if (kind == "while") {
        compiled.kind = CompiledKind::While;
        compiled.condition = compile_expr(json_string(block, "condition"), numeric_context);
        compiled.loop_limit = std::max(0, block.value("loopLimit", 1));
        compile_block_list(json_array_or_empty_local(block, "items"), numeric_context, compiled.items);
    } else if (kind == "log") {
        compiled.kind = CompiledKind::Log;
        compiled.level = json_string(block, "level");
        compiled.message = json_string(block, "message");
        compiled.expression = compile_expr(compiled.message, numeric_context);
    }
    return compiled;
}

void compile_block_list(const nlohmann::json& blocks, NumericContextStore& numeric_context, std::vector<CompiledBlock>& out)
{
    if (!blocks.is_array())
        return;
    out.reserve(blocks.size());
    for (const auto& block : blocks)
        out.push_back(compile_block(block, numeric_context));
}

void write_compiled_log_line(const std::string& level, const std::string& message)
{
    const std::string line = "[xblox] " + message;
    if (level == "error")
        logger::error(line);
    else if (level == "warn" || level == "warning")
        logger::warn(line);
    else if (level == "info")
        logger::info(line);
    else if (level == "debug")
        logger::debug(line);
    else
        logger::trace(line);
}

std::string normalize_command_stream_level(std::string level, const char* fallback)
{
    level = media::strutil::lower_ascii(std::move(level));
    if (level.empty())
        return fallback;
    if (level == "warning")
        return "warn";
    if (level == "err" || level == "critical")
        return "error";
    if (level == "off" || level == "none")
        return "off";
    if (level == "trace" || level == "debug" || level == "info" || level == "warn" || level == "error")
        return level;
    return fallback;
}

// `log` defaults on when stdout/stderr levels are explicitly set, matching shell.
bool command_wants_stream_log(const nlohmann::json& payload)
{
    if (payload.contains("log"))
        return json_bool_or(payload, "log", false);
    if (payload.contains("stdout") || payload.contains("stderr"))
        return true;
    if (payload.contains("runOptions") && payload["runOptions"].is_object()) {
        const auto& run = payload["runOptions"];
        if (run.contains("log"))
            return json_bool_or(run, "log", false);
        return run.contains("stdout") || run.contains("stderr");
    }
    return false;
}

std::string command_stream_level(const nlohmann::json& payload, const char* key, const char* fallback)
{
    std::string level = json_string(payload, key);
    if (level.empty() && payload.contains("runOptions") && payload["runOptions"].is_object())
        level = json_string(payload["runOptions"], key);
    return normalize_command_stream_level(std::move(level), fallback);
}

std::string command_log_owner_prefix(const std::string& block_id, const CommandInvocation& invocation)
{
    if (!block_id.empty())
        return block_id;
    if (!invocation.label.empty())
        return invocation.label;
    if (!invocation.id.empty())
        return invocation.id;
    return "command";
}

std::string_view trim_ascii_view(std::string_view view)
{
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t' || view.front() == '\r' || view.front() == '\n'))
        view.remove_prefix(1);
    while (!view.empty() && (view.back() == ' ' || view.back() == '\t' || view.back() == '\r' || view.back() == '\n'))
        view.remove_suffix(1);
    return view;
}

// Child xblox/shell already formats logger lines as `[HH:MM:SS] [info] rest`.
// Re-emit those at the embedded level so HandBrake-on-stderr is not forced to error.
bool split_embedded_log_line(const std::string& line, std::string& level, std::string& rest)
{
    std::string_view view = trim_ascii_view(line);
    if (view.size() >= 11 && view.front() == '[' && view[3] == ':' && view[6] == ':' && view[9] == ']')
        view.remove_prefix(11);
    view = trim_ascii_view(view);
    if (view.size() < 3 || view.front() != '[')
        return false;
    const auto close = view.find(']');
    if (close == std::string_view::npos || close < 2)
        return false;
    std::string tag(view.substr(1, close - 1));
    tag = media::strutil::lower_ascii(std::move(tag));
    if (tag == "warning")
        tag = "warn";
    if (tag != "trace" && tag != "debug" && tag != "info" && tag != "warn" && tag != "error")
        return false;
    view.remove_prefix(close + 1);
    view = trim_ascii_view(view);
    level = std::move(tag);
    rest.assign(view);
    return !rest.empty();
}

void log_command_stream_lines(const std::string& owner,
                              const std::string& stream,
                              const std::string& level,
                              const std::vector<std::string>& lines,
                              const std::function<void(const std::string&,
                                                       const std::string&,
                                                       const std::string&)>& event_sink = {})
{
    if (level == "off")
        return;
    for (const auto& line : lines) {
        if (trim_ascii_view(line).empty())
            continue;
        std::string embedded_level;
        std::string rest;
        const bool embedded = split_embedded_log_line(line, embedded_level, rest);
        const std::string& effective_level = embedded ? embedded_level : level;
        const std::string message = embedded
            ? owner + ": " + rest
            : owner + " " + stream + ": " + line;
        write_compiled_log_line(effective_level, message);
        if (event_sink)
            event_sink(effective_level, stream, message);
    }
}

// Format a numeric variable for interpolation: whole numbers print without a
// trailing ".000000", everything else uses default double formatting.
std::string format_fast_number(double d)
{
    if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e15)
        return std::to_string(static_cast<long long>(d));
    std::ostringstream os;
    os << d;
    return os.str();
}

// Lightweight ${var} interpolation for the compiled fast path. Resolves user/
// loop numerics from the numeric store and scalar context keys only  -  it does
// NOT build the heavy system variable map (dates/CWD/ENV/KNOWNFOLDER/...), so the
// hot loop stays allocation-light. Unknown placeholders are left verbatim.
// (The interpreted path uses the full bv::resolve via the block handler.)
std::string interpolate_fast_vars(const std::string& msg,
                                  const nlohmann::json& context,
                                  const NumericContextStore& numeric_context)
{
    std::string out;
    out.reserve(msg.size());
    size_t i = 0;
    while (i < msg.size()) {
        const size_t open = msg.find("${", i);
        if (open == std::string::npos) { out.append(msg, i, std::string::npos); break; }
        out.append(msg, i, open - i);
        const size_t close = msg.find('}', open + 2);
        if (close == std::string::npos) { out.append(msg, open, std::string::npos); break; }
        const std::string name = msg.substr(open + 2, close - (open + 2));
        bool resolved = false;
        if (const auto it = numeric_context.values.find(name); it != numeric_context.values.end()) {
            out += format_fast_number(it->second);
            resolved = true;
        } else if (context.is_object() && context.contains(name)) {
            const auto& v = context[name];
            if (v.is_string())       { out += v.get<std::string>(); resolved = true; }
            else if (v.is_number())  { out += format_fast_number(v.get<double>()); resolved = true; }
            else if (v.is_boolean()) { out += (v.get<bool>() ? "true" : "false"); resolved = true; }
        }
        if (!resolved)
            out.append(msg, open, close - open + 1); // leave ${unknown} verbatim
        i = close + 1;
    }
    return out;
}

void execute_compiled_blocks(const std::vector<CompiledBlock>& blocks,
                             nlohmann::json& context,
                             NumericContextStore& numeric_context,
                             const ExecutionOptions& options,
                             ExecutionResult& result,
                             FlowState& flow);

void execute_compiled_block(const CompiledBlock& block,
                            nlohmann::json& context,
                            NumericContextStore& numeric_context,
                            const ExecutionOptions& options,
                            ExecutionResult& result,
                            FlowState& flow)
{
    if (flow.break_requested)
        return;
    if (cancel_requested(options)) {
        if (!flow.cancel_emitted) {
            flow.cancel_emitted = true;
            // Scene/user stop — not a failed block. Keep exit_code 130 (SIGINT
            // convention) on the event without marking the run as errored.
            emit(result, options, ExecutionEvent{{}, "cancel", "cancelled", "cancel requested", 130});
        }
        return;
    }

    ++result.event_count; // running
    switch (block.kind) {
    case CompiledKind::SetVariable: {
        double evaluated = 0.0;
        if (!block.expression.empty_expr) {
            if (!eval_compiled_expr(block.expression, context, numeric_context, evaluated)) {
                emit(result, options, ExecutionEvent{{}, "setVariable", "error", "setVariable expression failed", 2});
                return;
            }
            numeric_context.slot_values[block.name_slot] = evaluated;
            numeric_context.values[block.name] = evaluated;
        } else if (block.has_value) {
            remember_numeric_value(numeric_context, block.name, block.value);
        }
        ++result.event_count; // ok
        return;
    }
    case CompiledKind::GetVariable: {
        double evaluated = 0.0;
        numeric_slot_value(numeric_context, block.name_slot, evaluated);
        const nlohmann::json value = evaluated;
        if (!block.target.empty() && block.target != kPreviousVar) {
            numeric_context.slot_values[block.target_slot] = evaluated;
            numeric_context.values[block.target] = evaluated;
        }
        numeric_context.values[kPreviousVar] = evaluated;
        emit(result, options, ExecutionEvent{{}, "getVariable", "ok", "variable read", 0,
            {{"name", block.name}, {"target", block.target}, {"value", value}}});
        return;
    }
    case CompiledKind::If: {
        double truthy = 0.0;
        const bool matched = eval_compiled_expr(block.condition, context, numeric_context, truthy)
                             && std::abs(truthy) > 1e-12;
        if (matched)
            execute_compiled_blocks(block.consequent, context, numeric_context, options, result, flow);
        if (flow.chain) {
            flow.chain->active = true;
            flow.chain->matched = matched;
        }
        ++result.event_count; // ok
        return;
    }
    case CompiledKind::While: {
        int iterations = 0;
        ++flow.loop_depth;
        for (; iterations < block.loop_limit && !flow.break_requested; ++iterations) {
            double truthy = 0.0;
            if (!eval_compiled_expr(block.condition, context, numeric_context, truthy) || std::abs(truthy) <= 1e-12)
                break;
            execute_compiled_blocks(block.items, context, numeric_context, options, result, flow);
            if ((iterations & 4095) == 0 && cancel_requested(options))
                break;
        }
        --flow.loop_depth;
        // `break` exits this loop only  -  execution continues after it. A Fatal
        // abort is tracked separately (abort_requested), so clearing here is safe.
        if (!flow.abort_requested)
            flow.break_requested = false;
        ++result.event_count; // ok
        return;
    }
    case CompiledKind::Log: {
        std::string message = block.message.empty() ? "(empty)" : block.message;
        if (block.message.find("${") != std::string::npos) {
            // Embedded placeholders -> interpolate live values (e.g. "tick n=${n}").
            message = interpolate_fast_vars(block.message, context, numeric_context);
        } else {
            // A bare variable/expression message ("n", "n+1") logs its value.
            double evaluated = 0.0;
            if (eval_compiled_expr(block.expression, context, numeric_context, evaluated)) {
                std::ostringstream os;
                os << evaluated;
                message = os.str();
            }
        }
        write_compiled_log_line(block.level.empty() ? "trace" : block.level, message);
        ++result.event_count; // ok
        return;
    }
    case CompiledKind::Unknown:
        if (block.source)
            execute_block(*block.source, {}, context, numeric_context, options, result, flow);
        return;
    }
}

// True when the block participates in (continues) a sibling if/elseIf/else
// chain  -  any other kind between chain members terminates the chain.
bool compiled_block_is_chain_member(const CompiledBlock& block)
{
    if (block.kind == CompiledKind::If)
        return true;
    if (block.kind != CompiledKind::Unknown || !block.source)
        return false;
    const std::string kind = json_string(*block.source, "kind");
    return kind == "elseIf" || kind == "else";
}

void execute_compiled_blocks(const std::vector<CompiledBlock>& blocks,
                             nlohmann::json& context,
                             NumericContextStore& numeric_context,
                             const ExecutionOptions& options,
                             ExecutionResult& result,
                             FlowState& flow)
{
    // Each list owns its sibling if/elseIf/else chain (docs/xblox/language.md D1).
    blocks::ChainState chain;
    blocks::ChainState* const parent_chain = flow.chain;
    flow.chain = &chain;
    for (const auto& block : blocks) {
        if (flow.break_requested)
            break;
        if (!compiled_block_is_chain_member(block))
            chain.active = false;
        const size_t errors_before = result.error_count;
        execute_compiled_block(block, context, numeric_context, options, result, flow);
        if (result.error_count > errors_before)
            break;
    }
    flow.chain = parent_chain;
}

bool compiled_blocks_supported(const std::vector<CompiledBlock>& blocks)
{
    for (const auto& block : blocks) {
        if (block.source && (!block_store_name(*block.source).empty() ||
                             block.source->contains("continueOnError") ||
                             block.source->contains("runFlags") ||
                             block.source->contains("onError")))
            return false;
        if (block.kind == CompiledKind::Unknown)
            return false;
        if (!compiled_blocks_supported(block.items) ||
            !compiled_blocks_supported(block.consequent))
            return false;
    }
    return true;
}

bool is_raw_constant_set_roots(const nlohmann::json& roots)
{
    if (!roots.is_array() || roots.size() != 1 || !roots[0].is_object())
        return false;
    const auto& root = roots[0];
    if (json_string(root, "kind") != "while" || json_string(root, "condition") != "1")
        return false;
    const auto& items = json_array_or_empty_local(root, "items");
    if (!items.is_array() || items.size() != 1 || !items[0].is_object())
        return false;
    const auto& child = items[0];
    return json_string(child, "kind") == "setVariable" && child.contains("value") && !child.contains("expression");
}

enum class DocumentHookPhase {
    Start,
    End,
};

std::string normalize_document_hook_event(std::string value)
{
    value = lower_compact(std::move(value));
    if (value == "start" || value == "onstart")
        return "start";
    if (value == "end" || value == "onend")
        return "end";
    return value;
}

bool is_document_hook_block(const nlohmann::json& block)
{
    return block.is_object() && json_string(block, "kind") == "onStart";
}

bool document_hook_matches(const nlohmann::json& block, DocumentHookPhase phase)
{
    std::string event = normalize_document_hook_event(json_string(block, "event"));
    if (event.empty())
        event = "start";
    return phase == DocumentHookPhase::Start ? event == "start" : event == "end";
}

nlohmann::json document_body_roots(const nlohmann::json& roots)
{
    if (!roots.is_array())
        return nlohmann::json::array();
    nlohmann::json body = nlohmann::json::array();
    for (const auto& block : roots) {
        if (!is_document_hook_block(block))
            body.push_back(block);
    }
    return body;
}

bool case_matches(const nlohmann::json& item, const nlohmann::json& context, double switch_number, bool has_switch_number, const std::string& switch_value, ScopeStack* scopes = nullptr)
{
    const std::string comparator = json_string(item, "comparator");
    const std::string expression = json_string(item, "expression");
    if (has_switch_number) {
        double rhs_number = 0.0;
        if (eval_mu_expression(expression, context, rhs_number, nullptr, nullptr, scopes))
            return compare_numbers(switch_number, comparator, rhs_number);
    }
    const std::string rhs = strip_quotes(expression);
    if (comparator == "!=" || comparator == "!==" || comparator == "<>")
        return switch_value != rhs;
    return switch_value == rhs;
}

std::string first_source_file(const nlohmann::json& payload, const nlohmann::json& command)
{
    if (payload.is_object() && payload.contains("file") && payload["file"].is_string())
        return payload["file"].get<std::string>();
    const nlohmann::json* source = nullptr;
    if (payload.is_object() && payload.contains("source") && payload["source"].is_object())
        source = &payload["source"];
    else if (command.is_object() && command.contains("source") && command["source"].is_object())
        source = &command["source"];
    if (source) {
        const auto files = json_string_array(*source, "files");
        if (!files.empty())
            return files.front();
    }
    return {};
}

bool extract_run_custom_command_payload(const std::string& method, nlohmann::json& payload, std::string& err)
{
    const std::string marker = "host.runCustomCommand";
    const size_t marker_pos = method.find(marker);
    if (marker_pos == std::string::npos)
        return false;
    const size_t open = method.find('(', marker_pos + marker.size());
    const size_t object_start = method.find('{', open == std::string::npos ? marker_pos : open);
    if (object_start == std::string::npos) {
        err = "host.runCustomCommand payload is missing an object";
        return false;
    }

    bool in_string = false;
    bool escaped = false;
    char quote = '\0';
    int depth = 0;
    for (size_t i = object_start; i < method.size(); ++i) {
        const char ch = method[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                in_string = false;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_string = true;
            quote = ch;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                try {
                    payload = nlohmann::json::parse(method.substr(object_start, i - object_start + 1));
                    return true;
                } catch (const std::exception& e) {
                    err = std::string("host.runCustomCommand payload parse failed: ") + e.what();
                    return false;
                }
            }
        }
    }
    err = "host.runCustomCommand payload is unterminated";
    return false;
}

#if defined(_WIN32)
using media::process::widen_utf8;
using media::process::quote_process_arg;

auto utf8_to_wide = media::process::widen_utf8;

void drain_pipe(HANDLE read_pipe, std::string& text)
{
    DWORD available = 0;
    while (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        char buffer[constants::kCommandPipeReadBufferBytes];
        DWORD read = 0;
        if (!::ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || read == 0)
            break;
        text.append(buffer, buffer + read);
    }
}

int run_process_command_line(std::wstring command_line,
                             const std::string& cwd,
                             const ExecutionOptions& options,
                             CommandRunOutput& output)
{
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!::CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !::CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        if (stdout_read) ::CloseHandle(stdout_read);
        if (stdout_write) ::CloseHandle(stdout_write);
        if (stderr_read) ::CloseHandle(stderr_read);
        if (stderr_write) ::CloseHandle(stderr_write);
        output.message = "failed to create output pipes";
        return 1;
    }
    ::SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const std::wstring wide_cwd = utf8_to_wide(cwd);
    const wchar_t* cwdp = wide_cwd.empty() ? nullptr : wide_cwd.c_str();
    if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                          CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP, nullptr, cwdp, &si, &pi)) {
        ::CloseHandle(stdout_read);
        ::CloseHandle(stdout_write);
        ::CloseHandle(stderr_read);
        ::CloseHandle(stderr_write);
        output.message = "process start failed";
        return 1;
    }
    ::CloseHandle(stdout_write);
    ::CloseHandle(stderr_write);
    ::CloseHandle(pi.hThread);

    std::string stdout_text;
    std::string stderr_text;
    const auto started = std::chrono::steady_clock::now();
    DWORD wait = WAIT_TIMEOUT;
    while ((wait = ::WaitForSingleObject(pi.hProcess, 50)) == WAIT_TIMEOUT) {
        drain_pipe(stdout_read, stdout_text);
        drain_pipe(stderr_read, stderr_text);
        if (options.cancel_requested && options.cancel_requested()) {
            output.cancelled = true;
            output.message = "command cancelled";
            ::TerminateProcess(pi.hProcess, 130);
            break;
        }
        if (options.default_timeout_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsed >= options.default_timeout_ms) {
                output.timed_out = true;
                output.message = "command timed out";
                ::TerminateProcess(pi.hProcess, 124);
                break;
            }
        }
    }
    drain_pipe(stdout_read, stdout_text);
    drain_pipe(stderr_read, stderr_text);
    DWORD code = 1;
    (void)::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(stdout_read);
    ::CloseHandle(stderr_read);
    ::CloseHandle(pi.hProcess);
    append_pipe_text(output.stdout_lines, stdout_text);
    append_pipe_text(output.stderr_lines, stderr_text);
    return static_cast<int>(code);
}
#else
using media::process::shell_quote_posix;

int run_shell_command(const std::string& command, CommandRunOutput& output)
{
    const fs::path base = fs::temp_directory_path() / ("pm-xblox-" + std::to_string(steady_now_ms()));
    fs::path stdout_path = base;
    stdout_path += media::path::path_from_utf8(".out");
    fs::path stderr_path = base;
    stderr_path += media::path::path_from_utf8(".err");
    const std::string redirected = command + " > " + shell_quote_posix(media::path::path_to_utf8(stdout_path)) + " 2> " + shell_quote_posix(media::path::path_to_utf8(stderr_path));
    const int raw = std::system(redirected.c_str());
    auto read_lines = [](const fs::path& path) {
        std::vector<std::string> lines;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return lines;
        std::ostringstream ss;
        ss << in.rdbuf();
        append_pipe_text(lines, ss.str());
        return lines;
    };
    output.stdout_lines = read_lines(stdout_path);
    output.stderr_lines = read_lines(stderr_path);
    std::error_code ec;
    fs::remove(stdout_path, ec);
    fs::remove(stderr_path, ec);
#if defined(__unix__) || defined(__APPLE__)
    if (raw == -1)
        return 1;
    if (WIFEXITED(raw))
        return WEXITSTATUS(raw);
    if (WIFSIGNALED(raw))
        return 128 + WTERMSIG(raw);
#endif
    return raw;
}
#endif

fs::path default_cli_executable(const ExecutionOptions& options)
{
    if (!options.cli_executable_path.empty())
        return options.cli_executable_path;
    fs::path exe = media::app::exe_executable();
    if (!exe.empty())
        return exe;
#if defined(_WIN32)
    return std::filesystem::path(pm::brand::k_exe_basename_cli_w);
#else
    return media::path::path_from_utf8(pm::brand::k_exe_basename_cli_u8).stem();
#endif
}

fs::path cli_executable_for_command(const std::string& cli, const ExecutionOptions& options)
{
    if (pm::cli::is_cli_ui_launch_command(cli)) {
        const fs::path gui = media::app::exe_gui_executable();
        if (!gui.empty())
            return gui;
    }
    return default_cli_executable(options);
}

std::string console_quote_arg(const std::string& arg)
{
#if defined(_WIN32)
    std::string out = "'";
    for (char ch : arg) {
        if (ch == '\'')
            out += "''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
#else
    return shell_quote_posix(arg);
#endif
}

std::string wrap_console_cwd(std::string command, const std::string& cwd)
{
    if (cwd.empty())
        return command;
#if defined(_WIN32)
    return "Push-Location -LiteralPath " + console_quote_arg(cwd) + "; try { " + command + " } finally { Pop-Location }";
#else
    return "cd " + shell_quote_posix(cwd) + " && " + command;
#endif
}

bool invocation_requests_console(const CommandInvocation& invocation)
{
    if (json_bool_or(invocation.payload, "background", false) ||
        json_bool_or(invocation.command, "background", false))
        return true;
    if (invocation.payload.contains("runOptions") && invocation.payload["runOptions"].is_object()) {
        const auto& run = invocation.payload["runOptions"];
        if (json_bool_or(run, "internalConsole", false) || json_bool_or(run, "runInConsole", false))
            return true;
    }
    if (invocation.command.contains("runOptions") && invocation.command["runOptions"].is_object()) {
        const auto& run = invocation.command["runOptions"];
        if (json_bool_or(run, "internalConsole", false) || json_bool_or(run, "runInConsole", false))
            return true;
    }
    return false;
}

bool console_line_for_invocation(const CommandInvocation& invocation,
                                 const ExecutionOptions& options,
                                 std::string& line,
                                 bool& close_on_exit,
                                 std::string& err)
{
    close_on_exit = false;
    if (invocation.command.contains("runOptions") && invocation.command["runOptions"].is_object())
        close_on_exit = json_bool_or(invocation.command["runOptions"], "closeOnExit", close_on_exit);
    if (invocation.payload.contains("runOptions") && invocation.payload["runOptions"].is_object())
        close_on_exit = json_bool_or(invocation.payload["runOptions"], "closeOnExit", close_on_exit);

    if (invocation.action == "cli") {
        const std::string cli = json_string(invocation.command, "cliCommand");
        if (cli.empty()) {
            err = "cliCommand is empty";
            return false;
        }
        std::vector<std::string> args = json_string_array(invocation.command, "globalArgs");
        const std::string configured_cwd = json_string(invocation.command, "cwd");
        if (!configured_cwd.empty() && configured_cwd != ".") {
            args.push_back("--cwd");
            args.push_back(configured_cwd);
        }
        const std::string log_level = json_string(invocation.command, "logLevel");
        if (!log_level.empty() && log_level != "info") {
            args.push_back("--log-level");
            args.push_back(log_level);
        }
        if (!pm::cli::is_cli_ui_launch_command(cli)) {
            const auto cli_parts = split_cli_command_path(cli);
            args.insert(args.end(), cli_parts.begin(), cli_parts.end());
        }
        const auto stored_args = json_string_array(invocation.command, "args");
        args.insert(args.end(), stored_args.begin(), stored_args.end());
        args.insert(args.end(), invocation.extra_args.begin(), invocation.extra_args.end());
#if defined(_WIN32)
        line = "& " + console_quote_arg(media::path::path_to_utf8(cli_executable_for_command(cli, options)));
#else
        line = console_quote_arg(media::path::path_to_utf8(cli_executable_for_command(cli, options)));
#endif
        for (const auto& arg : args)
            line += " " + console_quote_arg(arg);
        return true;
    }

    if (invocation.action == "external") {
        if (!invocation.command.contains("externalCommand") || !invocation.command["externalCommand"].is_object()) {
            err = "externalCommand is missing";
            return false;
        }
        const auto& ext = invocation.command["externalCommand"];
        std::string cwd = json_string(ext, "cwd");
        if (cwd.empty())
            cwd = json_string(invocation.command, "cwd");
        if (json_string(ext, "mode") == "shell") {
            line = json_string(ext, "shellLine");
            if (line.empty()) {
                err = "externalCommand.shellLine is empty";
                return false;
            }
            line = wrap_console_cwd(std::move(line), cwd);
            return true;
        }
        const std::string command = json_string(ext, "command");
        if (command.empty()) {
            err = "externalCommand.command is empty";
            return false;
        }
#if defined(_WIN32)
        line = "& " + console_quote_arg(command);
#else
        line = console_quote_arg(command);
#endif
        for (const auto& arg : json_string_array(ext, "args"))
            line += " " + console_quote_arg(arg);
        for (const auto& arg : invocation.extra_args)
            line += " " + console_quote_arg(arg);
        line = wrap_console_cwd(std::move(line), cwd);
        return true;
    }

    err = "terminal dispatch supports cli/external commands only";
    return false;
}

int spawn_external_command(const CommandInvocation& invocation, const ExecutionOptions& options, CommandRunOutput& output)
{
    if (!invocation.command.contains("externalCommand") || !invocation.command["externalCommand"].is_object()) {
        output.message = "externalCommand is missing";
        return 2;
    }
    const auto& ext = invocation.command["externalCommand"];
    std::string cwd = json_string(ext, "cwd");
    if (cwd.empty())
        cwd = json_string(invocation.command, "cwd");
    if (json_string(ext, "mode") == "shell") {
        const std::string line = json_string(ext, "shellLine");
        if (line.empty()) {
            output.message = "externalCommand.shellLine is empty";
            return 2;
        }
        if (!options.execute_external_commands) {
            output.message = "external shell command staged";
            return 0;
        }
#if defined(_WIN32)
        std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ";
        cmd += quote_process_arg(utf8_to_wide(line));
        return run_process_command_line(std::move(cmd), cwd, options, output);
#else
        std::string cmd;
        if (!cwd.empty())
            cmd += "cd " + shell_quote_posix(cwd) + " && ";
        cmd += "/bin/sh -lc " + shell_quote_posix(line);
        return run_shell_command(cmd, output);
#endif
    }

    const std::string command = json_string(ext, "command");
    if (command.empty()) {
        output.message = "externalCommand.command is empty";
        return 2;
    }
    auto args = json_string_array(ext, "args");
    args.insert(args.end(), invocation.extra_args.begin(), invocation.extra_args.end());
    if (!options.execute_external_commands) {
        output.message = "external argv command staged";
        return 0;
    }
#if defined(_WIN32)
    std::wstring cmd = quote_process_arg(utf8_to_wide(command));
    for (const auto& arg : args) {
        cmd.push_back(L' ');
        cmd += quote_process_arg(utf8_to_wide(arg));
    }
    return run_process_command_line(std::move(cmd), cwd, options, output);
#else
    std::string cmd;
    if (!cwd.empty())
        cmd += "cd " + shell_quote_posix(cwd) + " && ";
    cmd += shell_quote_posix(command);
    for (const auto& arg : args)
        cmd += " " + shell_quote_posix(arg);
    return run_shell_command(cmd, output);
#endif
}

int spawn_cli_command(const CommandInvocation& invocation, const ExecutionOptions& options, CommandRunOutput& output)
{
    const std::string cli = json_string(invocation.command, "cliCommand");
    if (cli.empty()) {
        output.message = "cliCommand is empty";
        return 2;
    }

    std::vector<std::string> args = json_string_array(invocation.command, "globalArgs");
    const std::string configured_cwd = json_string(invocation.command, "cwd");
    if (!configured_cwd.empty() && configured_cwd != ".") {
        args.push_back("--cwd");
        args.push_back(configured_cwd);
    }
    const std::string log_level = json_string(invocation.command, "logLevel");
    if (!log_level.empty() && log_level != "info") {
        args.push_back("--log-level");
        args.push_back(log_level);
    }
    if (!pm::cli::is_cli_ui_launch_command(cli)) {
        const auto cli_parts = split_cli_command_path(cli);
        args.insert(args.end(), cli_parts.begin(), cli_parts.end());
    }
    const auto stored_args = json_string_array(invocation.command, "args");
    args.insert(args.end(), stored_args.begin(), stored_args.end());
    args.insert(args.end(), invocation.extra_args.begin(), invocation.extra_args.end());

    const fs::path exe = cli_executable_for_command(cli, options);
    if (!options.execute_external_commands) {
        output.message = "cli command staged";
        return 0;
    }
#if defined(_WIN32)
    std::wstring cmd = quote_process_arg(exe.wstring());
    for (const auto& arg : args) {
        cmd.push_back(L' ');
        cmd += quote_process_arg(utf8_to_wide(arg));
    }
    return run_process_command_line(std::move(cmd), {}, options, output);
#else
    std::string cmd = shell_quote_posix(media::path::path_to_utf8(exe));
    for (const auto& arg : args)
        cmd += " " + shell_quote_posix(arg);
    return run_shell_command(cmd, output);
#endif
}

void emit(ExecutionResult& result, const ExecutionOptions& options, ExecutionEvent event)
{
    ScopedPerfTimer _t(g_xblox_perf.emit_ns);
    ++g_xblox_perf.emit_calls;

    if (event.type.empty())
        event.type = event.kind;
    if (event.status == "error" && event.error_code == 0)
        event.error_code = event.exit_code == 0 ? 1 : event.exit_code;
    ++result.event_count;
    if (event.status != "running")
        result.last_block_result = store_value_from_result(event.data);
    if (event.status == "error") {
        ++result.error_count;
        result.ok = false;
        if (result.exit_code == 0)
            result.exit_code = event.exit_code == 0 ? 1 : event.exit_code;
    }
    if (options.event_sink)
        options.event_sink(event);
    if (options.collect_events ||
        (options.collect_error_events && event.status == "error"))
        result.events.push_back(std::move(event));
}

bool cancel_requested(const ExecutionOptions& options)
{
    return thread_cancel_requested() || (options.cancel_requested && options.cancel_requested());
}

void execute_block_list(const nlohmann::json& blocks,
                        const std::string& path_prefix,
                        nlohmann::json& context,
                        NumericContextStore& numeric_context,
                        const ExecutionOptions& options,
                        ExecutionResult& result,
                        FlowState& flow);

bool emit_cancel_if_requested(const std::string& path,
                              const ExecutionOptions& options,
                              ExecutionResult& result,
                              FlowState& flow)
{
    if (!cancel_requested(options))
        return false;
    if (!flow.cancel_emitted) {
        flow.cancel_emitted = true;
        // Scene/user stop — not a failed block. Keep exit_code 130 (SIGINT
        // convention) on the event without marking the run as errored.
        emit(result, options, ExecutionEvent{path, "cancel", "cancelled", "cancel requested", 130});
    }
    return true;
}

RuntimeBridge& bridge_from(void* user_data)
{
    return *static_cast<RuntimeBridge*>(user_data);
}

void bridge_emit(void* user_data, ExecutionEvent event)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    if (event.block_id.empty())
        event.block_id = bridge.block_id;
    if (event.outputs.empty() && !bridge.block_id.empty()) {
        if (const OutputFrameValue* output =
                bridge.flow.output_frames.find(bridge.block_id, "result")) {
            event.outputs.push_back({
                "result", {}, output->kind, output->value, true,
            });
        }
    }
    emit(bridge.result, bridge.options, std::move(event));
}

void bridge_execute_block_list(void* user_data, const nlohmann::json& child_blocks, const std::string& child_path)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    execute_block_list(child_blocks, child_path, bridge.context, bridge.numeric_context, bridge.options, bridge.result, bridge.flow);
}

bool bridge_condition_truthy(void* user_data, const std::string& expression)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return condition_truthy(expression, bridge.context, &bridge.numeric_context, bridge.flow.scopes);
}

bool bridge_numeric_value(void* user_data, const std::string& expression, double& out)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return context_numeric_value(bridge.context, expression, out, &bridge.numeric_context, bridge.flow.scopes);
}

std::string bridge_string_value(void* user_data, const std::string& expression)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return context_string_value(bridge.context, expression);
}

bool bridge_case_matches(void* user_data,
                         const nlohmann::json& item,
                         double switch_number,
                         bool has_switch_number,
                         const std::string& switch_value)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return case_matches(item, bridge.context, switch_number, has_switch_number, switch_value, bridge.flow.scopes);
}

void bridge_set_context_value(void* user_data, const std::string& name, nlohmann::json value)
{
    ScopedPerfTimer _t(g_xblox_perf.set_ctx_ns);
    ++g_xblox_perf.set_ctx_calls;
    RuntimeBridge& bridge = bridge_from(user_data);
    // Numeric, simple-named user variables are tracked in the scope chain so that
    // loop bodies isolate freshly-declared locals while assignments to outer
    // variables update the parent frame. PREVIOUS is global (always hoists) and
    // is never frame-scoped. The JSON context is still mirrored so existing
    // reads/interpolation keep working; loop-local mirrors are erased on pop.
    if (name == "PREVIOUS" && !bridge.primary_output_published &&
        !bridge.block_id.empty()) {
        std::string kind = "json_value";
        if (value.is_boolean())             kind = "boolean";
        else if (value.is_number_integer() ||
                 value.is_number_unsigned()) kind = "integer";
        else if (value.is_number_float())   kind = "float";
        else if (value.is_string())         kind = "string";
        bridge.flow.output_frames.publish(
            bridge.block_id, "result", std::move(kind), value);
        bridge.primary_output_published = true;
    }
    double num = 0.0;
    if (bridge.flow.scopes && name != "PREVIOUS" && simple_context_name(name) && json_number(value, num))
        bridge.flow.scopes->set(name, num);
    remember_numeric_value(bridge.numeric_context, name, value);
    set_context_value(bridge.context, name, std::move(value));
}

void bridge_publish_output(void* user_data,
                           const std::string& block_id,
                           const std::string& slot,
                           const std::string& kind,
                           const nlohmann::json& value)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    bridge.flow.output_frames.publish(block_id, slot, kind, value);
    if (slot == "result" && block_id == bridge.block_id)
        bridge.primary_output_published = true;
}

const nlohmann::json* bridge_block_output_value(void* user_data,
                                                const std::string& block_id,
                                                const std::string& slot)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    const OutputFrameValue* value = bridge.flow.output_frames.find(block_id, slot);
    return value ? &value->value : nullptr;
}

const nlohmann::json* find_block_by_id(const nlohmann::json& value,
                                       const std::string& block_id,
                                       bool array_item_is_block = false)
{
    if (block_id.empty()) return nullptr;
    if (value.is_array()) {
        for (const auto& item : value) {
            if (const nlohmann::json* found =
                    find_block_by_id(item, block_id, true)) {
                return found;
            }
        }
        return nullptr;
    }
    if (!value.is_object()) return nullptr;

    const bool is_block =
        array_item_is_block && value.contains("kind") && value["kind"].is_string();
    if (is_block && value.value("blockId", std::string{}) == block_id)
        return &value;

    if (is_block) {
        const std::string kind = value.value("kind", std::string{});
        if (const blocks::BlockDescriptor* descriptor =
                blocks::builtin_block_descriptor(kind)) {
            for (const blocks::ChildListDef& child : descriptor->child_lists) {
                const auto child_it = value.find(child.key);
                if (child_it != value.end() && child_it->is_array()) {
                    if (const nlohmann::json* found =
                            find_block_by_id(*child_it, block_id, false)) {
                        return found;
                    }
                }
            }
        } else {
            for (const char* key : {"items", "consequent"}) {
                const auto child_it = value.find(key);
                if (child_it != value.end() && child_it->is_array()) {
                    if (const nlohmann::json* found =
                            find_block_by_id(*child_it, block_id, false)) {
                        return found;
                    }
                }
            }
        }
    }
    return nullptr;
}

bool bridge_block_enabled(void* user_data,
                          const std::string& block_id,
                          bool* enabled)
{
    if (!enabled) return false;
    RuntimeBridge& bridge = bridge_from(user_data);
    if (!bridge.flow.document_roots) return false;
    const nlohmann::json* block =
        find_block_by_id(*bridge.flow.document_roots, block_id);
    if (!block) return false;
    *enabled = block->value("enabled", true);
    return true;
}

void bridge_push_scope(void* user_data, ScopePolicy policy)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    if (bridge.flow.scopes)
        bridge.flow.scopes->push(static_cast<ScopeExit>(static_cast<int>(policy)));
}

void bridge_pop_scope(void* user_data)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    if (!bridge.flow.scopes)
        return;
    Scope* top = bridge.flow.scopes->current();
    if (!top || !top->parent())
        return; // never pop the root frame
    // Capture the frame's locals before the policy discards them, then strip the
    // JSON / numeric mirrors so isolated loop variables stop resolving.
    const std::vector<std::string> locals = top->localNames();
    const ScopeExit policy = top->policy();
    bridge.flow.scopes->pop();
    if (policy != ScopeExit::Isolate && policy != ScopeExit::Collect)
        return;
    for (const std::string& nm : locals) {
        if (nm == kPreviousVar)
            continue;
        bridge.numeric_context.values.erase(nm);
        bridge.numeric_context.slots.erase(nm);
        if (bridge.context.is_object())
            bridge.context.erase(nm);
    }
}

const nlohmann::json* bridge_context_value(void* user_data, const std::string& name)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return context_value_ptr(bridge.context, name);
}

bool bridge_eval_expression(void* user_data, const std::string& expression, double& out, std::string* err)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return eval_mu_expression(expression, bridge.context, out, err, &bridge.numeric_context, bridge.flow.scopes);
}

bool bridge_cancel_requested(void* user_data)
{
    RuntimeBridge& bridge = bridge_from(user_data);
    return cancel_requested(bridge.options);
}

int run_callback_or_stub(const CommandHandler& handler,
                         const CommandInvocation& invocation,
                         const char* stub_message,
                         std::string& message)
{
    if (handler)
        return handler(invocation, &message);
    message = stub_message;
    return 0;
}

CommandInvocation make_invocation(const std::string& path,
                                  nlohmann::json payload,
                                  const nlohmann::json& context,
                                  const ExecutionOptions& options,
                                  std::string& err)
{
    CommandInvocation invocation;
    invocation.path = path;
    invocation.payload = payload.is_object() ? std::move(payload) : nlohmann::json::object();
    invocation.id = json_string(invocation.payload, "id");
    invocation.label = json_string(invocation.payload, "label");

    if (is_action_command(invocation.payload)) {
        invocation.command = invocation.payload;
    } else if (!invocation.id.empty()) {
        if (!find_command_by_id(options.custom_commands, invocation.id, invocation.command))
            invocation.command = invocation.payload;
    } else {
        invocation.command = invocation.payload;
    }

    if (invocation.id.empty())
        invocation.id = json_string(invocation.command, "id");
    if (invocation.label.empty())
        invocation.label = json_string(invocation.command, "label");
    invocation.action = action_name(invocation.command);

    // Fold runtime/block extras into command.args before variable resolve so
    // ${var} tokens in block-level pd_args work the same as shell args.
    if (!options.extra_args.empty() && invocation.command.is_object()) {
        if (!invocation.command.contains("args") || !invocation.command["args"].is_array())
            invocation.command["args"] = nlohmann::json::array();
        for (const auto& arg : options.extra_args)
            invocation.command["args"].push_back(arg);
    }

    media::commands::VariableContext variable_context = options.command_context;
    if (variable_context.cwd.empty() && !options.default_cwd.empty())
        variable_context.cwd = media::path::path_to_utf8(options.default_cwd);
    const std::string source_file = first_source_file(invocation.payload, invocation.command);
    if (!source_file.empty())
        variable_context.source_file = source_file;
    std::error_code ec;
    if (variable_context.cwd.empty()) {
        const std::string cwd = media::path::path_to_utf8(fs::current_path(ec));
        if (!ec)
            variable_context.cwd = cwd;
    }

    // Fold xblox execution context (PREVIOUS, storeAs names, document vars) into
    // the command variable map — same overlay bv::resolve uses for block params.
    media::commands::VariableMap context_vars;
    if (context.is_object()) {
        for (auto it = context.begin(); it != context.end(); ++it) {
            const auto& val = it.value();
            if (val.is_string())
                context_vars.insert_or_assign(it.key(), val.get_ref<const std::string&>());
            else if (val.is_number_integer())
                context_vars.insert_or_assign(it.key(), std::to_string(val.get<long long>()));
            else if (val.is_number_unsigned())
                context_vars.insert_or_assign(it.key(), std::to_string(val.get<unsigned long long>()));
            else if (val.is_number_float())
                context_vars.insert_or_assign(it.key(), std::to_string(val.get<double>()));
            else if (val.is_boolean())
                context_vars.insert_or_assign(it.key(), val.get<bool>() ? "true" : "false");
            else if (val.is_object() || val.is_array())
                context_vars.insert_or_assign(it.key(), val.dump());
        }
    }
    media::commands::resolve_custom_command_item_variables(
        invocation.command, variable_context, context_vars, &err);
    return invocation;
}

void execute_command_payload(const std::string& path,
                             nlohmann::json payload,
                             const std::string& block_id,
                             nlohmann::json& context,
                             NumericContextStore& numeric_context,
                             const ExecutionOptions& options,
                             ExecutionResult& result)
{
    std::string warning;
    CommandInvocation invocation = make_invocation(path, std::move(payload), context, options, warning);
    if (!warning.empty()) {
        ExecutionEvent warning_event{path, "command", "warning", warning, 0, invocation.payload};
        warning_event.block_id = block_id;
        emit(result, options, std::move(warning_event));
    }

    int code = 0;
    CommandRunOutput output;
    logger::info("[xblox] command dispatch id=" + invocation.id + " action=" + invocation.action + " label=" + invocation.label);

    // Lifecycle bridge to the shared command hub. Key on the custom command id so
    // it matches the id other surfaces (ribbon/launcher/page buttons) use; fall
    // back to the invocation id. Only the process-backed branches below report.
    const std::string lifecycle_id = [&]() {
        const std::string cmd_id = json_string(invocation.command, "id");
        return cmd_id.empty() ? invocation.id : cmd_id;
    }();
    const bool emit_lifecycle = static_cast<bool>(options.command_lifecycle) && !lifecycle_id.empty();
    const auto lifecycle = [&](CommandLifecyclePhase phase) {
        if (emit_lifecycle)
            options.command_lifecycle(lifecycle_id, phase);
    };
    const bool log_streams = command_wants_stream_log(invocation.payload);
    const std::string stdout_level = command_stream_level(invocation.payload, "stdout", "info");
    const std::string stderr_level = command_stream_level(invocation.payload, "stderr", "error");
    // Console dispatch cannot capture pipes. Prefer in-process spawn when the
    // owner block asked to log stdout/stderr.
    if ((invocation.action == "cli" || invocation.action == "external") &&
        !log_streams &&
        options.console_command_handler && invocation_requests_console(invocation)) {
        std::string line;
        bool close_on_exit = false;
        std::string err;
        if (console_line_for_invocation(invocation, options, line, close_on_exit, err)) {
            code = options.console_command_handler(line, true, close_on_exit, &output.message);
            if (output.message.empty())
                output.message = code == 0 ? "sent to terminal" : "terminal dispatch failed";
        } else {
            code = 2;
            output.message = std::move(err);
        }
    } else if (invocation.action == "ribbon") {
        std::string message;
        code = run_callback_or_stub(options.ribbon_command_handler, invocation, "ribbon command stubbed", message);
        output.message = std::move(message);
    } else if (invocation.action == "app") {
        std::string message;
        code = run_callback_or_stub(options.app_command_handler, invocation, "app command stubbed", message);
        output.message = std::move(message);
    } else if (invocation.action == "cli") {
        lifecycle(CommandLifecyclePhase::Started);
        code = spawn_cli_command(invocation, options, output);
        lifecycle(code == 0 ? CommandLifecyclePhase::Exited : CommandLifecyclePhase::Failed);
    } else if (invocation.action == "external") {
        lifecycle(CommandLifecyclePhase::Started);
        code = spawn_external_command(invocation, options, output);
        lifecycle(code == 0 ? CommandLifecyclePhase::Exited : CommandLifecyclePhase::Failed);
    } else if (invocation.action == "url") {
        std::string message;
        code = run_callback_or_stub(options.open_url_handler, invocation, "url open stubbed", message);
        output.message = std::move(message);
    } else if (invocation.action == "path") {
        std::string message;
        code = run_callback_or_stub(options.open_path_handler, invocation, "path open stubbed", message);
        output.message = std::move(message);
    } else {
        output.message = "metadata command stubbed";
    }
    if (code == 0)
        logger::info("[xblox] command ok id=" + invocation.id + " action=" + invocation.action + " message=" + output.message);
    else
        logger::error("[xblox] command failed id=" + invocation.id + " action=" + invocation.action + " code=" + std::to_string(code) + " message=" + output.message);
    if (log_streams) {
        const std::string owner = command_log_owner_prefix(block_id, invocation);
        const auto emit_stream_event = [&](const std::string& level,
                                           const std::string& stream,
                                           const std::string& message) {
            ExecutionEvent event{
                path,
                "commandLog",
                "running",
                message,
                0,
                {{"level", level}, {"stream", stream}, {"owner", owner}},
            };
            event.block_id = block_id;
            event.type = "commandLog";
            emit(result, options, std::move(event));
        };
        log_command_stream_lines(owner, "stdout", stdout_level, output.stdout_lines, emit_stream_event);
        log_command_stream_lines(owner, "stderr", stderr_level, output.stderr_lines, emit_stream_event);
    }

    nlohmann::json data = {
        {"id", invocation.id},
        {"label", invocation.label},
        {"action", invocation.action},
        {"payload", invocation.payload},
        {"command", invocation.command},
        {"stdout", output.stdout_lines},
        {"stderr", output.stderr_lines},
        {"timedOut", output.timed_out},
        {"cancelled", output.cancelled},
    };
    const nlohmann::json stored = {
        {"ok", code == 0},
        {"blockId", block_id},
        {"type", "command"},
        {"result", output.message.empty() ? invocation.action : output.message},
        {"exitCode", code},
        {"errorCode", code == 0 ? 0 : code},
        {"stdout", output.stdout_lines},
        {"stderr", output.stderr_lines},
    };
    const std::string target = json_string(invocation.payload, "storeResult").empty()
        ? json_string(invocation.payload, "target")
        : json_string(invocation.payload, "storeResult");
    if (!target.empty()) {
        remember_numeric_value(numeric_context, target, stored);
        set_context_value(context, target, stored);
    }
    emit(result, options, ExecutionEvent{
        path,
        "command",
        code == 0 ? "ok" : "error",
        output.message.empty() ? invocation.action : output.message,
        code,
        std::move(data),
        block_id,
        "command",
        output.stdout_lines,
        output.stderr_lines,
        {},
        code == 0 ? 0 : code,
    });
}

nlohmann::json command_payload_from_block(const nlohmann::json& block)
{
    nlohmann::json payload = nlohmann::json::object();
    if (block.is_object() && block.contains("command") && block["command"].is_object())
        payload = block["command"];
    else if (block.is_object())
        payload = block;
    if (payload.is_object()) {
        // Saved-command picker wins for catalog lookup id.
        const std::string command_id = json_string(block, "commandId");
        if (!command_id.empty())
            payload["id"] = command_id;
        const std::string label = json_string(block, "label");
        if (!label.empty() && !payload.contains("label"))
            payload["label"] = label;
        std::string store_result = json_string(block, "storeResult");
        if (store_result.empty())
            store_result = json_string(block, "storeAs");
        if (!store_result.empty() && !payload.contains("storeResult"))
            payload["storeResult"] = store_result;
        const std::string target = json_string(block, "target");
        if (!target.empty() && !payload.contains("target"))
            payload["target"] = target;
        if (block.is_object() && block.contains("background") && block["background"].is_boolean() && !payload.contains("background"))
            payload["background"] = block["background"];
        if (block.is_object() && block.contains("runOptions") && block["runOptions"].is_object() && !payload.contains("runOptions"))
            payload["runOptions"] = block["runOptions"];
        if (block.is_object() && block.contains("log") && !payload.contains("log"))
            payload["log"] = block["log"];
        if (block.is_object() && block.contains("stdout") && !payload.contains("stdout"))
            payload["stdout"] = block["stdout"];
        if (block.is_object() && block.contains("stderr") && !payload.contains("stderr"))
            payload["stderr"] = block["stderr"];
    }
    return payload;
}

void execute_block_list(const nlohmann::json& blocks,
                        const std::string& path_prefix,
                        nlohmann::json& context,
                        NumericContextStore& numeric_context,
                        const ExecutionOptions& options,
                        ExecutionResult& result,
                        FlowState& flow);

void store_block_result_if_requested(const nlohmann::json& block,
                                     nlohmann::json& context,
                                     NumericContextStore& numeric_context,
                                     const ExecutionResult& result)
{
    const std::string name = block_store_name(block);
    if (name.empty())
        return;
    nlohmann::json value = result.last_block_result;
    remember_numeric_value(numeric_context, name, value);
    set_context_value(context, name, std::move(value));
}

void execute_background_block(nlohmann::json block,
                              std::string path,
                              nlohmann::json context,
                              ExecutionOptions options)
{
    std::thread([block = std::move(block), path = std::move(path), context = std::move(context), options = std::move(options)]() mutable {
        ExecutionResult background_result;
        FlowState background_flow;
        NumericContextStore background_numeric_context;
        seed_numeric_context(context, background_numeric_context);
        execute_block(block, path, context, background_numeric_context, options, background_result, background_flow);
    }).detach();
}

// Simulate-mode stand-in for a side-effecting block: emit an "ok (simulated)"
// event carrying the resolved inputs (+ any validation diagnostics), and feed a
// predicted result into PREVIOUS/storeAs so downstream blocks still see a value.
// The predicted result is the resolved path/target-like input (most side-
// effecting blocks output the path they would have produced); otherwise PREVIOUS
// is left unchanged (pass-through).
void emit_simulated_block(const std::string& path,
                          const std::string& kind,
                          const nlohmann::json& block,
                          const blocks::ResolvedInputs* inputs,
                          nlohmann::json& context,
                          NumericContextStore& numeric_context,
                          const ExecutionOptions& options,
                          ExecutionResult& result)
{
    ExecutionEvent ev;
    ev.path    = path;
    ev.kind    = kind;
    ev.status  = "ok";
    ev.message = "simulated: " + kind;
    ev.block_id = json_string(block, "blockId");
    ev.data["simulated"] = true;

    nlohmann::json predicted;
    bool have_predicted = false;
    if (inputs) {
        nlohmann::json in = nlohmann::json::object();
        for (const auto& kv : inputs->values)
            in[kv.first] = kv.second.to_json();
        ev.data["inputs"] = std::move(in);
        if (!inputs->diagnostics.empty())
            ev.data["diagnostics"] = inputs->diagnostics_json();

        for (const char* key : {"path", "output", "outputPath", "file", "dest", "url"}) {
            if (const blocks::Value* v = inputs->find(key); v && !v->is_null()) {
                predicted = v->to_json();
                have_predicted = true;
                break;
            }
        }
    }

    if (have_predicted) {
        ev.data["result"] = predicted;
        set_context_value(context, "PREVIOUS", predicted);
        remember_numeric_value(numeric_context, "PREVIOUS", predicted);
        const std::string store = block_store_name(block);
        if (!store.empty() && store != "PREVIOUS") {
            set_context_value(context, store, predicted);
            remember_numeric_value(numeric_context, store, predicted);
        }
    }

    emit(result, options, std::move(ev));
}

// Composition / iteration: after a leaf block's handler runs, any `items`
// children it carries run in an isolated child scope with PREVIOUS already set
// to the block's result. This makes plain nesting a pipeline  - 
// fs_read -> Parse(.field) -> fs_write  -  where each child consumes its parent's
// output via PREVIOUS, no named variables required. When the producer is flagged
// `iterable` and its result is a JSON array, the children run once per element
// (PREVIOUS = element, plus a numeric `index`); field projection is left to a
// child Parse block. Native containers (is_container) own their own child lists
// and are skipped here. Because this lives in the executor (not the handler) it
// composes with the run flags: flag a producer iterable + background and the
// whole map runs detached.
void run_child_blocks(const nlohmann::json& block,
                      const std::string& path,
                      const blocks::ExecFlags* ef,
                      blocks::BlockRuntime& runtime)
{
    if (ef && ef->is_container)
        return; // native containers drive their own children
    const nlohmann::json& items = json_array_or_empty_local(block, "items");
    if (!items.is_array() || items.empty())
        return;

    const std::string child_path = path.empty() ? std::string{} : path + "/items";

    const auto run_once = [&](nlohmann::json current, bool with_index, double index) {
        if (runtime.push_scope)
            runtime.push_scope(runtime.user_data, ScopePolicy::Isolate);
        runtime.set_context_value(runtime.user_data, "CURRENT", current);
        runtime.set_context_value(runtime.user_data, "PREVIOUS", std::move(current));
        if (with_index)
        {
            runtime.set_context_value(runtime.user_data, "CURRENT_INDEX", nlohmann::json(index));
            runtime.set_context_value(runtime.user_data, "index", nlohmann::json(index));
        }
        runtime.execute_block_list(runtime.user_data, items, child_path);
        if (runtime.pop_scope)
            runtime.pop_scope(runtime.user_data);
    };

    const nlohmann::json* produced = runtime.context_value
        ? runtime.context_value(runtime.user_data, "PREVIOUS")
        : nullptr;

    if (ef && ef->iterable && produced && produced->is_array()) {
        // Copy: PREVIOUS is overwritten on each pass below.
        const nlohmann::json elements = *produced;
        if (runtime.loop_depth) ++*runtime.loop_depth;
        for (size_t i = 0; i < elements.size() && !runtime.break_requested; ++i) {
            if (runtime.cancel_requested && runtime.cancel_requested(runtime.user_data))
                break;
            run_once(elements[i], true, static_cast<double>(i));
        }
        if (runtime.loop_depth) --*runtime.loop_depth;
        runtime.break_requested = false; // break exits this iteration loop only
    } else {
        run_once(produced ? *produced : nlohmann::json{}, false, 0.0);
    }
}

void clear_block_output_frames(const nlohmann::json& block, FlowState& flow)
{
    if (!block.is_object()) return;
    flow.output_frames.erase(json_string(block, "blockId"));

    const blocks::BlockDescriptor* descriptor =
        blocks::builtin_block_descriptor(json_string(block, "kind"));
    if (!descriptor) return;
    for (const blocks::ChildListDef& child : descriptor->child_lists) {
        const auto list_it = block.find(child.key);
        if (list_it == block.end() || !list_it->is_array()) continue;
        for (const auto& child_block : *list_it)
            clear_block_output_frames(child_block, flow);
    }
}

blocks::BlockResult execute_block(const nlohmann::json& block,
                   const std::string& path,
                   nlohmann::json& context,
                   NumericContextStore& numeric_context,
                   const ExecutionOptions& options,
                   ExecutionResult& result,
                   FlowState& flow)
{
    if (emit_cancel_if_requested(path, options, result, flow))
        return blocks::BlockResult::Fatal;
    if (!block.is_object()) {
        emit(result, options, ExecutionEvent{path, "unknown", "error", "block is not an object", 2});
        return blocks::BlockResult::Error;
    }
    const std::string kind = json_string(block, "kind");
    // A block's frame is execution-instance state, not a cache. Clearing the
    // block and its declared descendants prevents a skipped branch in a later
    // loop iteration from exposing values produced by an earlier iteration.
    clear_block_output_frames(block, flow);
    BlockProfileScope profile_scope(kind);
/*
    if (options.collect_events || options.event_sink)
        emit(result, options, ExecutionEvent{path, kind, "running", {}});
    else
        ++result.event_count;
*/
    if (kind == "command") {
        if (options.simulate) {
            emit_simulated_block(path, kind, block, nullptr, context, numeric_context, options, result);
            return blocks::BlockResult::Ok;
        }
        // Block-level args append like shell's pd_args — after the command's own args.
        ExecutionOptions cmd_options = options;
        for (const auto& arg : json_string_array(block, "args"))
            cmd_options.extra_args.push_back(arg);
        execute_command_payload(
            path,
            command_payload_from_block(block),
            json_string(block, "blockId"),
            context,
            numeric_context,
            cmd_options,
            result);
        // execute_command_payload stores the stable command envelope itself:
        // {ok, blockId, type, result, exitCode, errorCode, stdout, stderr}.
        // Running the generic storeAs path here overwrote that envelope with
        // the command event's diagnostic data.
        return blocks::BlockResult::Ok;
    }

    if (const blocks::BlockHandler builtin = blocks::builtin_block_handler(kind)) {
        RuntimeBridge bridge{context, numeric_context, options, result, flow};
        bridge.block_id = json_string(block, "blockId");
        blocks::BlockRuntime runtime{
            context,
            options,
            result,
            flow.break_requested,
            options.collect_events || options.event_sink,
            &bridge,
            bridge_emit,
            bridge_execute_block_list,
            bridge_condition_truthy,
            bridge_numeric_value,
            bridge_string_value,
            bridge_case_matches,
            bridge_set_context_value,
            bridge_context_value,
            bridge_eval_expression,
            bridge_cancel_requested,
            bridge_push_scope,
            bridge_pop_scope,
        };
        runtime.run_scope = flow.run_scope;
        runtime.chain = flow.chain;
        runtime.loop_depth = &flow.loop_depth;
        runtime.publish_output = bridge_publish_output;
        runtime.block_output_value = bridge_block_output_value;
        runtime.block_enabled = bridge_block_enabled;
        runtime.block_id = bridge.block_id;
        runtime.descriptor = blocks::builtin_block_descriptor(kind);
        // Resolve inputs up front: source (field -> PREVIOUS/variable -> default)
        // + transform/validate each declared param into a typed frame, so the
        // handler sees only finished values and never touches PREVIOUS itself.
        runtime.param_defs = runtime.descriptor
            ? &runtime.descriptor->param_defs
            : blocks::builtin_block_param_defs(kind);
        blocks::ResolvedInputs resolved_inputs;
        if (runtime.param_defs) {
            resolved_inputs = blocks::resolve_block_inputs(block, *runtime.param_defs, runtime);
            runtime.inputs  = &resolved_inputs;
        }

        // Surface input-validation diagnostics. severity<2 (clamp/enum/advisory)
        // are non-fatal warnings; a severity-2 finding is a violated precondition
        // (e.g. MustExist/Writable)  -  emit an error and skip the handler entirely,
        // in both normal and simulate mode, so the failure is predicted not run.
        if (!resolved_inputs.diagnostics.empty()) {
            const bool fatal = resolved_inputs.has_errors();
            ExecutionEvent dv;
            dv.path     = path;
            dv.kind     = kind;
            dv.block_id = runtime.block_id;
            dv.status    = fatal ? "error" : "warning";
            dv.message   = fatal ? "precondition failed" : "input validation";
            dv.exit_code = fatal ? 2 : 0;
            dv.data["diagnostics"] = resolved_inputs.diagnostics_json();
            if (options.collect_events || options.event_sink)
                emit(result, options, std::move(dv));
            else
                blocks::record_muted_event(result, dv, options.collect_error_events);
            if (fatal)
                return blocks::BlockResult::Error;
        }

        // Simulate mode: suppress side-effecting leaf blocks (containers and
        // pure dataflow/control blocks still run so flow + variables hold).
        const blocks::ExecFlags* ef = blocks::builtin_block_exec_flags(kind);
        const bool has_children = !json_array_or_empty_local(block, "items").empty();
        if (options.simulate && ef && ef->has_side_effects && !ef->is_container && !has_children) {
            emit_simulated_block(path, kind, block, &resolved_inputs, context, numeric_context, options, result);
            return blocks::BlockResult::Ok;
        }

        // The executor owns BlockIO: it bundles the raw block, path, runtime
        // sinks and the pre-resolved typed input frame. Handlers receive only
        // this facade (B2)  -  pure handlers read typed inputs; legacy handlers
        // are adapted transparently. Materialise a handler-local block only when
        // a final resolved value differs from its document field. This gives
        // legacy raw-field handlers the same pre-evaluated inputs without making
        // binding provenance part of block logic or copying every block.
        const nlohmann::json* handler_block = &block;
        nlohmann::json materialized_block;
        const auto bindings_it = block.find("bindings");
        const nlohmann::json* bindings =
            bindings_it != block.end() && bindings_it->is_object()
                ? &(*bindings_it)
                : nullptr;
        for (const auto& [name, value] : resolved_inputs.values) {
            nlohmann::json final_value = value.to_json();
            const auto field_it = block.find(name);
            const bool field_present = field_it != block.end();
            const bool explicitly_bound = bindings && bindings->contains(name);
            bool declared_source = false;
            if (runtime.param_defs) {
                for (const blocks::ParamDef& def : *runtime.param_defs) {
                    if (def.name == name) {
                        declared_source = def.uses_previous ||
                            def.source != blocks::ParamSource::Literal;
                        break;
                    }
                }
            }
            // Do not inject omitted literal defaults: legacy handlers may use
            // field presence to select a mode (log/stdout query is one example).
            // Bindings and declared sources are actual runtime inputs and must
            // be materialized even when the document field is absent.
            const bool needs_materialization =
                (field_present && *field_it != final_value) ||
                (!field_present && (explicitly_bound || declared_source));
            if (needs_materialization) {
                if (handler_block == &block) {
                    materialized_block = block;
                    handler_block = &materialized_block;
                }
                materialized_block[name] = std::move(final_value);
            }
        }
        blocks::BlockIO io{*handler_block, path, runtime};
        const blocks::BlockResult r = builtin(io);
        // The block ran regardless of outcome; finalise it. The return then
        // drives flow: Error marks failure (execute_block_list applies the
        // block's error policy); Fatal unwinds the whole run. A deliberate
        // `exit` block owns ok/exit_code  -  skip the generic failure marking.
        if (r != blocks::BlockResult::Ok && !result.exit_requested) {
            // User/scene stop on a cancellable block is not a failed run.
            if (!(r == blocks::BlockResult::Fatal && cancel_requested(options))) {
                result.ok = false;
                if (result.exit_code == 0) result.exit_code = 1;
            }
        }
        if (r == blocks::BlockResult::Fatal) {
            flow.abort_requested = true;
            flow.break_requested = true;
        }
        (void)emit_cancel_if_requested(path, options, result, flow);
        store_block_result_if_requested(block, context, numeric_context, result);
        run_child_blocks(block, path, ef, runtime);
        return r;
    }

    logger::warn("[xblox] unknown block kind skipped: " + (kind.empty() ? std::string("(empty)") : kind)
        + (path.empty() ? std::string{} : (" at " + path)));
    emit(result, options, ExecutionEvent{path, kind.empty() ? "unknown" : kind, "warning", "unknown block kind skipped"});
    store_block_result_if_requested(block, context, numeric_context, result);
    return blocks::BlockResult::Ok;
}

void execute_block_list(const nlohmann::json& blocks,
                        const std::string& path_prefix,
                        nlohmann::json& context,
                        NumericContextStore& numeric_context,
                        const ExecutionOptions& options,
                        ExecutionResult& result,
                        FlowState& flow)
{
    if (!blocks.is_array())
        return;
    static const std::string empty_path;
    const bool needs_paths = options.collect_events || options.event_sink;
    // Each list owns its sibling if/elseIf/else chain (docs/xblox/language.md
    // D1): `if` opens it, `elseIf`/`else` continue it, anything else ends it.
    // The parent list's chain is restored on exit so chains never cross levels.
    blocks::ChainState chain;
    blocks::ChainState* const parent_chain = flow.chain;
    flow.chain = &chain;
    for (size_t i = 0; i < blocks.size() && !flow.break_requested && !flow.abort_requested; ++i) {
        const size_t errors_before = result.error_count;
        const BlockRunSpec flags = block_run_flags(blocks[i]);
        if (!flags.enabled())
            continue; // disabled blocks are invisible to the chain
        const std::string kind = json_string(blocks[i], "kind");
        if (kind != "if" && kind != "elseIf" && kind != "else")
            chain.active = false;
        const std::string path = needs_paths
            ? (path_prefix.empty() ? std::to_string(i) : path_prefix + "/" + std::to_string(i))
            : empty_path;
        if (flags.background()) {
            execute_background_block(blocks[i], path, context, options);
            continue;
        }
        const blocks::BlockResult r = execute_block(blocks[i], needs_paths ? path : empty_path,
                                                    context, numeric_context, options, result, flow);
        // Fatal unwinds the whole run. Error (or a freshly emitted error event,
        // for handlers that still signal via events) stops this list when the
        // block's policy says abortOnError.
        if (r == blocks::BlockResult::Fatal || flow.abort_requested)
            break;
        if (flags.abort_on_error() && (r == blocks::BlockResult::Error || result.error_count > errors_before))
            break;
    }
    flow.chain = parent_chain;
}

void execute_document_hooks(const nlohmann::json& roots,
                            DocumentHookPhase phase,
                            nlohmann::json& context,
                            NumericContextStore& numeric_context,
                            const ExecutionOptions& options,
                            ExecutionResult& result,
                            FlowState& flow)
{
    if (!roots.is_array())
        return;
    static const std::string empty_path;
    const bool needs_paths = options.collect_events || options.event_sink;
    for (size_t i = 0; i < roots.size() && !flow.abort_requested; ++i) {
        const auto& block = roots[i];
        if (!is_document_hook_block(block) || !document_hook_matches(block, phase))
            continue;
        const std::string path = needs_paths ? std::to_string(i) : empty_path;
        const std::string event = phase == DocumentHookPhase::Start ? "On Start" : "On End";
        emit(result, options, ExecutionEvent{path, "onStart", "ok", event + " hook", 0, {{"event", event}}});
        execute_block_list(json_array_or_empty_local(block, "items"),
                           needs_paths ? path + "/items" : empty_path,
                           context, numeric_context, options, result, flow);
    }
}

void append_plan(const nlohmann::json& blocks, const std::string& path_prefix, std::vector<ExecutionEvent>& out)
{
    if (!blocks.is_array())
        return;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const std::string path = path_prefix.empty() ? std::to_string(i) : path_prefix + "/" + std::to_string(i);
        const auto& block = blocks[i];
        const std::string kind = json_string(block, "kind");
        out.push_back(ExecutionEvent{path, kind, "planned", {}});
        if (kind == "if" || kind == "case" || kind == "switchDefault") {
            append_plan(block.value("consequent", nlohmann::json::array()), path + "/consequent", out);
        } else if (kind == "for" || kind == "while" || kind == "group" || kind == "runScript" || kind == "onStart"
                   || kind == "elseIf" || kind == "else") {
            append_plan(block.value("items", nlohmann::json::array()), path + "/items", out);
        } else if (kind == "switch") {
            const auto items = block.value("items", nlohmann::json::array());
            if (items.is_array()) {
                for (size_t j = 0; j < items.size(); ++j)
                    append_plan(items[j].value("consequent", nlohmann::json::array()), path + "/items/" + std::to_string(j) + "/consequent", out);
            }
        }
    }
}

} // namespace

namespace {

bool json_tree_has_bindings(const nlohmann::json& value)
{
    if (value.is_object()) {
        const auto bindings = value.find("bindings");
        if (bindings != value.end() && bindings->is_object() && !bindings->empty())
            return true;
        for (auto it = value.begin(); it != value.end(); ++it)
            if (json_tree_has_bindings(it.value())) return true;
    } else if (value.is_array()) {
        for (const auto& item : value)
            if (json_tree_has_bindings(item)) return true;
    }
    return false;
}

void collect_v2_block_id_diagnostics(const nlohmann::json& value,
                                     const std::string& path,
                                     std::unordered_set<std::string>& ids,
                                     nlohmann::json& diagnostics,
                                     bool array_item_is_block = false)
{
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            collect_v2_block_id_diagnostics(
                value[i], path + "/" + std::to_string(i), ids, diagnostics, true);
        }
        return;
    }
    if (!value.is_object()) return;

    const bool is_block = array_item_is_block && value.contains("kind") && value["kind"].is_string();
    if (is_block) {
        const std::string id = value.value("blockId", std::string{});
        if (id.empty()) {
            diagnostics.push_back({
                {"code", "missing_block_id"},
                {"path", path},
                {"message", "document v2 requires a stable blockId on every block and switch item"},
            });
        } else if (!ids.insert(id).second) {
            diagnostics.push_back({
                {"code", "duplicate_block_id"},
                {"path", path},
                {"blockId", id},
                {"message", "document v2 block ids must be unique"},
            });
        }

        const std::string kind = value.value("kind", std::string{});
        if (const blocks::BlockDescriptor* descriptor =
                blocks::builtin_block_descriptor(kind)) {
            for (const blocks::ChildListDef& child : descriptor->child_lists) {
                const auto child_it = value.find(child.key);
                if (child_it != value.end() && child_it->is_array())
                    collect_v2_block_id_diagnostics(
                        *child_it, path + "/" + child.key, ids, diagnostics, false);
            }
        } else {
            // Unknown/extension blocks cannot provide native child metadata yet;
            // keep the two historical child-list keys valid for diagnostics.
            for (const char* key : {"items", "consequent"}) {
                const auto child_it = value.find(key);
                if (child_it != value.end() && child_it->is_array())
                    collect_v2_block_id_diagnostics(
                        *child_it, path + "/" + key, ids, diagnostics, false);
            }
        }
    }
}

// Sleep for `interval_ms` between loop passes, staying responsive to cancel.
// 0 == yield a single tick (never a busy spin) so the run stays cancellable and
// the host message pump / web UI keeps breathing. Larger intervals sleep in
// small slices so Stop is honored promptly even at the 1-day ceiling.
void loop_pass_sleep(long long interval_ms, const ExecutionOptions& options)
{
    if (interval_ms <= 0) {
        std::this_thread::yield();
        return;
    }
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(interval_ms);
    constexpr auto slice = std::chrono::milliseconds(50);
    while (clock::now() < deadline) {
        if (cancel_requested(options))
            return;
        const auto remaining = deadline - clock::now();
        std::this_thread::sleep_for(std::min<clock::duration>(remaining, slice));
    }
}

} // namespace

void PassEngine::prepare(const nlohmann::json& roots, const nlohmann::json& ctx, const ExecutionOptions& options)
{
    scope_session.stack().resetRoot();
    context = ctx.is_object() ? ctx : nlohmann::json::object();
    seed_default_scope_vars(context, options);
    seed_numeric_context(context, numeric_context);
    // Seed the root scope frame with context numerics (the expression runtime
    // binds muParser vars straight to the scope chain).
    if (context.is_object()) {
        for (auto it = context.begin(); it != context.end(); ++it) {
            double num = 0.0;
            if (it.key() != kPreviousVar && simple_context_name(it.key()) && json_number(it.value(), num))
                scope_session.stack().set(it.key(), num);
        }
    }
    // Compiled fast engine: when no events are observed and not simulating,
    // flatten supported docs into a scalar-slot program (compiled once and
    // reused across loop passes). Otherwise fall back to the interpreted path.
    const bool events_enabled = options.collect_events || options.event_sink;
    if (!events_enabled && !options.simulate && !is_raw_constant_set_roots(roots) &&
        !json_tree_has_bindings(roots)) {
        if (!compiled_checked) {
            compiled.clear();
            compile_block_list(roots, numeric_context, compiled);
            use_compiled = compiled_blocks_supported(compiled);
            if (!use_compiled)
                compiled.clear();
            compiled_checked = true;
        }
    } else {
        compiled.clear();
        use_compiled = false;
        compiled_checked = false;
    }
}

void PassEngine::run_pass(const nlohmann::json& roots, const ExecutionOptions& options, ExecutionResult& result)
{
    FlowState flow;
    flow.scopes = &scope_session.stack();
    flow.run_scope = &run_scope;
    flow.document_roots = &roots;
    if (use_compiled)
        execute_compiled_blocks(compiled, context, numeric_context, options, result, flow);
    else
        execute_block_list(roots, {}, context, numeric_context, options, result, flow);
}

void PassEngine::apply_set(const std::string& name, const nlohmann::json& value)
{
    if (name.empty() || name == kPreviousVar)
        return;
    set_context_value(context, name, value);
    remember_numeric_value(numeric_context, name, value);
    double num = 0.0;
    if (simple_context_name(name) && json_number(value, num))
        scope_session.stack().set(name, num);
}

nlohmann::json PassEngine::snapshot_vars() const
{
    nlohmann::json out = nlohmann::json::object();
    if (context.is_object())
        for (auto it = context.begin(); it != context.end(); ++it)
            if (it.key() != kPreviousVar)
                out[it.key()] = it.value();
    for (const auto& [name, value] : numeric_context.values)
        if (name != kPreviousVar)
            out[name] = value;
    for (const auto& [name, slot] : numeric_context.slots)
        if (name != kPreviousVar && slot < numeric_context.slot_values.size())
            out[name] = numeric_context.slot_values[slot];
    return out;
}

// Fold one pass's outcome into the loop aggregate. Counters accumulate; only the
// latest pass's events/result are retained so memory stays bounded on long
// loops (the host streams every event live via event_sink, so nothing is lost).
void accumulate_pass(ExecutionResult& aggregate, ExecutionResult&& pass)
{
    aggregate.ok = pass.ok;
    aggregate.exit_code = pass.exit_code;
    aggregate.exit_requested = aggregate.exit_requested || pass.exit_requested;
    aggregate.event_count += pass.event_count;
    aggregate.error_count += pass.error_count;
    aggregate.last_block_result = std::move(pass.last_block_result);
    aggregate.events = std::move(pass.events);
}

ExecutionResult run_blocks_file(const nlohmann::json& blocks_file, const ExecutionOptions& options)
{
    if (blocks_file.is_object() && blocks_file.value("version", 1) >= 2) {
        nlohmann::json diagnostics = nlohmann::json::array();
        std::unordered_set<std::string> ids;
        const nlohmann::json roots_to_validate =
            blocks_file.value("roots", nlohmann::json::array());
        collect_v2_block_id_diagnostics(roots_to_validate, "/roots", ids, diagnostics);
        if (!diagnostics.empty()) {
            ExecutionResult invalid;
            ExecutionEvent event;
            event.kind = "document";
            event.type = "document_validation";
            event.status = "error";
            event.message = "invalid XBlox v2 document";
            event.exit_code = 2;
            event.error_code = 2;
            event.data["diagnostics"] = std::move(diagnostics);
            emit(invalid, options, std::move(event));
            return invalid;
        }
    }

    nlohmann::json context = blocks_file.is_object() ? blocks_file.value("context", nlohmann::json::object()) : nlohmann::json::object();
    const nlohmann::json roots = blocks_file.is_object() ? blocks_file.value("roots", nlohmann::json::array()) : nlohmann::json::array();
    const nlohmann::json body_roots = document_body_roots(roots);

    // Resolve the document-level loop: CLI override > document `loop` setting >
    // default (off, 100ms, persist). The interval is clamped to [0, 1 day].
    bool loop_enabled = false;
    long long interval_ms = 100;
    bool loop_reset = false; // false == persist state across passes (default)
    if (blocks_file.is_object() && blocks_file.contains("loop") && blocks_file["loop"].is_object()) {
        const auto& loop = blocks_file["loop"];
        loop_enabled = loop.value("enabled", false);
        if (loop.contains("intervalMs") && loop["intervalMs"].is_number())
            interval_ms = loop["intervalMs"].get<long long>();
        loop_reset = loop.value("reset", false);
    }
    if (options.loop_override)
        loop_enabled = *options.loop_override;
    if (options.loop_interval_ms_override)
        interval_ms = *options.loop_interval_ms_override;
    if (options.loop_reset_override)
        loop_reset = *options.loop_reset_override;
    interval_ms = std::clamp<long long>(interval_ms, 0, ExecutionOptions::kLoopIntervalMaxMs);
    // Finite document-loop limit: 0 = unlimited (the default).
    int64_t loop_passes_remaining = options.loop_limit_override.value_or(0);

    ExecutionOptions cleanup_options = options;
    cleanup_options.cancel_requested = {};

    if (!loop_enabled) {
        PassEngine engine;
        engine.prepare(body_roots, context, options);
        ExecutionResult result;
        {
            FlowState flow;
            flow.scopes = &engine.scope_session.stack();
            flow.run_scope = &engine.run_scope;
            execute_document_hooks(roots, DocumentHookPhase::Start, engine.context, engine.numeric_context, options, result, flow);
        }
        engine.run_pass(body_roots, options, result);
        {
            FlowState flow;
            flow.scopes = &engine.scope_session.stack();
            flow.run_scope = &engine.run_scope;
            execute_document_hooks(roots, DocumentHookPhase::End, engine.context, engine.numeric_context, cleanup_options, result, flow);
        }
        return result;
    }

    const LoopScopePolicy policy = loop_reset ? LoopScopePolicy::Reset : LoopScopePolicy::Persist;
    ExecutionResult aggregate;

    if (policy == LoopScopePolicy::Persist) {
        // One scope session reused across passes: variables carry over (n climbs
        // 1,2,3...) and NH_Context is built once.
        PassEngine engine;
        engine.prepare(body_roots, context, options);
        {
            FlowState flow;
            flow.scopes = &engine.scope_session.stack();
            flow.run_scope = &engine.run_scope;
            execute_document_hooks(roots, DocumentHookPhase::Start, engine.context, engine.numeric_context, options, aggregate, flow);
        }
        while (!cancel_requested(options)) {
            // Ingest peer-issued variable sets before the pass (session bus).
            if (options.pull_external_vars) {
                for (const auto& [name, value] : options.pull_external_vars())
                    engine.apply_set(name, value);
            }
            ExecutionResult pass;
            engine.run_pass(body_roots, options, pass);
            accumulate_pass(aggregate, std::move(pass));
            // Publish the run's variables so peers can read live scope.
            if (options.publish_vars)
                options.publish_vars(engine.snapshot_vars());
            // Runtime-level pass counter (before the yield, so the limit is
            // precise regardless of interval_ms).
            if (loop_passes_remaining > 0 && --loop_passes_remaining == 0)
                break;
            if (aggregate.exit_requested || cancel_requested(options))
                break;
            loop_pass_sleep(interval_ms, options);
        }
        {
            FlowState flow;
            flow.scopes = &engine.scope_session.stack();
            flow.run_scope = &engine.run_scope;
            execute_document_hooks(roots, DocumentHookPhase::End, engine.context, engine.numeric_context, cleanup_options, aggregate, flow);
        }
    } else {
        // Reset: each pass is a clean restart (fresh scope, re-seeded from the
        // document context)  -  rebuilds NH_Context per pass by design.
        nlohmann::json loop_context = context;
        nlohmann::json end_context = context;
        {
            PassEngine start_engine;
            start_engine.prepare(body_roots, loop_context, options);
            FlowState flow;
            flow.scopes = &start_engine.scope_session.stack();
            flow.run_scope = &start_engine.run_scope;
            execute_document_hooks(roots, DocumentHookPhase::Start, start_engine.context, start_engine.numeric_context, options, aggregate, flow);
            loop_context = start_engine.context;
            end_context = start_engine.context;
        }
        while (!cancel_requested(options)) {
            PassEngine engine;
            engine.prepare(body_roots, loop_context, options);
            ExecutionResult pass;
            engine.run_pass(body_roots, options, pass);
            end_context = engine.context;
            accumulate_pass(aggregate, std::move(pass));
            if (loop_passes_remaining > 0 && --loop_passes_remaining == 0)
                break;
            if (aggregate.exit_requested || cancel_requested(options))
                break;
            loop_pass_sleep(interval_ms, options);
        }
        PassEngine end_engine;
        end_engine.prepare(body_roots, end_context, cleanup_options);
        FlowState flow;
        flow.scopes = &end_engine.scope_session.stack();
        flow.run_scope = &end_engine.run_scope;
        execute_document_hooks(roots, DocumentHookPhase::End, end_engine.context, end_engine.numeric_context, cleanup_options, aggregate, flow);
    }
    return aggregate;
}

ExecutionResult run_block_roots(const nlohmann::json& roots, const nlohmann::json& context, const ExecutionOptions& options)
{
    // S3: a per-run scope stack (NH_Context-backed) tracks frame membership of
    // numeric user variables so container bodies can isolate their locals, and is
    // the single source the expression runtime binds muParser variables to. The
    // PassEngine owns that session, seeds the context, and picks the compiled vs
    // interpreted path; a single run is one prepare()+run_pass().
    PassEngine engine;
    engine.prepare(roots, context, options);
    ExecutionResult result;
    engine.run_pass(roots, options, result);
    return result;
}

std::vector<ExecutionEvent> build_execution_plan(const nlohmann::json& blocks_file)
{
    std::vector<ExecutionEvent> out;
    const nlohmann::json roots = blocks_file.is_object() ? blocks_file.value("roots", nlohmann::json::array()) : blocks_file;
    append_plan(roots, {}, out);
    return out;
}

} // namespace media::xblox
