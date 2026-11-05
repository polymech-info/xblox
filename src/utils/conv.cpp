#include "utils/conv.hpp"

namespace media::xblox::conv {

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

nlohmann::json event_to_json(const ExecutionEvent& event)
{
    nlohmann::json out = {
        {"path", event.path},
        {"kind", event.kind},
        {"status", event.status},
        {"message", event.message},
        {"exitCode", event.exit_code},
        {"data", event.data},
    };
    if (!event.block_id.empty())
        out["blockId"] = event.block_id;
    if (!event.type.empty())
        out["type"] = event.type;
    if (event.error_code != 0)
        out["errorCode"] = event.error_code;
    if (!event.stdout_lines.empty() || event.type == "command")
        out["stdout"] = event.stdout_lines;
    if (!event.stderr_lines.empty() || event.type == "command")
        out["stderr"] = event.stderr_lines;
    if (!event.outputs.empty()) {
        nlohmann::json outputs = nlohmann::json::array();
        for (const auto& output : event.outputs) {
            nlohmann::json item = {
                {"slot", output.slot},
                {"kind", output.kind},
                {"value", output.value},
                {"primary", output.primary},
            };
            if (!output.export_name.empty())
                item["exportName"] = output.export_name;
            outputs.push_back(std::move(item));
        }
        out["outputs"] = std::move(outputs);
    }
    return out;
}

} // namespace media::xblox::conv
