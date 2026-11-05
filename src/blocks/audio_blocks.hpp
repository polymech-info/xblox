#pragma once

#include "builtin_blocks.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace media::xblox::blocks {

void register_audio_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
