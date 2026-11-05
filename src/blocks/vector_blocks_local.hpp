#pragma once
// vector_blocks_local.hpp  -  XBlox local (offline) vector store blocks.
//
// Blocks: vectorLocalOpen · vectorLocalAddText · vectorLocalAddFile · vectorLocalSearch
//
// Compiled only when FEATURE_VECTOR_LOCAL_XBLOX=1 (requires FEATURE_LLAMA).
// register_vector_local_blocks() is always declared so builtin_blocks.cpp can
// guard its call with the same macro.

#include "blocks/block_registry.hpp"

namespace media::xblox::blocks {

void register_vector_local_blocks(BlockRegistry& registry);

#if defined(FEATURE_VECTOR_LOCAL_XBLOX) && FEATURE_VECTOR_LOCAL_XBLOX
// Release cached VectorStore instances (and their llama embedding models) before
// process exit. Avoids teardown crashes when static destructors run late.
void clear_vector_local_store_registry();
#endif

} // namespace media::xblox::blocks
