#pragma once
// builtin_blocks.hpp  -  umbrella over the split block-runtime headers.
//
// Historically this was one ~1000-line header. It is now split by concern; this
// file just re-exports them so existing includes keep working. Prefer including
// the specific concern header when you only need part of the surface:
//
//   block_types.hpp      core vocabulary: BlockRuntime, Value, Diagnostic,
//                        ResolvedInputs, EventBuilder, BlockIO, BlockHandler
//   block_validation.hpp ParamConstraint preconditions (+ json)
//   block_params.hpp     ParamDef + pd_* factories
//   block_resolver.hpp   source_param, BlockIO::def/resolved, resolve_block_inputs
//   block_registry.hpp   ExecFlags, ChildListDef, BlockDescriptor, bdb(), lookups
//   parameter_operations.hpp declarative inline binding-transform registry
//   block_variables.hpp  bv::resolve  -  ${var} interpolation (incl. app-level vars)
//
// Include order matters (each builds on the previous); keep it.
//
// See docs/xblox-next.md §10.49 for the split rationale.

#include "blocks/block_types.hpp"
#include "blocks/block_validation.hpp"
#include "blocks/block_params.hpp"
#include "blocks/block_resolver.hpp"
#include "blocks/block_registry.hpp"
#include "blocks/parameter_operations.hpp"
