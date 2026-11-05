#pragma once
//
// block_params_mapping  -  maps dynamic provider schema fields (Replicate
// OpenAPI etc.) to known xBlox ParamKinds via the reserved `x-xblox.kind`
// annotation. UI-only overlay: the property panel renders the matching
// native widget (file picker, prompt textarea, color input) instead of a
// plain text field. See docs/xblox/ui-schema.md  -  "Pattern: Kind Mapping".
//
#include <nlohmann/json.hpp>

namespace media::xblox::blocks::ui {

/// Annotate string-typed schema properties with `x-xblox.kind` hints.
/// Tiered matcher pipe: structural gate -> name-glob rules -> optional
/// embedding inference (when the strutil embed backend is available).
/// No-op when the local MAP_MODEL_PARAMS switch is off.
void annotate_schema_param_kinds(nlohmann::json& schema_props);

} // namespace media::xblox::blocks::ui
