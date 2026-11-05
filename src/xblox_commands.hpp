#pragma once

#include "runtime.hpp"

namespace media::xblox {

ExecutionResult run_blocks_file(const nlohmann::json& blocks_file, const ExecutionOptions& options = {});

ExecutionResult run_block_roots(const nlohmann::json& roots,
                                const nlohmann::json& context = nlohmann::json::object(),
                                const ExecutionOptions& options = {});

std::vector<ExecutionEvent> build_execution_plan(const nlohmann::json& blocks_file);

} // namespace media::xblox
