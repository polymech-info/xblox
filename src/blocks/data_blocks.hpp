#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler data_block_handler(const std::string& kind);
void register_data_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
