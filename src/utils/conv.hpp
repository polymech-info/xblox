#pragma once

#include "xblox_commands.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::xblox::conv {

std::string json_string(const nlohmann::json& object, const char* key);
std::vector<std::string> json_string_array(const nlohmann::json& object, const char* key);
nlohmann::json event_to_json(const ExecutionEvent& event);

} // namespace media::xblox::conv
