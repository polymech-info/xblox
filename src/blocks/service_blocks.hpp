#pragma once
// service_blocks.hpp  -  XBlox service blocks (Pixlwiz API: images, files, pages, posts, categories).
//
// Compiled only when FEATURE_SERVICE_XBLOX=1 (CMake preset flag).
// register_service_blocks() is always declared so builtin_blocks.cpp can
// guard its call with the same macro.

#include "blocks/block_registry.hpp"

namespace media::xblox::blocks {

void register_service_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
