#pragma once
// input_blocks.hpp  -  keyboard (and future mouse) input blocks.
//
// Blocks:
//   keyWait    -  wait for a keyboard shortcut then run child blocks.
//               on=pressed  fires on leading edge.
//               on=released fires on trailing edge.
//               on=held     fires while the key is down.
//               on=toggle   starts children on first press, cancels them on second.
//               repeat=true keeps listening across multiple events.
//
// Win32 only for now (uses GetAsyncKeyState + hotkeys::parse_shortcut).

#include "blocks/block_registry.hpp"

namespace media::xblox::blocks {

void register_input_blocks(BlockRegistry& registry);

} // namespace media::xblox::blocks
