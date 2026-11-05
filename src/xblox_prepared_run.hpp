#pragma once

#include "runtime.hpp"
#include "xblox_types.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace media::xblox {

class PreparedXbloxRun {
public:
    explicit PreparedXbloxRun(std::filesystem::path path = {});

    const std::filesystem::path& path() const noexcept { return path_; }
    bool ok() const noexcept { return ok_; }
    const std::string& error() const noexcept { return error_; }

    bool reload();
    nlohmann::json instantiate(const nlohmann::json& context_overrides,
                               const nlohmann::json& arg_overrides) const;
    ExecutionResult run_once(const nlohmann::json& context_overrides,
                             const nlohmann::json& arg_overrides,
                             const ExecutionOptions& options) const;

private:
    nlohmann::json merged_context(const nlohmann::json& context_overrides,
                                  const nlohmann::json& arg_overrides) const;
    bool should_run_document_loop(const ExecutionOptions& options) const;

    std::filesystem::path path_;
    nlohmann::json document_ = nlohmann::json::object();
    nlohmann::json roots_ = nlohmann::json::array();
    nlohmann::json base_context_ = nlohmann::json::object();
    mutable std::mutex engine_mutex_;
    mutable std::unique_ptr<PassEngine> engine_;
    std::string error_;
    bool document_loop_enabled_ = false;
    bool ok_ = false;
};

} // namespace media::xblox
