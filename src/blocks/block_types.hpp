#pragma once
// block_types.hpp  -  core type vocabulary + handler-facing IO facade.
//
// This is the foundation every block handler sees: the BlockRuntime sink table,
// the typed Value currency, Diagnostic/ResolvedInputs data, the EventBuilder and
// BlockIO facades, and the BlockHandler closure surface (+ legacy adapter). It is
// deliberately free of ParamDef/registry concerns (those layer on top); ParamDef
// is only forward-referenced here via pointers.
//
// Split out of the former monolithic builtin_blocks.hpp (see that header  -  now a
// thin umbrella  -  and docs/xblox-next.md §10.49).

#include "runtime.hpp"
#include "commands/value_kind.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace media::xblox::blocks {

struct ParamDef;       // defined in block_params.hpp; BlockRuntime carries a pointer to the block's defs
struct BlockDescriptor; // defined in block_registry.hpp
struct ResolvedInputs; // typed, pre-resolved input frame (defined below)

// ─────────────────────────────────────────────────────────────────────────────
// ChainState  -  sibling if/elseIf/else chain (docs/xblox/language.md D1)
//
// Owned per block list by the list runner; handlers read/write it through
// BlockRuntime::chain. A chain opens at an `if`, continues through contiguous
// `elseIf` siblings, and is consumed by `else` (or terminated by any other
// kind  -  the list runner resets `active`).
// ─────────────────────────────────────────────────────────────────────────────
struct ChainState {
    bool active = false;   // an if/elseIf chain is open at this list position
    bool matched = false;  // a branch in the open chain already ran
};

// ─────────────────────────────────────────────────────────────────────────────
// BlockRuntime
// ─────────────────────────────────────────────────────────────────────────────

struct BlockRuntime {
    using EmitFn = void (*)(void*, ExecutionEvent);
    using ExecuteBlockListFn = void (*)(void*, const nlohmann::json&, const std::string&);
    using ConditionTruthyFn = bool (*)(void*, const std::string&);
    using NumericValueFn = bool (*)(void*, const std::string&, double&);
    using StringValueFn = std::string (*)(void*, const std::string&);
    using CaseMatchesFn = bool (*)(void*, const nlohmann::json&, double, bool, const std::string&);
    using SetContextValueFn = void (*)(void*, const std::string&, nlohmann::json);
    using ContextValueFn = const nlohmann::json* (*)(void*, const std::string&);
    using EvalExpressionFn = bool (*)(void*, const std::string&, double&, std::string*);
    using CancelRequestedFn = bool (*)(void*);
    using PushScopeFn = void (*)(void*, ScopePolicy);
    using PopScopeFn = void (*)(void*);
    using PublishOutputFn = void (*)(void*, const std::string&, const std::string&,
                                     const std::string&, const nlohmann::json&);
    using BlockOutputValueFn = const nlohmann::json* (*)(void*, const std::string&,
                                                         const std::string&);
    /// Look up whether `block_id` exists in the active document and whether it
    /// is enabled. Returns false when the id is unknown / lookup unavailable.
    /// When true, writes *enabled (default true when the field is absent).
    using BlockEnabledFn = bool (*)(void*, const std::string& block_id, bool* enabled);

    nlohmann::json& context;
    const ExecutionOptions& options;
    ExecutionResult& result;
    bool& break_requested;
    bool events_enabled = true;
    void* user_data = nullptr;

    EmitFn emit = nullptr;
    ExecuteBlockListFn execute_block_list = nullptr;
    ConditionTruthyFn condition_truthy = nullptr;
    NumericValueFn numeric_value = nullptr;
    StringValueFn string_value = nullptr;
    CaseMatchesFn case_matches = nullptr;
    SetContextValueFn set_context_value = nullptr;
    ContextValueFn context_value = nullptr;
    EvalExpressionFn eval_expression = nullptr;
    CancelRequestedFn cancel_requested = nullptr;
    // Scope frames (S3): containers push a child frame around their body and pop
    // it on exit, applying the policy (loops use Isolate). Optional  -  handlers
    // must null-check before calling.
    PushScopeFn push_scope = nullptr;
    PopScopeFn pop_scope = nullptr;
    // Document-v2 output-frame bridge. IDs/slots are validated and compiled by
    // the executor; handlers only publish declared slots through BlockIO.
    PublishOutputFn publish_output = nullptr;
    BlockOutputValueFn block_output_value = nullptr;
    BlockEnabledFn block_enabled = nullptr;
    std::string block_id;
    const BlockDescriptor* descriptor = nullptr;

