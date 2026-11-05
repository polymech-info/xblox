#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace media::xblox::blocks::json_query {

struct QueryOptions {
    bool string_fallback = false;
    bool collect_outputs = false;
};

bool evaluate(const nlohmann::json& input,
              const std::string& filter,
              nlohmann::json& result,
              std::string& err,
              QueryOptions options = {});

/// Collect jq path arrays for every match of `filter` (e.g. `.items[].name`).
/// Each path is a JSON array of string/number keys suitable for getpath/setpath.
/// Implemented as `[path(<filter>)]` under FEATURE_JQ.
bool collect_paths(const nlohmann::json& input,
                   const std::string& filter,
                   nlohmann::json& path_arrays,
                   std::string& err);

nlohmann::json normalize_iterable_value(const nlohmann::json& value, const std::string& mode);

} // namespace media::xblox::blocks::json_query
