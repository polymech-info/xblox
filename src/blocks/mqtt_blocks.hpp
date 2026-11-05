#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler mqtt_block_handler(const std::string& kind);
void register_mqtt_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
