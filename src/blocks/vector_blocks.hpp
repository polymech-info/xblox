#pragma once
// vector_blocks.hpp  -  XBlox LiteLLM vector store blocks.
//
// Compiled only when FEATURE_VECTOR_XBLOX=1 (CMake preset flag).
// register_vector_blocks() is always declared so builtin_blocks.cpp can
// guard its call with the same macro.

#include "blocks/block_registry.hpp"

namespace media::xblox::blocks {

void register_vector_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
