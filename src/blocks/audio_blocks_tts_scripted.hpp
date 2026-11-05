#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

/// Scripted TTS / SRT voice-over (`audioTtsScripted`). Registered first.
void register_audio_tts_scripted_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
