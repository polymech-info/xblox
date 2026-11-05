#pragma once
// xblox_perf.hpp  -  lightweight per-thread performance counters for xblox.
//
// Usage:
//   #include "xblox_perf.hpp"
//
//   { ScopedPerfTimer t(g_xblox_perf.jq_vm_ns); /* ... */ }
//   g_xblox_perf.jq_calls++;
//
// A single inline thread_local instance (C++17) shared across all TUs.
// Zero overhead when --profile is not printed; the only cost is two
// clock reads per timed scope.

#include <chrono>
#include <cstdint>

namespace media::xblox {

struct XbloxPerfCounters {
    // ── jq path (json_query.cpp) ─────────────────────────────────────────────
    uint64_t jq_calls       = 0;   // eval_jq_filter invocations
    uint64_t jq_cache_hits  = 0;   // filter found in compiled-state cache
    int64_t  jq_vm_ns       = 0;   // time inside jq_start + jq_next loop
    int64_t  to_jv_ns       = 0;   // nlohmann_to_jv (input conversion)
    int64_t  from_jv_ns     = 0;   // jv_to_nlohmann (output conversion)

    // ── event / variable layer (xblox_commands.cpp) ──────────────────────────
    uint64_t emit_calls     = 0;   // emit() invocations
    int64_t  emit_ns        = 0;   // time inside emit()
    uint64_t set_ctx_calls  = 0;   // bridge_set_context_value() invocations
    int64_t  set_ctx_ns     = 0;   // time inside bridge_set_context_value()

    // ── expression front-end (eval_mu_expression) ───────────────────────────
    uint64_t expr_calls     = 0;   // eval_mu_expression invocations
    uint64_t expr_prep_hits = 0;   // muParser bytecode reused (no recompile)
    int64_t  resolve_ns     = 0;   // resolve_variables() string pre-pass
    int64_t  prepare_ns     = 0;   // prepare_expression() (trim/replace/refs)
    int64_t  mu_eval_ns     = 0;   // muParser bind + Eval() (cached bytecode)

    void reset() { *this = {}; }

    // Convenience: total jq time including conversion bookends.
    int64_t jq_total_ns() const { return to_jv_ns + jq_vm_ns + from_jv_ns; }
};

inline thread_local XbloxPerfCounters g_xblox_perf;

// RAII timer: on destruction adds elapsed nanoseconds to `accumulator`.
struct ScopedPerfTimer {
    int64_t& accumulator;
    std::chrono::high_resolution_clock::time_point start;

    explicit ScopedPerfTimer(int64_t& acc)
        : accumulator(acc)
        , start(std::chrono::high_resolution_clock::now())
    {}

    ~ScopedPerfTimer()
    {
        accumulator += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - start).count();
    }

    ScopedPerfTimer(const ScopedPerfTimer&) = delete;
    ScopedPerfTimer& operator=(const ScopedPerfTimer&) = delete;
};

} // namespace media::xblox
