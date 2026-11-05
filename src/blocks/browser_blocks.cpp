#include "blocks/browser_blocks.hpp"

#include "blocks/block_params.hpp"
#include "blocks/block_registry.hpp"
#include "llm/tools/browser_use/Tool_BrowserUse.hpp"

#include <string>
#include <vector>

namespace media::xblox::blocks {
namespace {

struct BrowserBlockSpec {
    const char* kind;
    const char* label;
    const char* description;
    BlockHandler handler;
    nlohmann::json (*default_block)();
    std::vector<ParamDef> (*params)();
};

void add_target_args(BlockIO& io, nlohmann::json& args)
{
    std::string ref = io.str("ref");
    std::string epoch = io.str("epoch");
    const std::string selector = io.str("selector");
    if (const Value* target = io.value("target")) {
        const nlohmann::json value = target->to_json();
        if (value.is_object()) {
            if (epoch.empty() && value.contains("epoch") && value["epoch"].is_string())
                epoch = value["epoch"].get<std::string>();
            const nlohmann::json data = value.value("data", nlohmann::json::object());
            if (ref.empty() && data.is_object()) {
                if (data.contains("ref") && data["ref"].is_string())
                    ref = data["ref"].get<std::string>();
                else if (data.contains("items") && data["items"].is_array()
                         && !data["items"].empty() && data["items"][0].is_object())
                    ref = data["items"][0].value("ref", std::string{});
            }
        }
    }
    if (!ref.empty()) args["ref"] = ref;
    if (!epoch.empty()) args["epoch"] = epoch;
    if (!selector.empty()) args["selector"] = selector;
}

BlockResult run_browser_tool(BlockIO& io, const char* tool)
{
    nlohmann::json args = nlohmann::json::object();
    add_target_args(io, args);
    const std::string name(tool);
    if (name == "browser_read") {
        args["limit"] = io.i64("limit", 120);
        args["viewport_only"] = io.flag("viewportOnly", false);
        args["text_max_chars"] = io.i64("textMaxChars", 12000);
    } else if (name == "browser_find") {
        const std::string role = io.str("role");
        const std::string accessible_name = io.str("name");
        if (!role.empty()) args["role"] = role;
        if (!accessible_name.empty()) args["name"] = accessible_name;
        args["limit"] = io.i64("limit", 20);
    } else if (name == "browser_type") {
        args["text"] = io.str("text");
        args["clear"] = io.flag("clear", true);
        args["submit"] = io.flag("submit", false);
    } else if (name == "browser_select") {
        const std::string value = io.str("value");
        const std::string label = io.str("label");
        if (!value.empty()) args["value"] = value;
        if (!label.empty()) args["label"] = label;
        if (io.raw().contains("index")) args["index"] = io.i64("index", 0);
    } else if (name == "browser_batch") {
        if (const Value* steps = io.value("steps"))
            args["steps"] = steps->to_json();
        const std::string epoch = io.str("epoch");
        if (!epoch.empty()) args["epoch"] = epoch;
        if (io.raw().contains("defaultDelayMs"))
            args["default_delay_ms"] = io.i64("defaultDelayMs", 50);
        if (io.raw().contains("continueOnError"))
            args["continue_on_error"] = io.flag("continueOnError", false);
    }

    auto result = media::llm::browser_use::execute(name, args);
    const std::string kind = io.str("kind", name);
    const std::string id = io.str("id");
    if (!result.ok) {
        io.error(kind, result.error.empty() ? "browser-use failed" : result.error, 1)
          .id(id).type("browser").payload(result.envelope);
        return BlockResult::Error;
    }
    io.ok(kind, "ok").id(id).type("browser")
      .payload(result.envelope)
      .result(Value::from_json(result.envelope), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult browser_read_block(BlockIO& io)   { return run_browser_tool(io, "browser_read"); }
BlockResult browser_find_block(BlockIO& io)   { return run_browser_tool(io, "browser_find"); }
BlockResult browser_click_block(BlockIO& io)  { return run_browser_tool(io, "browser_click"); }
BlockResult browser_type_block(BlockIO& io)   { return run_browser_tool(io, "browser_type"); }
BlockResult browser_select_block(BlockIO& io) { return run_browser_tool(io, "browser_select"); }
BlockResult browser_batch_block(BlockIO& io)  { return run_browser_tool(io, "browser_batch"); }

nlohmann::json browser_default(const char* kind)
{
    return {
        {"kind", kind},
        {"selector", ""},
        {"ref", ""},
        {"epoch", ""},
        {"storeAs", ""},
    };
}

nlohmann::json read_default()
{
    auto out = browser_default("browserRead");
    out["limit"] = 120;
    out["viewportOnly"] = false;
    return out;
}
nlohmann::json find_default()
{
    auto out = browser_default("browserFind");
    out["role"] = "";
    out["name"] = "";
    out["limit"] = 20;
    return out;
}
nlohmann::json click_default() { return browser_default("browserClick"); }
nlohmann::json type_default()
{
    auto out = browser_default("browserType");
    out["text"] = "";
    out["clear"] = true;
    out["submit"] = false;
    return out;
}
nlohmann::json select_default()
{
    auto out = browser_default("browserSelect");
    out["value"] = "";
    out["label"] = "";
    return out;
}
nlohmann::json batch_default()
{
    return {
        {"kind", "browserBatch"},
        {"epoch", ""},
        {"steps", nlohmann::json::array()},
        {"defaultDelayMs", 50},
        {"continueOnError", false},
        {"storeAs", ""},
    };
}

std::vector<ParamDef> target_params()
{
    return {
        pd_json("target").in().prev().dflt(nullptr)
            .desc("Previous Browser Read/Find result; extracts epoch and the first ref."),
        pd_str("ref").in().dflt("").desc("Opaque ref returned by Browser Read/Find."),
        pd_str("epoch").in().dflt("").desc("Page epoch returned with the ref."),
        pd_str("selector").in().dflt("").desc("CSS selector alternative to ref."),
    };
}

std::vector<ParamDef> read_params()
{
    auto params = target_params();
    params.push_back(pd_int("limit").opt().dflt(120).range(1, 500));
    params.push_back(pd_bool("viewportOnly").opt().dflt(false));
    params.push_back(pd_int("textMaxChars").adv().dflt(12000).range(0, 100000));
    params.push_back(pd_store_as());
    return params;
}

std::vector<ParamDef> find_params()
{
    auto params = target_params();
    params.push_back(pd_str("role").in().dflt(""));
    params.push_back(pd_str("name").in().dflt(""));
    params.push_back(pd_int("limit").opt().dflt(20).range(1, 100));
    params.push_back(pd_store_as());
    return params;
}

std::vector<ParamDef> click_params()
{
    auto params = target_params();
    params.push_back(pd_store_as());
    return params;
}

std::vector<ParamDef> type_params()
{
    auto params = target_params();
    params.push_back(pd_str("text").in().dflt("").desc("Text to enter."));
    params.push_back(pd_bool("clear").opt().dflt(true));
    params.push_back(pd_bool("submit").opt().dflt(false));
    params.push_back(pd_store_as());
    return params;
}

std::vector<ParamDef> select_params()
{
    auto params = target_params();
    params.push_back(pd_str("value").in().dflt(""));
    params.push_back(pd_str("label").in().dflt(""));
    params.push_back(pd_int("index").in().desc("Zero-based option index alternative."));
    params.push_back(pd_store_as());
    return params;
}

std::vector<ParamDef> batch_params()
{
    return {
        pd_str("epoch").in().dflt("").desc("Optional page epoch from Browser Read."),
        pd_json("steps").in().dflt(nlohmann::json::array())
            .desc("Ordered step objects with action/type plus step fields."),
        pd_int("defaultDelayMs").opt().dflt(50).range(0, 5000),
        pd_bool("continueOnError").opt().dflt(false),
        pd_store_as(),
    };
}

const std::vector<BrowserBlockSpec>& browser_block_specs()
{
#if defined(_WIN32) && defined(FEATURE_BROWSER) && FEATURE_BROWSER \
    && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT \
    && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE \
    && defined(FEATURE_TOOLS_APP_USE) && FEATURE_TOOLS_APP_USE
    static const std::vector<BrowserBlockSpec> specs = {
        {"browserRead", "Browser Read", "Read a compact index or focused DOM snapshot.",
         browser_read_block, read_default, read_params},
        {"browserFind", "Browser Find", "Find WebView elements by selector, role, or name.",
         browser_find_block, find_default, find_params},
        {"browserClick", "Browser Click", "Click a WebView element by ref or selector.",
         browser_click_block, click_default, click_params},
        {"browserType", "Browser Type", "Enter text into a WebView form control.",
         browser_type_block, type_default, type_params},
        {"browserSelect", "Browser Select", "Choose a WebView select option.",
         browser_select_block, select_default, select_params},
        {"browserBatch", "Browser Batch", "Run multiple WebView actions in one call.",
         browser_batch_block, batch_default, batch_params},
    };
#else
    static const std::vector<BrowserBlockSpec> specs;
#endif
    return specs;
}

} // namespace

BlockHandler browser_block_handler(const std::string& kind)
{
    for (const auto& spec : browser_block_specs())
        if (kind == spec.kind)
            return spec.handler;
    return nullptr;
}

void register_browser_blocks(BlockRegistry& registry)
{
    for (const auto& spec : browser_block_specs()) {
        registry[spec.kind] = bdb(
            spec.kind,
            spec.handler,
            spec.label,
            "Browser",
            spec.description,
            spec.default_block())
            .params(spec.params())
            .exec({false, false, true})
            .primary_output(ParamKind::json_value, "Browser result");
    }
}

} // namespace media::xblox::blocks