    // Declared parameter schema for the block being executed (from its
    // descriptor). Lets BlockIO source/default/validate inputs up front, so
    // handlers never resolve PREVIOUS / variables / defaults themselves.
    const std::vector<ParamDef>* param_defs = nullptr;

    // Typed, pre-resolved inputs for this block: the executor runs each
    // ParamDef through source -> default -> transform/validate once, before the
    // handler. BlockIO reads from here; handlers see only finished typed values.
    const ResolvedInputs* inputs = nullptr;

    // Run-scoped resource cache: persists across loop passes so a handler can
    // cache an expensive instance (ONNX session, overlay window) keyed by
    // identity and reuse it. Null in detached/background contexts.
    RunScope* run_scope = nullptr;

    // Sibling if/elseIf/else chain of the list currently being executed
    // (owned by the list runner). Null in contexts without chain tracking.
    ChainState* chain = nullptr;

    // Depth of enclosing for/while/iterable loops  -  lets `break` warn instead
    // of silently killing the pass when used outside a loop. Null-safe.
    int* loop_depth = nullptr;
};

class BlockIO; // the typed facade handlers receive; defined below

// ─────────────────────────────────────────────────────────────────────────────
// BlockResult  -  a handler's outcome, which drives executor flow control.
//
//   Ok      -  proceed to the next block.
//   Error   -  the block failed; the executor applies the block's error policy
//            (`continueOnError`/`onError`/`abortOnError` -> BlockRunSpec). The
//            human-readable cause still rides on an emitted error *event*; this
//            return is purely the flow-control decision.
//   Fatal   -  abort the whole run immediately (like cancellation), regardless of
//            error policy.
//
// Named BlockResult (not E_OK/E_FAIL) on purpose: Win32 pulls in HRESULT macros
// (E_FAIL, E_NOTIMPL, ...) that would collide. A bool-returning handler still
// works during migration  -  the adapters below map true/false -> Ok.
// ─────────────────────────────────────────────────────────────────────────────
enum class BlockResult { Ok, Error, Fatal };

// Block handler surface (B2): a closure over the typed BlockIO facade. The
// executor builds BlockIO (raw block + pre-resolved typed input frame + runtime
// sinks) and invokes this  -  handlers never see raw nlohmann::json or resolve
// PREVIOUS/variables themselves. std::function so a handler can be a free
// function, a lambda, or a stateful closure (e.g. a DLL plugin).
using BlockHandler = std::function<BlockResult(BlockIO&)>;

// Legacy raw-JSON handler signature, kept while blocks migrate file-by-file to
// pure BlockIO bodies. `HandlerArg`/`legacy()` adapt these into a BlockHandler.
using LegacyBlockHandler = bool (*)(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime);

// Diagnostic logging sink (developer logs, NOT the event stream). Defined in
// builtin_blocks.cpp so this header stays free of the logger dependency; routed
// to logger::trace/debug/info/warn/error by level. `io.trace()/debug()/...` call it.
void block_log(const char* level, const std::string& message);

// ─────────────────────────────────────────────────────────────────────────────
// ParamKind  -  compatibility alias of media::commands::ValueKind
// ─────────────────────────────────────────────────────────────────────────────

using ParamKind = media::commands::ValueKind;
using media::commands::param_kind_str;
using media::commands::value_kind_str;

// ─────────────────────────────────────────────────────────────────────────────
// Value  -  the typed currency that flows in/out of block handlers (S4/B1)
//
// Block logic deals in `Value`, never `nlohmann::json`. Scalars/strings live
// inline; genuinely structured data (jq results, directory listings, parsed
// JSON) is carried in the opaque `shared_ptr<const json>` arm  -  most handlers
// just pass it through, only json-processing blocks open it via `as_json()`.
// `to_json()` is the single serialisation step, used by the executor/event sink.
// ─────────────────────────────────────────────────────────────────────────────

