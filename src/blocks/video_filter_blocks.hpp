#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

// Single-frame image/video filter blocks (grayscale, color, crop, resize) plus
// pictureOut. They consume a PREVIOUS frame handle (zero-copy) or an image path
// and emit a handle/path via the run-scoped FrameStore. Registered no-op when
// FEATURE_VIDEO is off.
void register_video_filter_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
