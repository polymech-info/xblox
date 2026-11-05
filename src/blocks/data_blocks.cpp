#include "blocks/data_blocks.hpp"
#include "blocks/json_query.hpp"

#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

std::string json_string(const nlohmann::json& o, const char* key)
{
    return o.is_object() && o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string{};
}

std::string json_string_any(const nlohmann::json& o, std::initializer_list<const char*> keys, const std::string& fallback)
{
    if (o.is_object()) {
        for (const char* key : keys) {
            if (o.contains(key) && o[key].is_string() && !o[key].get<std::string>().empty())
                return o[key].get<std::string>();
        }
    }
    return fallback;
}

using media::strutil::lower_ascii;

/* ── Parse block handler ─────────────────────────────────────────────────── */

bool parse_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};

    const std::string parser = lower_ascii(io.str("parser", "jq"));
    if (parser != "jq") {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "Parse", "error", "unsupported parser: " + parser, 2});
        return true;
    }

    const std::string filter   = io.str_any({"filter", "query"}, ".");
    const std::string input_name = json_string_any(block, {"input", "from"}, "PREVIOUS");
    const nlohmann::json* input = io.context(input_name);
    if (!input) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "Parse", "error", "parse input is missing: " + input_name, 2});
        return true;
    }

    std::string err;
    nlohmann::json result;
    if (!json_query::evaluate(*input, filter, result, err)) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "Parse", "error", "jq parse failed: " + err, 2});
        return true;
    }

    // Emit first while result is still a valid lvalue (event gets a copy of
    // result), then move result into set_result to avoid the parameter copy.
    if (runtime.events_enabled) {
        runtime.emit(runtime.user_data, ExecutionEvent{
            path,
            "Parse",
            "ok",
            "",
            0,
            {{"parser", parser}, {"filter", filter}, {"input", input_name}, {"result", result}},
            runtime.block_id,
            "data",
        });
    }
    io.set_result(std::move(result), ParamKind::json_value);
    return true;
}

bool iterator_block(const nlohmann::json& block, const std::string& path, BlockRuntime& runtime)
{
    BlockIO io{block, path, runtime};

    const std::string parser = lower_ascii(io.str("parser", "jq"));
    if (parser != "jq") {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "iterator", "error", "unsupported parser: " + parser, 2});
        return true;
    }

    const std::string input_name = json_string_any(block, {"input", "from"}, "PREVIOUS");
    const nlohmann::json* input = io.context(input_name);
    if (!input) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "iterator", "error", "iterator input is missing: " + input_name, 2});
        return true;
    }

    const std::string filter = io.str_any({"filter", "query"}, ".");
    nlohmann::json selected;
    std::string err;
    if (!json_query::evaluate(*input, filter, selected, err, {.string_fallback = true, .collect_outputs = true})) {
        runtime.emit(runtime.user_data, ExecutionEvent{path, "iterator", "error", "iterator jq failed: " + err, 2});
        return true;
    }

    const std::string mode = io.str("mode", "auto");
    nlohmann::json result = json_query::normalize_iterable_value(selected, mode);
    const auto count = static_cast<long long>(result.size());
    io.set_result(result, ParamKind::json_value);

    runtime.emit(runtime.user_data, ExecutionEvent{
        path,
        "iterator",
        "ok",
        "iterator prepared " + std::to_string(count) + " item(s)",
        0,
        {{"parser", parser}, {"filter", filter}, {"input", input_name}, {"mode", mode}, {"count", count}, {"result", result}},
        runtime.block_id,
        "data",
    });
    return true;
}

} // namespace

BlockHandler data_block_handler(const std::string& kind)
{
    if (kind == "Parse" || kind == "parse")
        return legacy(parse_block);
    if (kind == "iterator")
        return legacy(iterator_block);
    return nullptr;
}

void register_data_blocks(BlockRegistry& registry)
{
    const nlohmann::json params = nlohmann::json::array({
        {{"name", "parser"}, {"type", "string"}, {"default", "jq"}},
        {{"name", "filter"}, {"type", "string"}, {"default", "."}},
        {{"name", "input"}, {"type", "string"}, {"default", "PREVIOUS"}},
        {{"name", "storeAs"}, {"type", "string"}},
    });
    const std::vector<ParamDef> parse_params = {
        pd_enum("parser", {"jq","jsonpath"}).opt().dflt("jq").desc("Parser engine."),
        pd_str("filter").in().req().dflt(".").desc("jq filter expression (e.g. .items[] | select(.active))."),
        pd_str("input").in().dflt("PREVIOUS")
            .desc("Input to parse. Leave as PREVIOUS to use the previous block's result."),
        pd_store_as().dflt("parsed"),
    };
    registry["Parse"] = bdb(
        "Parse", parse_block, "Parse JSON", "Data",
        "Parse PREVIOUS or named input with a jq filter expression.",
        {{"kind","Parse"},{"parser","jq"},{"filter","."},{"storeAs","parsed"}})
        .params(parse_params)
        .pure()
        .primary_output(ParamKind::json_value, "Parsed value")
        .children({{"items", "Then", "body", {"*"}, 0, -1}});
    registry["parse"] = bdb(
        "parse", parse_block, "Parse", "Data",
        "Parse PREVIOUS or named input with a jq filter expression.",
        {{"kind","parse"},{"parser","jq"},{"filter","."},{"storeAs","parsed"}})
        .params(parse_params)
        .pure()
        .primary_output(ParamKind::json_value, "Parsed value")
        .hidden();

    registry["iterator"] = bdb(
        "iterator", iterator_block, "Iterator", "Data",
        "Iterate over PREVIOUS or any named scope/context variable. Arrays fan out directly, objects become {key,value} entries, scalars run once.",
        {{"kind","iterator"},{"parser","jq"},{"filter","."},{"input","PREVIOUS"},{"mode","auto"},{"storeAs","items"},{"items", nlohmann::json::array()}})
        .params({
            pd_enum("parser", {"jq","jsonpath"}).opt().dflt("jq").desc("Parser engine."),
            pd_str("filter").in().req().dflt(".").desc("jq filter expression to select the value to iterate."),
            pd_str("input").in().dflt("PREVIOUS")
                .desc("Scope/context variable to iterate. Leave as PREVIOUS to use the previous block's result."),
            pd_enum("mode", {"auto","array","objectEntries","objectValues","once"}).grp("iteration").dflt("auto")
                .desc("How to normalize the selected value before fan-out."),
            pd_store_as().dflt("items"),
        })
        .primary_output(ParamKind::json_value, "Iterable")
        .children({{"items", "For each", "body", {"*"}, 0, -1}})
        .pure()
        .iterable();
}

} // namespace media::xblox::blocks
