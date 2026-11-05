#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler llm_block_handler(const std::string& kind);
void register_llm_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
