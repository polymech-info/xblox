#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

// Win32-only Bluetooth + audio-endpoint blocks. On non-Win32 / when
// FEATURE_COMMAND_BLUETOOTH is off, the registry stays empty.
void register_bluetooth_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
