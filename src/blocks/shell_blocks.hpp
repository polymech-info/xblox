#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler shell_block_handler(const std::string& kind);
void register_shell_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
