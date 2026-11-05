#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::xblox {

std::string json_string(const nlohmann::json& object, const char* key);
std::vector<std::string> json_string_array(const nlohmann::json& object, const char* key);
bool json_bool_or(const nlohmann::json& object, const char* key, bool fallback);
bool json_number(const nlohmann::json& value, double& out);
nlohmann::json* context_value_ptr(nlohmann::json& context, const std::string& path);
const nlohmann::json* context_value_ptr(const nlohmann::json& context, const std::string& path);
void set_context_value(nlohmann::json& context, const std::string& path, nlohmann::json value);
nlohmann::json store_value_from_result(const nlohmann::json& result);

} // namespace media::xblox
