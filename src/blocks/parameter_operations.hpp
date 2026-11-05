#pragma once
// parameter_operations.hpp - declarative registry for inline parameter transforms.
//
// Parameter operations are binding-time value transforms, not executable blocks:
// they do not participate in document ordering, emit events, or perform I/O.
// This first slice intentionally registers metadata only. Evaluator dispatch is
// added later; documents that reference a metadata-only operation fail with the
// deterministic `unsupported_parameter_operation` diagnostic.

#include "blocks/block_types.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace media::xblox::blocks {

enum class ParameterOperationOutputRule {
    Fixed,
    SameAsInput,
    PromoteInputs,
    Inferred,
};

inline const char* parameter_operation_output_rule_str(ParameterOperationOutputRule rule) noexcept
{
    switch (rule) {
    case ParameterOperationOutputRule::Fixed:         return "fixed";
    case ParameterOperationOutputRule::SameAsInput:   return "sameAsInput";
    case ParameterOperationOutputRule::PromoteInputs: return "promoteInputs";
    case ParameterOperationOutputRule::Inferred:      return "inferred";
    default:                                          return "fixed";
    }
}

struct ParameterOperationInputDef {
    std::string id; // Stable port id: "in-1", "in-2", ...
    std::string label;
    std::vector<ParamKind> supported_kinds;
    bool required = true;
    int min_connections = 1;
    int max_connections = 1; // -1 = unlimited
    bool accepts_literal = true;
    bool accepts_binding = true;

    nlohmann::json to_json() const
    {
        nlohmann::json kinds = nlohmann::json::array();
        for (const ParamKind kind : supported_kinds)
            kinds.push_back(param_kind_str(kind));
        nlohmann::json j = {
            {"id", id},
            {"supportedKinds", std::move(kinds)},
            {"required", required},
            {"minConnections", min_connections},
            {"maxConnections", max_connections},
            {"acceptsLiteral", accepts_literal},
            {"acceptsBinding", accepts_binding},
        };
        if (!label.empty()) j["label"] = label;
        return j;
    }
};

struct ParameterOperationOutputDef {
    ParameterOperationOutputRule rule = ParameterOperationOutputRule::Fixed;
    ParamKind fixed_kind = ParamKind::json_value;
    std::string input; // Source input for SameAsInput.
    std::vector<ParamKind> supported_kinds;

    nlohmann::json to_json() const
    {
        nlohmann::json supported = nlohmann::json::array();
        for (const ParamKind kind : supported_kinds)
            supported.push_back(param_kind_str(kind));
        nlohmann::json j = {
            {"rule", parameter_operation_output_rule_str(rule)},
            {"supportedKinds", std::move(supported)},
        };
        if (rule == ParameterOperationOutputRule::Fixed)
            j["kind"] = param_kind_str(fixed_kind);
        if (!input.empty())
            j["input"] = input;
        return j;
    }
};

struct ParameterOperationDef {
    std::string id;      // Namespaced stable id, e.g. "core.numeric".
    std::string owner;   // Contribution/extension owner.
    int version = 1;
    std::string label;
    std::string description;
    std::map<std::string, ParameterOperationInputDef> inputs;
    std::vector<std::string> variants;
    ParameterOperationOutputDef output;
    bool pure = true;
    bool deterministic = true;
    bool bounded = true;
    bool evaluator_available = false;

    nlohmann::json to_json() const
    {
        nlohmann::json input_map = nlohmann::json::object();
        for (const auto& [key, input] : inputs)
            input_map[key] = input.to_json();
        nlohmann::json j = {
            {"id", id},
            {"owner", owner},
            {"version", version},
            {"inputs", std::move(input_map)},
            {"variants", variants},
            {"output", output.to_json()},
            {"pure", pure},
            {"deterministic", deterministic},
            {"bounded", bounded},
            {"available", evaluator_available},
        };
        if (!label.empty())       j["label"] = label;
        if (!description.empty()) j["description"] = description;
        return j;
    }
};

class ParameterOperationRegistry {
public:
    bool add(ParameterOperationDef def, std::string* error = nullptr)
    {
        if (def.id.empty() || def.id.find('.') == std::string::npos) {
            if (error) *error = "parameter operation id must be non-empty and namespaced";
            return false;
        }
        if (def.version <= 0) {
            if (error) *error = "parameter operation version must be positive";
            return false;
        }
        for (const auto& [key, input] : def.inputs) {
            if (key.empty() || input.id.empty() || key != input.id) {
                if (error) *error = "parameter operation input map key must match its non-empty id";
                return false;
            }
            if (input.min_connections < 0 ||
                (input.max_connections != -1 && input.max_connections < input.min_connections)) {
                if (error) *error = "parameter operation input connection bounds are invalid";
                return false;
            }
        }
        const auto [_, inserted] = definitions_.emplace(def.id, std::move(def));
        if (!inserted && error)
            *error = "duplicate parameter operation id";
        return inserted;
    }

    const ParameterOperationDef* find(const std::string& id) const
    {
        const auto it = definitions_.find(id);
        return it == definitions_.end() ? nullptr : &it->second;
    }

    nlohmann::json to_manifest() const
    {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& [_, def] : definitions_)
            out.push_back(def.to_json());
        return out;
    }

private:
    // std::map keeps manifest output deterministic across platforms.
    std::map<std::string, ParameterOperationDef> definitions_;
};

inline ParameterOperationRegistry& parameter_operation_registry()
{
    static ParameterOperationRegistry registry;
    return registry;
}

} // namespace media::xblox::blocks
