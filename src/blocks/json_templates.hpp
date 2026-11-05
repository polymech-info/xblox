#pragma once

#include <nlohmann/json.hpp>

namespace media::xblox::blocks {

class BlockIO;

nlohmann::json resolve_json_templates(BlockIO& io, const nlohmann::json& value);

} // namespace media::xblox::blocks
