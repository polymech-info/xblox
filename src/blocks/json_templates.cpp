#include "blocks/json_templates.hpp"

#include "blocks/block_variables.hpp"
#include "blocks/block_types.hpp"
#include "string/string_utils.hpp"

#include <string>

namespace media::xblox::blocks {
namespace {

std::string json_template_scalar(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_null())
        return {};
    return value.dump();
}

} // namespace

nlohmann::json resolve_json_templates(BlockIO& io, const nlohmann::json& value)
{
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : value)
            out.push_back(resolve_json_templates(io, item));
        return out;
    }
    if (value.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[it.key()] = resolve_json_templates(io, it.value());
        return out;
    }
    if (!value.is_string())
        return value;

    const std::string input = value.get<std::string>();

    if (const auto whole = media::strutil::whole_variable_token(input)) {
        if (const nlohmann::json* resolved = io.context(*whole))
            return *resolved;
    }

    bool changed = false;
    const std::string output = media::strutil::substitute_variables(
        input,
        [&](std::string_view name) -> std::optional<std::string> {
            if (const nlohmann::json* resolved = io.context(std::string(name)))
                return json_template_scalar(*resolved);
            return std::nullopt;
        },
        &changed);

    if (media::strutil::has_variable_tokens(output)) {
        const std::string resolved = bv::resolve(output, io.runtime());
        if (resolved != output)
            return nlohmann::json(resolved);
    }
    return changed ? nlohmann::json(output) : value;
}

} // namespace media::xblox::blocks
