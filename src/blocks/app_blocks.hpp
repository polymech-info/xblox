#pragma once

// XBlox "App" block family  -  desktop automation primitives.
//
// These blocks expose the same surface as the LLM `computer-use` tools
// (window targeting + UIA inspection + input synthesis) to the visual
// block runtime so users can author scripts in-IDE without going through
// the agent loop. All blocks are Win32-only and gated on FEATURE_ASSISTANT
// at compile time  -  on other platforms / configurations the registration
// is a no-op and the kinds are simply unknown.
//
// The underlying engines (`media::assistant::app_use`, `app_inspect`,
// `app_batch`) are the exact same libraries the agent tools use, so
// behaviour matches 1:1 between block scripts and agent actions.

#include "blocks/builtin_blocks.hpp"

namespace media::xblox::blocks {

BlockHandler app_block_handler(const std::string& kind);
void register_app_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
