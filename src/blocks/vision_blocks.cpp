#include "blocks/vision_blocks.hpp"

#include "core/local_model_catalog.hpp"
#include "llm/llama_vlm_client.hpp"
#include "string/string_filters.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

constexpr long long kVisionNormalizeStripThinking = 1;
constexpr long long kVisionNormalizeStripFences = 2;
constexpr long long kVisionNormalizeStripRoleLabels = 4;
constexpr long long kVisionNormalizeTrimOutput = 8;
constexpr long long kVisionNormalizeDefault =
    kVisionNormalizeStripThinking |
    kVisionNormalizeStripFences |
    kVisionNormalizeStripRoleLabels |
    kVisionNormalizeTrimOutput;

using media::strutil::lower_ascii;

bool cli_log_level_off(const BlockIO& io)
{
    static constexpr const char kLogLevelPrefix[] = "--log-level=";
    const auto& args = io.runtime().options.process_args;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--log-level") {
            if (i + 1 < args.size() && lower_ascii(args[i + 1]) == "off")
                return true;
            ++i; // skip value if present
        } else if (arg.rfind(kLogLevelPrefix, 0) == 0) {
            if (lower_ascii(arg.substr(sizeof(kLogLevelPrefix) - 1)) == "off")
                return true;
        }
    }
    return false;
}

std::vector<std::string> strings_from_param(BlockIO& io, const char* name)
{
    std::vector<std::string> out;
    if (const Value* v = io.value(name)) {
        const nlohmann::json j = v->to_json();
        if (j.is_array()) {
            for (const auto& item : j)
                if (item.is_string() && !item.get<std::string>().empty())
                    out.push_back(item.get<std::string>());
            return out;
        }
        if (j.is_string() && !j.get<std::string>().empty())
            out.push_back(j.get<std::string>());
        return out;
    }

    const nlohmann::json& raw = io.json(name);
    if (raw.is_array()) {
        for (const auto& item : raw)
            if (item.is_string() && !item.get<std::string>().empty())
                out.push_back(item.get<std::string>());
    } else if (raw.is_string() && !raw.get<std::string>().empty()) {
        out.push_back(raw.get<std::string>());
    }
    return out;
}

std::vector<std::string> vision_inputs(BlockIO& io)
{
    std::vector<std::string> inputs = strings_from_param(io, "input");
    const auto frames = strings_from_param(io, "frames");
    inputs.insert(inputs.end(), frames.begin(), frames.end());
    return inputs;
}

std::string preferred_vision_model()
{
    std::string fallback;
    for (const auto& m : media::local_models::discover_llama_vlm_models(false)) {
        bool has_vision = false;
        for (const auto& slot : m.slots)
            if (slot == "vision")
                has_vision = true;
        if (!has_vision)
            continue;

        const std::string id = m.id.empty() ? media::path::path_to_utf8(m.model_path) : m.id;
        if (fallback.empty())
            fallback = id;

        const std::string hay = lower_ascii(m.id + " " + m.display_name + " " + media::path::path_to_utf8(m.model_path));
        if (hay.find("minicpm-v") != std::string::npos)
            return id;
    }
    return fallback;
}

