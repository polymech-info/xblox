#include "blocks/json_query.hpp"
#include "xblox_perf.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if FEATURE_JQ
extern "C" {
#  include "jq.h"
#  include "jv.h"
}
#endif

namespace media::xblox::blocks::json_query {
namespace {

std::vector<std::string> split_filter_path(std::string filter)
{
    std::vector<std::string> parts;
    if (!filter.empty() && filter.front() == '.')
        filter.erase(filter.begin());
    std::string current;
    for (char ch : filter) {
        if (ch == '.') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        parts.push_back(std::move(current));
    return parts;
}

const nlohmann::json* select_path(const nlohmann::json& value, const std::string& filter)
{
    if (filter.empty() || filter == ".")
        return &value;
    const auto parts = split_filter_path(filter);
    const nlohmann::json* current = &value;
    for (const auto& part : parts) {
        if (!current)
            return nullptr;
        if (current->is_object()) {
            if (!current->contains(part))
                return nullptr;
            current = &(*current)[part];
            continue;
        }
        if (current->is_array()) {
            try {
                size_t consumed = 0;
                const size_t index = static_cast<size_t>(std::stoull(part, &consumed));
                if (consumed != part.size() || index >= current->size())
                    return nullptr;
                current = &(*current)[index];
                continue;
            } catch (...) {
                return nullptr;
            }
        }
        return nullptr;
    }
    return current;
}

bool parse_input(const nlohmann::json& input, nlohmann::json& parsed, std::string& err, bool string_fallback)
{
    try {
        if (input.is_object() && input.contains("raw") && input["raw"].is_string()) {
            parsed = nlohmann::json::parse(input["raw"].get<std::string>());
            return true;
        }
        if (input.is_string()) {
            parsed = nlohmann::json::parse(input.get<std::string>());
            return true;
        }
        parsed = input;
        return true;
    } catch (const std::exception& e) {
        if (string_fallback && input.is_string()) {
            parsed = input;
            return true;
        }
        err = e.what();
        return false;
    }
}

bool is_simple_path(const std::string& filter)
{
    if (filter.empty() || filter == ".") return true;
    if (filter.front() != '.') return false;
    for (char ch : filter) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) &&
            ch != '.' && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

#if FEATURE_JQ

static void jq_err_cb(void* data, jv msg)
{
    auto* err = reinterpret_cast<std::string*>(data);
    if (jv_get_kind(msg) == JV_KIND_STRING)
        *err += jv_string_value(msg);
    jv_free(msg);
}

// Thread-local cache: filter string -> compiled jq_state*.
// States are compiled once and reused across loop iterations; jq_start() resets
// execution state for a new input without recompiling the program.
// Lifetime is the thread (fine for CLI  -  one thread per document run).
thread_local std::unordered_map<std::string, jq_state*> s_jq_compiled;

// Build a jv value directly from nlohmann::json  -  no string serialisation.
// Returns a fresh jv (refcnt=1); caller owns it.
static jv nlohmann_to_jv(const nlohmann::json& j)
{
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            return jv_null();
        case nlohmann::json::value_t::boolean:
            return j.get<bool>() ? jv_true() : jv_false();
        case nlohmann::json::value_t::number_integer:
            return jv_number(static_cast<double>(j.get<int64_t>()));
        case nlohmann::json::value_t::number_unsigned:
            return jv_number(static_cast<double>(j.get<uint64_t>()));
        case nlohmann::json::value_t::number_float:
            return jv_number(j.get<double>());
        case nlohmann::json::value_t::string: {
            const std::string& s = j.get_ref<const std::string&>();
            return jv_string_sized(s.c_str(), static_cast<int>(s.size()));
        }
        case nlohmann::json::value_t::array: {
            jv arr = jv_array_sized(static_cast<int>(j.size()));
            for (const auto& item : j)
                arr = jv_array_append(arr, nlohmann_to_jv(item));
            return arr;
        }
        case nlohmann::json::value_t::object: {
            jv obj = jv_object();
            for (const auto& [key, val] : j.items())
                obj = jv_object_set(obj,
                                    jv_string_sized(key.c_str(), static_cast<int>(key.size())),
                                    nlohmann_to_jv(val));
            return obj;
        }
        default:
            return jv_null();
    }
}

// Convert a jv value to nlohmann::json without going through a JSON string.
// Consumes (frees) v.
// NOTE: jv_object_iter_* functions do NOT consume the object; they return
// jv_copy() of keys/values and walk the raw hash-table slots via the opaque
// integer iterator  -  not contiguous 0..n-1 indices.
static nlohmann::json jv_to_nlohmann(jv v)
{
    switch (jv_get_kind(v)) {
        case JV_KIND_NULL:   jv_free(v); return nullptr;
        case JV_KIND_FALSE:  jv_free(v); return false;
        case JV_KIND_TRUE:   jv_free(v); return true;
        case JV_KIND_NUMBER: { double d = jv_number_value(v); jv_free(v); return d; }
        case JV_KIND_STRING: {
            std::string s(jv_string_value(v));
            jv_free(v);
            return s;
        }
        case JV_KIND_ARRAY: {
            nlohmann::json arr = nlohmann::json::array();
            const int len = jv_array_length(jv_copy(v));
            for (int i = 0; i < len; ++i)
                arr.push_back(jv_to_nlohmann(jv_array_get(jv_copy(v), i)));
            jv_free(v);
            return arr;
        }
        case JV_KIND_OBJECT: {
            nlohmann::json obj = nlohmann::json::object();
            // Use the proper hash-table slot iterator  -  iter values are opaque
            // slot indices, NOT 0..length-1.
            for (int i = jv_object_iter(v); jv_object_iter_valid(v, i);
                 i = jv_object_iter_next(v, i)) {
                jv key = jv_object_iter_key(v, i);    // returns jv_copy, non-consuming
                jv val = jv_object_iter_value(v, i);  // returns jv_copy, non-consuming
                obj[jv_string_value(key)] = jv_to_nlohmann(val);  /* val consumed */
                jv_free(key);
            }
            jv_free(v);
            return obj;
        }
        default:
            jv_free(v);
            return nullptr;
    }
}

bool eval_jq_filter(const nlohmann::json& parsed,
                    const std::string& filter,
                    bool collect_outputs,
                    nlohmann::json& result,
                    std::string& err)
{
    using media::xblox::g_xblox_perf;
    using media::xblox::ScopedPerfTimer;
    ++g_xblox_perf.jq_calls;

    // Build jv input directly from nlohmann::json  -  no string round-trip.
    jv jv_input;
    { ScopedPerfTimer t(g_xblox_perf.to_jv_ns); jv_input = nlohmann_to_jv(parsed); }

    // Look up (or compile and cache) the jq program for this filter.
    jq_state* jq = nullptr;
    auto it = s_jq_compiled.find(filter);
    if (it != s_jq_compiled.end()) {
        jq = it->second;
        ++g_xblox_perf.jq_cache_hits;
    } else {
        jq = jq_init();
        if (!jq) {
            err = "jq_init failed";
            jv_free(jv_input);
            return false;
        }
        std::string compile_err;
        jq_set_error_cb(jq, jq_err_cb, &compile_err);
        if (!jq_compile(jq, filter.c_str())) {
            err = compile_err.empty() ? ("jq compile error: " + filter) : compile_err;
            jq_teardown(&jq);
            jv_free(jv_input);
            return false;
        }
        s_jq_compiled.emplace(filter, jq);
    }

    // Bind the per-call error sink and run the VM.
    jq_set_error_cb(jq, jq_err_cb, &err);
    bool ok = false;
    {
        ScopedPerfTimer t(g_xblox_perf.jq_vm_ns);
        jq_start(jq, jv_input, 0);  /* jv_input is consumed */

        if (collect_outputs)
            result = nlohmann::json::array();

        for (;;) {
            jv jv_result = jq_next(jq);
            if (!jv_is_valid(jv_result)) {
                jv msg = jv_invalid_get_msg(jv_copy(jv_result));
                if (jv_get_kind(msg) == JV_KIND_STRING && err.empty()) {
                    err = "jq: ";
                    err += jv_string_value(msg);
                }
                jv_free(msg);
                jv_free(jv_result);
                break;
            }

            // Convert directly jv->nlohmann without string serialisation.
            // The timer wraps only the jv->nlohmann part; jq_next is inside jq_vm_ns.
            nlohmann::json parsed_result;
            { ScopedPerfTimer t2(g_xblox_perf.from_jv_ns); parsed_result = jv_to_nlohmann(jv_result); }

            if (collect_outputs) {
                result.push_back(std::move(parsed_result));
                ok = true;
            } else {
                result = std::move(parsed_result);
                ok = true;
                break;
            }
        }
    }

    // State is cached  -  do NOT call jq_teardown.
    if (collect_outputs)
        return err.empty();
    if (!ok && err.empty())
        err = "jq returned no value for filter: " + filter;
    return ok && err.empty();
}

#endif

} // namespace

bool evaluate(const nlohmann::json& input,
              const std::string& filter,
              nlohmann::json& result,
              std::string& err,
              QueryOptions options)
{
    nlohmann::json parsed;
    if (!parse_input(input, parsed, err, options.string_fallback))
        return false;

#if FEATURE_JQ
    if (is_simple_path(filter)) {
        const nlohmann::json* selected = select_path(parsed, filter);
        if (!selected) {
            err = "filter returned no value: " + filter;
            return false;
        }
        result = *selected;
        return true;
    }
    return eval_jq_filter(parsed, filter, options.collect_outputs, result, err);
#else
    (void)options;
    const nlohmann::json* selected = select_path(parsed, filter);
    if (!selected) {
        err = "jq filter returned no value: " + filter;
        return false;
    }
    result = *selected;
    return true;
#endif
}

bool collect_paths(const nlohmann::json& input,
                   const std::string& filter,
                   nlohmann::json& path_arrays,
                   std::string& err)
{
    nlohmann::json parsed;
    if (!parse_input(input, parsed, err, /*string_fallback=*/false))
        return false;

    std::string expr = filter;
    // Accept a few JSONPath-ish spellings used by the TS iterator and rewrite
    // them to jq (no jsonpath-plus in-tree).
    if (!expr.empty() && expr.front() == '$')
        expr.erase(expr.begin());
    // $[*] / .foo[*].bar → .foo[].bar
    {
        std::string rewritten;
        rewritten.reserve(expr.size());
        for (std::size_t i = 0; i < expr.size(); ++i) {
            if (expr[i] == '[' && i + 2 < expr.size() && expr[i + 1] == '*' && expr[i + 2] == ']') {
                rewritten += "[]";
                i += 2;
            } else {
                rewritten.push_back(expr[i]);
            }
        }
        expr = std::move(rewritten);
    }
    if (expr.empty())
        expr = ".";
    if (expr.front() != '.' && expr.front() != '[')
        expr.insert(expr.begin(), '.');

    // Already a path-collector program?
    const bool looks_wrapped =
        expr.size() >= 7 &&
        (expr.rfind("[path(", 0) == 0 || expr.rfind("path(", 0) == 0);

    std::string program;
    if (looks_wrapped && expr.rfind("[path(", 0) == 0) {
        program = expr;
    } else if (looks_wrapped && expr.rfind("path(", 0) == 0) {
        program = "[" + expr + "]";
    } else {
        program = "[path(" + expr + ")]";
    }

#if FEATURE_JQ
    QueryOptions opts;
    opts.collect_outputs = false;
    if (!evaluate(parsed, program, path_arrays, err, opts))
        return false;
    if (!path_arrays.is_array()) {
        err = "collect_paths expected a JSON array of path arrays";
        return false;
    }
    return true;
#else
    err = "collect_paths requires FEATURE_JQ";
    (void)program;
    return false;
#endif
}

nlohmann::json normalize_iterable_value(const nlohmann::json& value, const std::string& mode)
{
    if (mode == "array") {
        return value.is_array() ? value : nlohmann::json::array();
    }
    if (mode == "objectValues") {
        nlohmann::json out = nlohmann::json::array();
        if (value.is_object())
            for (auto it = value.begin(); it != value.end(); ++it)
                out.push_back(it.value());
        return out;
    }
    if (mode == "objectEntries") {
        nlohmann::json out = nlohmann::json::array();
        if (value.is_object())
            for (auto it = value.begin(); it != value.end(); ++it)
                out.push_back({{"key", it.key()}, {"value", it.value()}});
        return out;
    }
    if (mode == "once") {
        return nlohmann::json::array({value});
    }

    if (value.is_array())
        return value;
    if (value.is_object()) {
        nlohmann::json out = nlohmann::json::array();
        for (auto it = value.begin(); it != value.end(); ++it)
            out.push_back({{"key", it.key()}, {"value", it.value()}});
        return out;
    }
    if (value.is_null())
        return nlohmann::json::array();
    return nlohmann::json::array({value});
}

} // namespace media::xblox::blocks::json_query
