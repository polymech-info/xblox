#include "xblox_prepared_run.hpp"

#include "xblox_commands.hpp"

#include <algorithm>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <type_traits>
#include "fs/path_sanitizer.hpp"

namespace media::xblox {
namespace {

bool read_json_file(const std::filesystem::path& path, nlohmann::json& out, std::string& err)
{
    try {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            err = "cannot open " + media::path::path_to_utf8(path);
            return false;
        }
        std::ostringstream ss;
        ss << ifs.rdbuf();
        out = nlohmann::json::parse(ss.str());
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool nested_profile_enabled()
{
    static const bool enabled = std::getenv("TANIT_XBLOX_PROFILE_BLOCKS") != nullptr ||
                                std::getenv("TANIT_XBLOX_PROFILE_NESTED") != nullptr;
    return enabled;
}

struct NestedProfileTotals {
    std::mutex mutex;
    std::map<std::string, std::pair<unsigned long long, unsigned long long>> by_phase;

    ~NestedProfileTotals()
    {
        if (!nested_profile_enabled())
            return;
        std::lock_guard<std::mutex> lock(mutex);
        std::fprintf(stderr, "[xblox:profile] nested totals\n");
        for (const auto& [phase, totals] : by_phase) {
            const auto count = totals.first;
            const auto micros = totals.second;
            const double avg = count ? static_cast<double>(micros) / static_cast<double>(count) : 0.0;
            std::fprintf(stderr, "[xblox:profile] phase=%s count=%llu total_us=%llu avg_us=%.2f\n",
                         phase.c_str(), count, micros, avg);
        }
    }
};

NestedProfileTotals& nested_profile_totals()
{
    static NestedProfileTotals totals;
    return totals;
}

void record_nested_profile(const char* phase, long long micros)
{
    if (!nested_profile_enabled())
        return;
    auto& totals = nested_profile_totals();
    std::lock_guard<std::mutex> lock(totals.mutex);
    auto& item = totals.by_phase[phase];
    ++item.first;
    item.second += static_cast<unsigned long long>(std::max<long long>(0, micros));
}

template <class Fn>
auto time_nested_phase(const char* phase, Fn&& fn)
{
    const auto start = std::chrono::steady_clock::now();
    if constexpr (std::is_void_v<decltype(fn())>) {
        fn();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        record_nested_profile(phase, elapsed);
    } else {
        auto result = fn();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        record_nested_profile(phase, elapsed);
        return result;
    }
}

} // namespace

PreparedXbloxRun::PreparedXbloxRun(std::filesystem::path path)
    : path_(std::move(path))
{
    if (!path_.empty())
        reload();
}

bool PreparedXbloxRun::reload()
{
    std::lock_guard<std::mutex> lock(engine_mutex_);
    error_.clear();
    ok_ = read_json_file(path_, document_, error_);
    roots_ = nlohmann::json::array();
    base_context_ = nlohmann::json::object();
    document_loop_enabled_ = false;
    if (ok_ && !document_.is_object())
        document_ = nlohmann::json::object();
    if (ok_) {
        roots_ = document_.value("roots", nlohmann::json::array());
        base_context_ = document_.value("context", nlohmann::json::object());
        if (!roots_.is_array())
            roots_ = nlohmann::json::array();
        if (!base_context_.is_object())
            base_context_ = nlohmann::json::object();
        if (document_.contains("loop") && document_["loop"].is_object())
            document_loop_enabled_ = document_["loop"].value("enabled", false);
    }
    engine_ = std::make_unique<PassEngine>();
    return ok_;
}

nlohmann::json PreparedXbloxRun::merged_context(const nlohmann::json& context_overrides,
                                                const nlohmann::json& arg_overrides) const
{
    nlohmann::json context = base_context_.is_object() ? base_context_ : nlohmann::json::object();
    if (context_overrides.is_object()) {
        for (auto it = context_overrides.begin(); it != context_overrides.end(); ++it)
            context[it.key()] = it.value();
    }
    if (arg_overrides.is_object()) {
        for (auto it = arg_overrides.begin(); it != arg_overrides.end(); ++it)
            context[it.key()] = it.value();
    }
    context["args"] = arg_overrides.is_object() ? arg_overrides : nlohmann::json::object();
    return context;
}

nlohmann::json PreparedXbloxRun::instantiate(const nlohmann::json& context_overrides,
                                             const nlohmann::json& arg_overrides) const
{
    nlohmann::json document = document_.is_object() ? document_ : nlohmann::json::object();
    document["context"] = merged_context(context_overrides, arg_overrides);
    return document;
}

bool PreparedXbloxRun::should_run_document_loop(const ExecutionOptions& options) const
{
    if (options.loop_override)
        return *options.loop_override;
    return document_loop_enabled_;
}

ExecutionResult PreparedXbloxRun::run_once(const nlohmann::json& context_overrides,
                                           const nlohmann::json& arg_overrides,
                                           const ExecutionOptions& options) const
{
    if (!should_run_document_loop(options)) {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        ExecutionResult result;
        if (!engine_)
            engine_ = std::make_unique<PassEngine>();
        nlohmann::json context = time_nested_phase("merge_context", [&] {
            return merged_context(context_overrides, arg_overrides);
        });
        time_nested_phase("engine_prepare", [&] {
            engine_->prepare(roots_, context, options);
        });
        time_nested_phase("engine_run_pass", [&] {
            engine_->run_pass(roots_, options, result);
        });
        return result;
    }
    return time_nested_phase("run_blocks_file", [&] {
        return run_blocks_file(instantiate(context_overrides, arg_overrides), options);
    });
}

} // namespace media::xblox