BlockResult vision_ask_block(BlockIO& io)
{
    std::vector<std::string> inputs = vision_inputs(io);
    if (inputs.empty()) {
        io.error("visionAsk", "visionAsk: provide input image path(s)", 1);
        return BlockResult::Error;
    }

    std::string prompt = io.str("prompt");
    if (prompt.empty())
        prompt = "Describe what you see in detail.";

    const std::string block_model = io.str("model");
    const std::string model = block_model.empty() ? preferred_vision_model() : block_model;
    if (model.empty()) {
        io.error("visionAsk", "visionAsk: no local vision model available; add MiniCPM GGUF + mmproj under models/ or pass model", 1);
        return BlockResult::Error;
    }

    if (io.cancelled())
        return BlockResult::Fatal;

    media::llm::vlm_client::VlmRequest req;
    req.image_paths = inputs;
    req.prompt = prompt;
    req.model_id = model;
    req.slot = lower_ascii(io.str("slot", "vision"));
    if (req.slot.empty())
        req.slot = "vision";
    req.n_predict = static_cast<int>(io.i64("maxTokens", 512));
    req.n_threads = static_cast<int>(io.i64("threads", 8));
    req.n_ctx = static_cast<int>(io.i64("ctx", inputs.size() > 1 ? 8192 : 4096));
    req.n_gpu_layers = static_cast<int>(io.i64("gpuLayers", -1));
    req.cancel_check = [&io] { return io.cancelled(); };
    req.stream_tokens = !cli_log_level_off(io);

    const auto res = media::llm::vlm_client::execute(req);
    if (io.cancelled())
        return BlockResult::Fatal;
    if (!res.ok) {
        io.error("visionAsk", "visionAsk: " + res.error, 1)
          .set("model", model)
          .set("inputs", Value::from_json(inputs));
        return BlockResult::Error;
    }

    const nlohmann::json& raw = io.raw();
    const long long normalizers = io.i64("normalizers", kVisionNormalizeDefault);
    media::llm::string_filters::GeneratedTextFilterOptions filter_options;
    filter_options.strip_thinking = (normalizers & kVisionNormalizeStripThinking) != 0;
    filter_options.strip_fences = (normalizers & kVisionNormalizeStripFences) != 0;
    filter_options.strip_role_labels = (normalizers & kVisionNormalizeStripRoleLabels) != 0;
    filter_options.trim_output = (normalizers & kVisionNormalizeTrimOutput) != 0;
    // Transitional tolerance for older in-progress documents that used boolean fields.
    if (raw.contains("stripThinking")) filter_options.strip_thinking = io.flag("stripThinking", filter_options.strip_thinking);
    if (raw.contains("stripFences")) filter_options.strip_fences = io.flag("stripFences", filter_options.strip_fences);
    if (raw.contains("stripRoleLabels")) filter_options.strip_role_labels = io.flag("stripRoleLabels", filter_options.strip_role_labels);
    if (raw.contains("trimOutput")) filter_options.trim_output = io.flag("trimOutput", filter_options.trim_output);
    const bool include_raw = io.flag("includeRaw", false);
    const auto filtered = media::llm::string_filters::filter_generated_text(res.text, filter_options);
    const std::string& text = filtered.text;

    nlohmann::json result{
        {"ok", true},
        {"provider", "local-vlm"},
        {"slot", req.slot},
        {"model", res.model_id.empty() ? model : res.model_id},
        {"modelPath", res.model_path},
        {"mmprojPath", res.mmproj_path},
        {"inputs", inputs},
        {"prompt", prompt},
        {"text", text},
        {"normalizers", {
            {"stripThinking", filter_options.strip_thinking},
            {"stripFences", filter_options.strip_fences},
            {"stripRoleLabels", filter_options.strip_role_labels},
            {"trimOutput", filter_options.trim_output},
            {"mask", normalizers},
        }},
        {"elapsedMs", res.elapsed_ms},
        {"nGpuLayers", res.n_gpu_layers},
    };
    if (include_raw)
        result["rawText"] = res.text;
    if (filtered.empty_after_filter)
        result["normalizedEmpty"] = true;

    const bool json_output = io.flag("json", true);
    io.ok("visionAsk", "vision answered for " + std::to_string(inputs.size()) + " image(s)")
      .set("model", result.value("model", std::string{}))
      .set("inputCount", static_cast<long long>(inputs.size()))
      .set("elapsedMs", res.elapsed_ms)
      .payload(result)
      .result(json_output ? Value::from_json(result) : Value(text),
              json_output ? ParamKind::json_value : ParamKind::string_);
    return BlockResult::Ok;
}

} // namespace

