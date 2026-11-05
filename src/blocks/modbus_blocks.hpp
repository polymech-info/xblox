#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler modbus_block_handler(const std::string& kind);
void register_modbus_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
