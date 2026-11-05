#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

bool fs_new_block(BlockIO& io);
void register_fs_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