class Value {
public:
    Value() = default;
    Value(bool b)               : v_(b) {}
    Value(int i)                : v_(static_cast<std::int64_t>(i)) {}
    Value(long long i)          : v_(static_cast<std::int64_t>(i)) {}
    Value(unsigned long long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(double d)             : v_(d) {}
    Value(const char* s)        : v_(std::string(s ? s : "")) {}
    Value(std::string s)        : v_(std::move(s)) {}

    // Structured/opaque blob (the only place a handler touches nlohmann::json).
    static Value from_json(nlohmann::json j) {
        Value v;
        v.v_ = std::make_shared<const nlohmann::json>(std::move(j));
        return v;
    }

    bool is_null() const { return v_.index() == 0; }
    bool is_array() const {
        if (v_.index() != 5) return false;
        const auto& p = std::get<JsonPtr>(v_);
        return p && p->is_array();
    }

    bool as_bool(bool d = false) const {
        switch (v_.index()) {
        case 1: return std::get<bool>(v_);
        case 2: return std::get<std::int64_t>(v_) != 0;
        case 3: return std::get<double>(v_) != 0.0;
        case 4: { const auto& s = std::get<std::string>(v_); return s == "true" || s == "1" || s == "yes" || s == "on"; }
        case 5: { const auto& p = std::get<JsonPtr>(v_); return p && p->is_boolean() ? p->get<bool>() : (p && !p->is_null()); }
        default: return d;
        }
    }
    std::int64_t as_int(std::int64_t d = 0) const {
        switch (v_.index()) {
        case 1: return std::get<bool>(v_) ? 1 : 0;
        case 2: return std::get<std::int64_t>(v_);
        case 3: return static_cast<std::int64_t>(std::get<double>(v_));
        case 4: { try { return std::stoll(std::get<std::string>(v_)); } catch (...) { return d; } }
        case 5: { const auto& p = std::get<JsonPtr>(v_); return p && p->is_number() ? p->get<std::int64_t>() : d; }
        default: return d;
        }
    }
    double as_double(double d = 0.0) const {
        switch (v_.index()) {
        case 1: return std::get<bool>(v_) ? 1.0 : 0.0;
        case 2: return static_cast<double>(std::get<std::int64_t>(v_));
        case 3: return std::get<double>(v_);
        case 4: { try { return std::stod(std::get<std::string>(v_)); } catch (...) { return d; } }
        case 5: { const auto& p = std::get<JsonPtr>(v_); return p && p->is_number() ? p->get<double>() : d; }
        default: return d;
        }
    }
    std::string as_string(std::string d = {}) const {
        switch (v_.index()) {
        case 1: return std::get<bool>(v_) ? "true" : "false";
        case 2: return std::to_string(std::get<std::int64_t>(v_));
        case 3: {
            std::ostringstream os;
            os << std::get<double>(v_);
            return os.str();
        }
        case 4: return std::get<std::string>(v_);
        case 5: {
            const auto& p = std::get<JsonPtr>(v_);
            if (!p) return d;
            if (p->is_string()) return p->get<std::string>();
            if (p->is_number_float()) {
                std::ostringstream os;
                os << p->get<double>();
                return os.str();
            }
            if (p->is_number_integer())  return std::to_string(p->get<long long>());
            if (p->is_number_unsigned()) return std::to_string(p->get<unsigned long long>());
            if (p->is_boolean())         return p->get<bool>() ? "true" : "false";
            return p->dump();
        }
        default: return d;
        }
    }

    nlohmann::json to_json() const {
        switch (v_.index()) {
        case 1: return std::get<bool>(v_);
        case 2: return std::get<std::int64_t>(v_);
        case 3: return std::get<double>(v_);
        case 4: return std::get<std::string>(v_);
        case 5: { const auto& p = std::get<JsonPtr>(v_); return p ? *p : nlohmann::json(nullptr); }
        default: return nlohmann::json(nullptr);
        }
    }

private:
    using JsonPtr = std::shared_ptr<const nlohmann::json>;
    std::variant<std::monostate, bool, std::int64_t, double, std::string, JsonPtr> v_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic  -  a single validation/precondition finding for one param.
//
// severity: 0 info · 1 warning (best-effort: clamp/enum coercion) · 2 error
// (a violated precondition  -  the executor skips the handler). Carrying the
// param name + severity (vs. a bare string) lets the UI group issues per field
// and lets strict/fatal handling key off severity instead of string-matching.
// ─────────────────────────────────────────────────────────────────────────────

struct Diagnostic {
    std::string param;
    std::string message;
    int         severity = 1;
    std::string code;

    nlohmann::json to_json() const {
        nlohmann::json j{{"param", param}, {"message", message}, {"severity", severity}};
        if (!code.empty()) j["code"] = code;
        return j;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ResolvedInputs  -  the typed input frame the executor builds before a handler.
//
// One entry per declared ParamDef, in declaration order. The value has already
// been sourced (field -> PREVIOUS/variable -> default), interpolated (${var}) and
// run through the kind-specific transform (cast + enum/range validation) plus
// declared preconditions (see ParamConstraint). `diagnostics` records every
// finding; severity 2 entries make the executor skip the handler. Small enough
// (a handful of params) that a flat vector beats a hash map.
//
// Defined here (not in block_resolver.hpp) because BlockIO::frame() reads it
// inline and must see the complete type.
// ─────────────────────────────────────────────────────────────────────────────

struct ResolvedInputs {
    std::vector<std::pair<std::string, Value>> values;
    std::vector<Diagnostic>                    diagnostics;

    const Value* find(const std::string& name) const {
        for (const auto& kv : values)
            if (kv.first == name) return &kv.second;
        return nullptr;
    }
    bool has_errors() const {
        for (const auto& d : diagnostics) if (d.severity >= 2) return true;
        return false;
    }
    nlohmann::json diagnostics_json() const {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& d : diagnostics) a.push_back(d.to_json());
        return a;
    }
};

// A stable-slot, kind-tagged output produced by a block. The slot identifies the
// value for typed bindings; export_name is an optional legacy/named scope sink.
using BlockOutput = ExecutionOutput;

// Aggregate bookkeeping for a *muted* event (events_enabled == false). The run's
// state must advance exactly as if it were reported; callers may retain errors
// so diagnostics remain available without storing successful hot-loop events.
// Mirrors the reported-path sink in xblox_commands.cpp emit().
inline void record_muted_event(ExecutionResult& result, const ExecutionEvent& event,
                               bool collect_error_events = false)
{
    ++result.event_count;
    if (event.status != "running") {
        result.last_block_result =
            (event.data.is_object() && event.data.contains("result")) ? event.data["result"] : event.data;
    }
    if (event.status == "error") {
        ++result.error_count;
        result.ok = false;
        if (result.exit_code == 0)
            result.exit_code = event.exit_code == 0 ? 1 : event.exit_code;
        if (collect_error_events)
            result.events.push_back(event);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EventBuilder  -  typed, fluent replacement for `runtime.emit(ExecutionEvent{...})`
//
// Handlers describe an event with typed `Value` fields; the builder serialises
// to the report `ExecutionEvent` and pushes it through the runtime sink on
// `.emit()` (or end-of-statement). Two distinct write families:
//   • field(key, value)           -  event metadata only.
//   • output(slot, kind, value)   -  a typed, bindable output slot.
//   • result(v[, kind])           -  the implicit *primary* output: PREVIOUS (+ the
//                                  block's storeAs/target sink) + "result" field.
// ─────────────────────────────────────────────────────────────────────────────

class EventBuilder {
public:
    EventBuilder(BlockRuntime& rt, std::string store, std::vector<BlockOutput>* outputs, ExecutionEvent ev)
        : rt_(&rt), store_(std::move(store)), outputs_(outputs), ev_(std::move(ev)) {}

    EventBuilder(EventBuilder&& o) noexcept
        : rt_(o.rt_), store_(std::move(o.store_)), outputs_(o.outputs_), ev_(std::move(o.ev_)), emitted_(o.emitted_) {
        o.emitted_ = true; o.rt_ = nullptr;
    }
    EventBuilder(const EventBuilder&) = delete;
    EventBuilder& operator=(const EventBuilder&) = delete;
    EventBuilder& operator=(EventBuilder&&) = delete;
    ~EventBuilder() { emit(); }

    // Event metadata only: attaches a field to the event, not a context variable.
    EventBuilder& field(const std::string& key, const Value& v) {
        ev_.data[key] = v.to_json();
        return *this;
    }
    // Transitional source compatibility while registrations/handlers migrate.
    EventBuilder& set(const std::string& key, const Value& v) { return field(key, v); }

    // Typed named output: event field + context variable + kind-tagged outputs[]
    // entry. The rare "block sets more than `result`" case.
    // Consumer order: ev_.data (copy), outputs_ (copy), context (move  -  last sink).
    EventBuilder& output(const std::string& slot, ParamKind kind, const Value& v,
                         std::string export_name = {}) {
        nlohmann::json jv = v.to_json();
        if (!slot.empty()) {
            ev_.data[slot] = jv;
            ev_.outputs.push_back({slot, export_name, param_kind_str(kind), jv, false});
            if (outputs_)
                outputs_->push_back({slot, export_name, param_kind_str(kind), jv, false});
            if (rt_ && rt_->publish_output)
                rt_->publish_output(rt_->user_data, rt_->block_id, slot, param_kind_str(kind), jv);
            if (rt_ && !export_name.empty())
                rt_->set_context_value(rt_->user_data, export_name, std::move(jv));
        }
        return *this;
    }
    EventBuilder& set(const std::string& name, ParamKind kind, const Value& v) {
        // Legacy typed set exported under the same name. New handlers should
        // use output(slot, kind, value, export_name) explicitly.
        return output(name, kind, v, name);
    }

    EventBuilder& id(std::string v) {
        if (ev_.block_id.empty())
            ev_.block_id = std::move(v);
        else
            ev_.data["instanceId"] = std::move(v);
        return *this;
    }
    EventBuilder& type(std::string v) { ev_.type = std::move(v); return *this; }
    EventBuilder& code(int c)         { ev_.exit_code = c; ev_.error_code = c; return *this; }

    // Structured event metadata: merge an object's fields into the event data in
    // one shot. For blocks whose output is inherently structured (shell result,
    // modbus register dumps)  -  the metadata counterpart of set(key, value).
    EventBuilder& payload(nlohmann::json fields) {
        if (fields.is_object())
            for (auto it = fields.begin(); it != fields.end(); ++it)
                ev_.data[it.key()] = it.value();
        return *this;
    }

    // Dedicated stdout/stderr line channels (the structured terminal view reads
    // these off the event, separate from the data payload).
    EventBuilder& lines(std::vector<std::string> out, std::vector<std::string> err) {
        ev_.stdout_lines = std::move(out);
        ev_.stderr_lines = std::move(err);
        return *this;
    }

    // Implicit primary output: PREVIOUS (+ storeAs/target) + event "result" field.
    // Consumer order: ev_.data["result"] (copy), outputs_ (copy), PREVIOUS (copy),
    // storeAs (move  -  last sink).  One fewer copy than the naive approach.
    EventBuilder& result(const Value& v, ParamKind kind = ParamKind::json_value) {
        nlohmann::json jv = v.to_json();
        const bool has_store = !store_.empty() && store_ != "PREVIOUS";
        ev_.data["result"] = jv;                                             // copy -> event
        ev_.outputs.push_back({"result", has_store ? store_ : std::string{},
                               param_kind_str(kind), jv, true});
        if (outputs_)
            outputs_->push_back({"result", has_store ? store_ : std::string{},
                                 param_kind_str(kind), jv, true});
        if (rt_) {
            if (rt_->publish_output)
                rt_->publish_output(rt_->user_data, rt_->block_id, "result", param_kind_str(kind), jv);
            if (has_store) {
                rt_->set_context_value(rt_->user_data, "PREVIOUS", jv);             // copy -> PREVIOUS
                rt_->set_context_value(rt_->user_data, store_, std::move(jv));      // move -> storeAs (last)
            } else {
                rt_->set_context_value(rt_->user_data, "PREVIOUS", std::move(jv));  // move -> PREVIOUS (last)
            }
        }
        return *this;
    }

    void emit() {
        if (emitted_ || !rt_) return;
        emitted_ = true;
        if (rt_->events_enabled) {
            rt_->emit(rt_->user_data, std::move(ev_));
        } else {
            // Per-frame pipelines mute the visible event stream, but file outputs
            // (videoWriter, pictureOut, …) must still reach the artifact ledger.
            if (rt_->options.event_sink && ev_.status == "ok")
                rt_->options.event_sink(ev_);
            record_muted_event(rt_->result, ev_, rt_->options.collect_error_events);
        }
    }

private:
    BlockRuntime*              rt_ = nullptr;
    std::string                store_;
    std::vector<BlockOutput>*  outputs_ = nullptr;
    ExecutionEvent             ev_;
    bool                       emitted_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// BlockIO  -  typed accessor over a block's inputs/outputs (S4)
//
// Handlers read parameters as typed values through BlockIO instead of poking at
// the raw nlohmann::json block, and publish results as named, kind-tagged
// outputs. JSON only appears at the two boundaries BlockIO owns: the read
// boundary (block fields supplied by the host/document) and the write boundary
// (`set_context_value`, which is the RPC/serialisation transport). Block logic
// in between stays off raw JSON.
//
// `def()` / `resolved()` are declared here but defined in block_resolver.hpp
// (they need the complete ParamDef + source_param).
// ─────────────────────────────────────────────────────────────────────────────

class BlockIO {
public:
    using Output = BlockOutput; // kept for existing call sites

    BlockIO(const nlohmann::json& block, std::string path, BlockRuntime& runtime)
        : block_(block), path_(std::move(path)), rt_(runtime) {}

    const std::string& path() const noexcept { return path_; }
    BlockRuntime&      runtime() const noexcept { return rt_; }
    const nlohmann::json& raw() const noexcept { return block_; }

    // Run-scoped resource cache (persists across loop passes). Null when the run
    // does not provide one (e.g. detached background work); handlers must
    // null-check and fall back to building a per-call instance.
    RunScope* scope() const noexcept { return rt_.run_scope; }

    // ── Typed inputs ──────────────────────────────────────────────────────────
    // `has` reports *explicit* presence on the block (literal), independent of
    // any declared source/default  -  useful to distinguish "set by author/LLM"
    // from "sourced/defaulted".
    bool has(const char* name) const {
        return block_.is_object() && block_.contains(name) && !block_[name].is_null();
    }

    // Single-value accessors read the pre-resolved typed frame first (sourced +
    // cast + validated by the executor). For params with no declared schema
    // (undeclared fields like "id") they fall back to a lazy field -> source ->
    // default lookup so nothing regresses during migration.
    std::string str(const char* name, std::string dflt = {}) const {
        if (const Value* fv = frame(name)) return fv->as_string(std::move(dflt));
        const nlohmann::json* v = resolved(name);
        if (!v || v->is_null()) return dflt;
        return v->is_string() ? v->get<std::string>() : scalar_to_string(*v);
    }

    // First present (non-null) of several aliased parameter names, else default.
    // Pure field lookup  -  never applies declared value-sourcing  -  so it stays
    // safe for variable-name/alias resolution (e.g. Parse's input/from).
    std::string str_any(std::initializer_list<const char*> names, std::string dflt = {}) const {
        for (const char* n : names) {
            const nlohmann::json* v = field(n);
            if (v && !v->is_null()) {
                if (v->is_string()) {
                    if (!v->get<std::string>().empty())
                        return v->get<std::string>();
                } else {
                    return scalar_to_string(*v);
                }
            }
        }
        return dflt;
    }

    long long i64(const char* name, long long dflt = 0) const {
        if (const Value* fv = frame(name)) return static_cast<long long>(fv->as_int(dflt));
        const nlohmann::json* v = resolved(name);
        if (!v) return dflt;
        if (v->is_number_integer())  return v->get<long long>();
        if (v->is_number_unsigned()) return static_cast<long long>(v->get<unsigned long long>());
        if (v->is_number_float())    return static_cast<long long>(v->get<double>());
        if (v->is_boolean())         return v->get<bool>() ? 1 : 0;
        if (v->is_string()) { try { return std::stoll(v->get<std::string>()); } catch (...) {} }
        return dflt;
    }

    double f64(const char* name, double dflt = 0.0) const {
        if (const Value* fv = frame(name)) return fv->as_double(dflt);
        const nlohmann::json* v = resolved(name);
        if (!v) return dflt;
        if (v->is_number())  return v->get<double>();
        if (v->is_boolean()) return v->get<bool>() ? 1.0 : 0.0;
        if (v->is_string())  { try { return std::stod(v->get<std::string>()); } catch (...) {} }
        return dflt;
    }

    bool flag(const char* name, bool dflt = false) const {
        if (const Value* fv = frame(name)) return fv->as_bool(dflt);
        const nlohmann::json* v = resolved(name);
        if (!v) return dflt;
        if (v->is_boolean()) return v->get<bool>();
        if (v->is_number())  return v->get<double>() != 0.0;
        if (v->is_string()) {
            const std::string s = v->get<std::string>();
            return s == "true" || s == "1" || s == "yes" || s == "on";
        }
        return dflt;
    }

    // Pre-resolved typed value from the executor's input frame (or nullptr when
    // the param has no declared schema). Lets a handler inspect the resolved
    // Value directly  -  e.g. a `.globs()` param yields an array of matched paths.
    const Value* value(const char* name) const { return frame(name); }

    // Escape hatch for blocks that genuinely operate on structured JSON
    // (e.g. the data/parse block). Still a single, explicit boundary.
    const nlohmann::json& json(const char* name) const {
        static const nlohmann::json kNull;
        const nlohmann::json* v = field(name);
        return v ? *v : kNull;
    }

    // ── Input resolution against the scope/context ────────────────────────────
    const nlohmann::json* context(const std::string& name) const {
        return rt_.context_value(rt_.user_data, name);
    }

    // ── Typed outputs (multiple, named, kind-tagged) ──────────────────────────
    // Write a named output variable. Does NOT touch PREVIOUS.
    // outputs_ gets the copy; the context write moves the value (one fewer copy).
    void set_output(const std::string& name, ParamKind kind, nlohmann::json value) {
        outputs_.push_back({name, name, param_kind_str(kind), value, false});
        if (rt_.publish_output)
            rt_.publish_output(rt_.user_data, rt_.block_id, name, param_kind_str(kind), value);
        if (!name.empty())
            rt_.set_context_value(rt_.user_data, name, std::move(value));
    }

    // Set PREVIOUS only (the implicit chained output).
    // outputs_ gets the copy; the context write moves the value (one fewer copy).
    void set_previous(nlohmann::json value, ParamKind kind = ParamKind::json_value) {
        outputs_.push_back({"result", {}, param_kind_str(kind), value, true});
        if (rt_.publish_output)
            rt_.publish_output(rt_.user_data, rt_.block_id, "result", param_kind_str(kind), value);
        rt_.set_context_value(rt_.user_data, "PREVIOUS", std::move(value));
    }

    // Set PREVIOUS in the live context WITHOUT recording an output. For native
    // container loops (e.g. a frame pump) that re-seed PREVIOUS for every child
    // pass  -  recording one output per frame would bloat the event/output log.
    void prime_previous(nlohmann::json value) {
        rt_.set_context_value(rt_.user_data, "PREVIOUS", std::move(value));
    }

    // Event gate (B-perf): a native container running children at high frequency
    // (e.g. a frame pump's per-frame pipeline) can mute child events to avoid
    // thousands of per-frame records. PREVIOUS/storeAs still flow when muted  - 
    // only the emitted event is suppressed. Restore the saved value afterwards.
    bool events_enabled() const noexcept { return rt_.events_enabled; }
    void set_events_enabled(bool on) noexcept { rt_.events_enabled = on; }

    // Primary result: sets PREVIOUS plus any `storeAs`/`target` sink declared on
    // the block. This mirrors the convention used across the builtin blocks.
    //
    // Consumer ordering (3 potential sinks, N copies minimised):
    //   outputs_  ← always gets the output record (copy if storeAs exists, else move)
    //   PREVIOUS  ← always written (copy if storeAs exists, else move from outputs_ copy)
    //   storeAs   ← written last, receives a move (the cheapest path for the final sink)
    void set_result(nlohmann::json value, ParamKind kind = ParamKind::json_value) {
        const std::string store = str_any({"storeAs", "target"});
        const bool has_store = !store.empty() && store != "PREVIOUS";
        if (has_store) {
            // 3 consumers: outputs_, PREVIOUS, storeAs.  2 copies + 1 move.
            outputs_.push_back({"result", store, param_kind_str(kind), value, true});
            if (rt_.publish_output)
                rt_.publish_output(rt_.user_data, rt_.block_id, "result", param_kind_str(kind), value);
            rt_.set_context_value(rt_.user_data, "PREVIOUS", value);         // copy -> PREVIOUS
            rt_.set_context_value(rt_.user_data, store, std::move(value));   // move -> storeAs (last)
        } else {
            // 2 consumers: outputs_, PREVIOUS.  1 copy + 1 move.
            outputs_.push_back({"result", {}, param_kind_str(kind), value, true});
            if (rt_.publish_output)
                rt_.publish_output(rt_.user_data, rt_.block_id, "result", param_kind_str(kind), value);
            rt_.set_context_value(rt_.user_data, "PREVIOUS", std::move(value));          // move -> PREVIOUS (last)
        }
    }

    const std::vector<Output>& outputs() const noexcept { return outputs_; }

    // ── Typed events (B1)  -  replace `runtime.emit(ExecutionEvent{...})` ──────────
    // The builder captures the block's storeAs/target sink so `.result(v)` can
    // do the implicit PREVIOUS/storeAs flush.
    EventBuilder event(std::string kind, std::string status, std::string message, int code = 0) {
        ExecutionEvent ev;
        ev.path       = path_;
        ev.kind       = std::move(kind);
        ev.status     = std::move(status);
        ev.message    = std::move(message);
        ev.exit_code  = code;
        ev.block_id   = rt_.block_id;
        // error_code left 0; the runtime sink derives it from exit_code for errors.
        return EventBuilder{rt_, str_any({"storeAs", "target"}), &outputs_, std::move(ev)};
    }
    EventBuilder ok(std::string kind, std::string message)    { return event(std::move(kind), "ok",      std::move(message)); }
    EventBuilder warn(std::string kind, std::string message)  { return event(std::move(kind), "warning", std::move(message)); }
    EventBuilder error(std::string kind, std::string message, int code = 2) { return event(std::move(kind), "error", std::move(message), code); }

    // ── Child execution by declared list key (B1) ─────────────────────────────
    // Runs the child block list named `key` (e.g. "items"/"consequent"), threading
    // the child path. Containers never touch the raw JSON array.
    void run(const char* key) {
        rt_.execute_block_list(rt_.user_data, child_list(key), child_list_path(key));
    }
    bool has_children(const char* key) const {
        const nlohmann::json* f = field(key);
        return f && f->is_array() && !f->empty();
    }

    // ── Cancellation (B-async) ────────────────────────────────────────────────
    // Long-running / background-capable handlers poll this between work slices.
    bool cancelled() const {
        return rt_.cancel_requested && rt_.cancel_requested(rt_.user_data);
    }

    // Cancellable sleep: waits up to `ms`, waking early (returning true) if
    // cancellation is requested. Replaces the hand-rolled 50ms slice loops in
    // the audio/video handlers. Returns true iff cancelled during the wait.
    bool wait(long long ms, long long slice_ms = 50) const {
        if (ms <= 0) return cancelled();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancelled()) return true;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining < slice_ms ? remaining : slice_ms));
        }
        return cancelled();
    }

    // ── Diagnostic logging (developer logs, NOT the event stream) ─────────────
    // Distinct from ok()/warn()/error() which emit flow-visible events. Routed to
    // logger::* via block_log (defined in builtin_blocks.cpp).
    void log(const char* level, const std::string& message) const { block_log(level, message); }
    void trace(const std::string& m) const { block_log("trace", m); }
    void debug(const std::string& m) const { block_log("debug", m); }
    void info(const std::string& m)  const { block_log("info",  m); }

private:
    // Pre-resolved typed value for `name` from the executor's input frame, or
    // nullptr when the param has no declared schema (falls back to `resolved`).
    const Value* frame(const char* name) const {
        return rt_.inputs ? rt_.inputs->find(name) : nullptr;
    }

    // Declared schema for `name` on the block being executed (or nullptr).
    const ParamDef* def(const char* name) const;
    // Effective value for `name`: explicit field -> declared source -> default.
    // Returns nullptr when nothing applies. Defined out-of-line (block_resolver.hpp)
    // once ParamDef is complete.
    const nlohmann::json* resolved(const char* name) const;

    const nlohmann::json& child_list(const char* key) const {
        static const nlohmann::json kEmpty = nlohmann::json::array();
        const nlohmann::json* f = field(key);
        return (f && f->is_array()) ? *f : kEmpty;
    }
    std::string child_list_path(const char* key) const {
        if (!rt_.events_enabled) return {};
        return path_ + "/" + key;
    }

    const nlohmann::json* field(const char* name) const {
        if (!block_.is_object()) return nullptr;
        const auto it = block_.find(name);
        return it == block_.end() ? nullptr : &(*it);
    }

    static std::string scalar_to_string(const nlohmann::json& v) {
        if (v.is_string())           return v.get<std::string>();
        if (v.is_boolean())          return v.get<bool>() ? "true" : "false";
        if (v.is_number_integer())   return std::to_string(v.get<long long>());
        if (v.is_number_unsigned())  return std::to_string(v.get<unsigned long long>());
        if (v.is_number_float()) {
            std::string s = nlohmann::json(v.get<double>()).dump();
            return s;
        }
        if (v.is_null())             return {};
        return v.dump();
    }

    const nlohmann::json& block_;
    std::string           path_;
    BlockRuntime&         rt_;
    std::vector<Output>   outputs_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Handler adapters (B2 migration)
//
// `legacy()` wraps a raw-JSON handler as a BlockHandler closure; the runtime's
// resolved input frame still flows because BlockIO is rebuilt over the same
// BlockRuntime. `HandlerArg` lets registration sites pass *either* a native
// `bool(BlockIO&)` handler/lambda or a legacy fn-ptr unchanged  -  the legacy one
// auto-adapts  -  so the file-by-file migration needs no churn at the call sites.
// ─────────────────────────────────────────────────────────────────────────────

// bool -> BlockResult bridge for migrating handlers: historically a handler
// returned true ("handled") and signalled failure by emitting an error *event*.
// That contract is preserved  -  both true and false map to Ok, so the flow-stop
// path stays driven by error events until a handler opts into returning
// Error/Fatal explicitly.
inline BlockHandler legacy(LegacyBlockHandler fn) {
    return [fn](BlockIO& io) { (void)fn(io.raw(), io.path(), io.runtime()); return BlockResult::Ok; };
}

struct HandlerArg {
    BlockHandler fn;
    HandlerArg(BlockHandler h)                  : fn(std::move(h)) {}
    HandlerArg(BlockResult (*native)(BlockIO&)) : fn(native) {}                 // new typed handler
    HandlerArg(bool (*native)(BlockIO&))                                       // bool handler (one step)
        : fn([native](BlockIO& io) { (void)native(io); return BlockResult::Ok; }) {}
    HandlerArg(LegacyBlockHandler h)                                           // legacy raw-JSON handler
        : fn([h](BlockIO& io) { (void)h(io.raw(), io.path(), io.runtime()); return BlockResult::Ok; }) {}
    operator BlockHandler() const { return fn; }
};

} // namespace media::xblox::blocks
