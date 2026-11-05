#include "xblox_json.hpp"

#include "xblox_expressions.hpp"
#include "xblox_strings.hpp"

#include <utility>

namespace media::xblox {

std::string json_string(const nlohmann::json& object, const char* key)
{
    return object.is_object() && object.contains(key) && object[key].is_string()
        ? object[key].get<std::string>()
        : std::string{};
}

std::vector<std::string> json_string_array(const nlohmann::json& object, const char* key)
{
    std::vector<std::string> out;
    if (!object.is_object() || !object.contains(key) || !object[key].is_array())
        return out;
    for (const auto& item : object[key]) {
        if (item.is_string())
            out.push_back(item.get<std::string>());
    }
    return out;
}

bool json_bool_or(const nlohmann::json& object, const char* key, bool fallback)
{
    if (!object.is_object() || !object.contains(key) || !object[key].is_boolean())
        return fallback;
    return object[key].get<bool>();
}

bool json_number(const nlohmann::json& value, double& out)
{
    if (value.is_number()) {
        out = value.get<double>();
        return true;
    }
    if (value.is_boolean()) {
        out = value.get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (value.is_string()) {
        try {
            size_t consumed = 0;
            const std::string s = value.get<std::string>();
            out = std::stod(s, &consumed);
            return consumed == s.size();
        } catch (...) {
            return false;
        }
    }
    return false;
}

nlohmann::json* context_value_ptr(nlohmann::json& context, const std::string& path)
{
    return expr::resolve_reference(context, path);
}

const nlohmann::json* context_value_ptr(const nlohmann::json& context, const std::string& path)
{
    return expr::resolve_reference(context, path);
}

void set_context_value(nlohmann::json& context, const std::string& path, nlohmann::json value)
{
    if (path.find('.') == std::string::npos) {
        if (!context.is_object())
            context = nlohmann::json::object();
        context[path] = std::move(value);
        return;
    }
    const auto parts = split_path(path);
    if (parts.empty())
        return;
    if (!context.is_object())
        context = nlohmann::json::object();
    nlohmann::json* cur = &context;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        auto& child = (*cur)[parts[i]];
        if (!child.is_object())
            child = nlohmann::json::object();
        cur = &child;
    }
    (*cur)[parts.back()] = std::move(value);
}

nlohmann::json store_value_from_result(const nlohmann::json& result)
{
    if (result.is_object() && result.contains("result"))
        return result["result"];
    return result;
}

} // namespace media::xblox
