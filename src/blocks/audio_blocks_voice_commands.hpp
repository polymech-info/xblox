#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

/// Voice Command Center blocks (`voiceListen` / replay / stop / status).
/// Gated on FEATURE_VOICE_COMMANDS; no-op registration when the feature is off.
void register_audio_voice_command_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
