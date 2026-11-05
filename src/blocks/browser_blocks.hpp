#pragma once

#include "blocks/builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler browser_block_handler(const std::string& kind);
void register_browser_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
