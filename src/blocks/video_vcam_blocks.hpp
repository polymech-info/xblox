#pragma once

#include "builtin_blocks.hpp"

namespace media::xblox::blocks {

// Pushes a BGR24 frame handle into the same VcamSink as `video record --virtual-out`.
// Registered only when FEATURE_VIDEO_VCAM is on.
void register_video_vcam_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
