#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler network_block_handler(const std::string& kind);
bool network_block_allowed(const std::string& kind);
void register_network_blocks(BlockRegistry& registry);
void register_network_mcp_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