void register_vision_blocks(BlockRegistry& registry)
{
    const std::vector<ParamDef> params = {
        pd_image("input").in().prev().globs().dflt("")
            .desc("Image path, glob, or PREVIOUS. Glob arrays are passed as multiple images."),
        pd_json("frames").in().grp("source_model").dflt(nlohmann::json::array())
            .desc("Additional image paths for multi-frame or comparison prompts."),
        pd_model().grp("source_model").lbl("Model").dflt("")
            .ui({{"control", "select"}, {"options_path", "vision.models.vlm"}, {"empty_label", "(auto MiniCPM vision)"}})
            .desc("Local vision VLM model id/path. Empty = prefer MiniCPM-V from downloaded models."),
        pd_enum("slot", {"vision", "image", "video"}).grp("source_model").dflt("vision")
            .desc("Local model catalog slot used for auto-selection."),
        pd_prompt("prompt").in().grp("prompt").dflt("Describe what you see in detail.")
            .desc("Question or instruction for the vision model."),
        pd_bool("json").grp("output").dflt(true)
            .desc("When true, result is structured JSON; when false, result is only generated text."),
        pd_flags("normalizers").grp("output_normalization").dflt(kVisionNormalizeDefault)
            .desc("Post-process generated text for common model-output artifacts.")
            .flag(kVisionNormalizeStripThinking, "Strip thinking", "Remove reasoning sections such as <think>...</think>, including unclosed sections.")
            .flag(kVisionNormalizeStripFences, "Strip fences", "Unwrap a single outer Markdown/code fence around the whole answer.")
            .flag(kVisionNormalizeStripRoleLabels, "Strip role labels", "Remove common leading/trailing assistant role or template markers.")
            .flag(kVisionNormalizeTrimOutput, "Trim output", "Trim leading and trailing whitespace after other normalizers."),
        pd_bool("includeRaw").grp("output").dflt(false)
            .desc("Include rawText in JSON output for debugging before normalizers were applied."),
        pd_int("threads").grp("runtime").dflt(8).range(1, 64)
            .desc("CPU thread count for llama.cpp."),
        pd_int("gpuLayers").grp("runtime").dflt(-1).range(-1, 999)
            .desc("GPU layers to offload. -1 = all available, 0 = CPU-only."),
        pd_int("ctx").adv().grp("runtime").dflt(4096).range(1024, 65536)
            .desc("Context size. Use 8192+ for multi-frame prompts."),
        pd_int("maxTokens").grp("runtime").dflt(512).range(1, 8192)
            .desc("Maximum generated tokens."),
        pd_json("result").out().desc("Structured vision response when json=true."),
        pd_store_as().dflt("vision"),
    };

    ExecFlags exec;
    exec.supports_background = true;
    exec.supports_cancel = true;
    exec.is_blocking = true;
    exec.min_duration_hint_ms = 2000;

    registry["visionAsk"] = bdb(
        "visionAsk", vision_ask_block, "Vision Ask", "Vision",
        "Ask a local MiniCPM-style VLM about one or more images.",
        {{"kind","visionAsk"}, {"input",""}, {"frames", nlohmann::json::array()}, {"model",""},
         {"slot","vision"}, {"prompt","Describe what you see in detail."}, {"json",true},
         {"normalizers", kVisionNormalizeDefault}, {"includeRaw",false},
         {"threads",8}, {"gpuLayers",-1}, {"ctx",4096}, {"maxTokens",512}, {"storeAs","vision"},
         {"items", nlohmann::json::array()}})
        .params(params)
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::json_value, ParamKind::string_},
            "Vision result")
        .children({{"items", "Then", "body", {"*"}, 0, -1}})
        .exec(exec);

    registry["visionDescribe"] = bdb(
        "visionDescribe", vision_ask_block, "Vision Describe", "Vision",
        "Describe one or more images with a local MiniCPM-style VLM.",
        {{"kind","visionDescribe"}, {"input",""}, {"prompt","Describe what you see in detail."},
         {"json",true}, {"normalizers", kVisionNormalizeDefault},
         {"includeRaw",false}, {"storeAs","vision"}, {"items", nlohmann::json::array()}})
        .params(params)
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::json_value, ParamKind::string_},
            "Vision result")
        .children({{"items", "Then", "body", {"*"}, 0, -1}})
        .exec(exec)
        .hidden();
}

} // namespace media::xblox::blocks
