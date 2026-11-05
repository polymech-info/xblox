// XBlox image blocks  -  imageResize, imageCreate, imageTransform, imageUnderstand,
//                        imageScreenshot, imageAnalyze, imageFind.
//
// These wrap the same core library APIs used by the CLI:
//   � resize_batch      (src/core/resize.*)
//   � create_image      (src/core/transform.*)
//   � transform_image   (src/core/transform.*)
//   � meta_extract      (src/core/meta.*)          � imageUnderstand
//   � capture_still*    (src/core/video.*, src/core/screen_input.*)
//
// PREVIOUS / storeAs contract:
//   � imageResize       -> output image path string, or array of paths for batches
//   � imageCreate       -> generated output image path string
//   � imageTransform    -> output image path string, or array of rows for batches
//   � imageUnderstand   -> vision answer text, or array of per-image rows for batches
//   � imageScreenshot   -> output image path string
//   � imageAnalyze      -> JSON array of quality rows (fingerprint later, same decode)
//   � imageFind         -> JSON array of matches (junk / name; duplicates later)

#include "blocks/image_blocks.hpp"

#include "core/batch_queue.hpp"
#include "core/find.hpp"
#include "fs/glob_paths.hpp"
#include "core/meta.hpp"
#include "core/resize.hpp"
#include "core/settings_runtime.hpp"
#include "core/transform.hpp"
#include "llm/llama_vlm_client.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "logger/logger.h"
#include "win/overlay_hud.hpp"

#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include "core/video.hpp"
#include "core/screen_input.hpp"
#include "core/host_backends.hpp"
#include "video/video_picker_input_spec.hpp"
#include "win/display_affinity.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::xblox::blocks {
namespace {

using media::strutil::lower_ascii;

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

std::string join_semicolons(const std::vector<std::string>& values)
{
    std::string out;
    for (const auto& value : values) {
        if (value.empty()) continue;
        if (!out.empty()) out.push_back(';');
        out += value;
    }
    return out;
}

void apply_image_ai_defaults(std::string& provider, std::string& model)
{
    if (provider.empty() || model.empty()) {
        std::vector<media::runtime_settings::ChatProviderPreset> presets;
        std::string err;
        if (media::runtime_settings::load_chat_provider_presets(presets, err)) {
            const media::runtime_settings::ChatProviderPreset* def = nullptr;
            for (const auto& p : presets) {
                if (p.is_default) {
                    def = &p;
                    break;
                }
            }
            if (!def) {
                for (const auto& p : presets) {
                    const std::string id = lower_ascii(p.id);
                    const std::string name = lower_ascii(p.name);
                    if (id == "default" || name == "default") {
                        def = &p;
                        break;
                    }
                }
            }
            if (!def && presets.size() == 1)
                def = &presets.front();
            if (def) {
                if (provider.empty()) provider = def->chat.image_provider;
                if (model.empty()) model = def->chat.image_model;
            }
        }
    }

    if (provider.empty() || model.empty()) {
        media::runtime_settings::ChatProviderSettings chat;
        std::string err;
        if (media::runtime_settings::load_chat_provider(chat, err)) {
            if (provider.empty()) provider = chat.image_provider;
            if (model.empty()) model = chat.image_model;
        }
    }
}

std::string path_leaf(const std::string& path)
{
    const auto name = media::path::path_to_utf8(media::path::path_from_utf8(path).filename());
    return name.empty() ? path : name;
}

std::unique_ptr<media::win::OverlayJob> maybe_overlay(BlockIO& io, std::string prefix)
{
    if (!io.flag("hud", false))
        return {};
    return std::make_unique<media::win::OverlayJob>(std::move(prefix));
}

void apply_image_recognition_defaults(std::string& provider, std::string& model)
{
    media::llm::apply_chat_image_recognition_defaults_for_tool_options(
        provider, model, nlohmann::json::object());
    media::llm::repair_meta_google_model_if_replicate_slug(provider, model);
}

media::MetaOptions meta_options_from_io(BlockIO& io)
{
    media::MetaOptions opts;
    opts.provider = io.str("provider");
    opts.model = io.str("model");
    apply_image_recognition_defaults(opts.provider, opts.model);

    opts.api_key = io.str("apiKey");
    opts.base_url = io.str("baseUrl");
    opts.prompt = io.str("prompt");
    opts.resize_first = io.flag("resizeFirst", true);
    opts.resize_width = static_cast<int>(io.i64("resizeWidth", 512));
    opts.out_md = false;
    opts.out_json = false;
    opts.update_exif = false;

    if (!opts.provider.empty())
        media::runtime_settings::merge_provider_credentials(opts.provider, false, opts.api_key, opts.base_url);
    return opts;
}

media::TransformOptions transform_options_from_io(BlockIO& io)
{
    media::TransformOptions opts;
    opts.provider.clear();
    opts.model.clear();
    opts.provider = io.str("provider");
    opts.model = io.str("model");
    apply_image_ai_defaults(opts.provider, opts.model);

    opts.api_key = io.str("apiKey");
    opts.base_url = io.str("baseUrl");
    opts.prompt = io.str("prompt");
    {
        const nlohmann::json& po = io.json("providerOptions");
        if (po.is_object() && !po.empty())
            opts.provider_options = po;
    }
    opts.aspect_ratio = io.str("aspectRatio");
    opts.image_size = io.str("imageSize");
    opts.reference_images = strings_from_param(io, "references");
    opts.resize_first = io.flag("resizeFirst", false);
    opts.resize_width = static_cast<int>(io.i64("resizeWidth", 0));
    opts.preresize_raw_only = io.flag("preresizeRawOnly", false);

    if (!opts.provider.empty())
        media::runtime_settings::merge_provider_credentials(opts.provider, false, opts.api_key, opts.base_url);
    return opts;
}

media::ResizeOptions resize_options_from_io(BlockIO& io)
{
    media::ResizeOptions opt;
    opt.max_width = static_cast<int>(io.i64("maxWidth", 0));
    opt.max_height = static_cast<int>(io.i64("maxHeight", 0));
    opt.aspect = io.str("aspect");
    opt.format = io.str("format");
    opt.fit = io.str("fit", "inside");
    opt.position = io.str("position", "centre");
    opt.kernel = io.str("kernel", "lanczos3");
    opt.background = io.str("background", "#ffffff");
    opt.quality = static_cast<int>(io.i64("quality", 85));
    opt.png_compression = static_cast<int>(io.i64("pngCompression", 6));
    opt.rotate = static_cast<int>(io.i64("rotate", 0));
    opt.flip = io.flag("flip", false);
    opt.flop = io.flag("flop", false);
    opt.auto_rotate = io.flag("autoRotate", false);
    opt.autorotate = io.flag("autorotate", true);
    opt.strip_metadata = io.flag("stripMetadata", true);
    opt.without_enlargement = !io.flag("allowEnlargement", false);
    opt.cache_enabled = io.flag("cache", true);
    opt.cache_dir = io.str("cacheDir");
    opt.url_timeout_sec = static_cast<int>(io.i64("urlTimeoutSec", 5));
    opt.url_max_redirects = static_cast<int>(io.i64("urlMaxRedirects", 20));
    opt.output_stem_suffix = io.str("suffix");
    return opt;
}

nlohmann::json transform_row(const media::TransformResult& result, const std::string& source)
{
    nlohmann::json row{
        {"kind", "image"},
        {"path", result.output_path},
    };
    if (!source.empty()) row["source"] = source;
    if (!result.ai_text.empty()) row["text"] = result.ai_text;
    if (!result.replicate_prediction_web_url.empty())
        row["predictionUrl"] = result.replicate_prediction_web_url;
    return row;
}

BlockResult image_resize_block(BlockIO& io)
{
    std::vector<std::string> inputs = strings_from_param(io, "input");
    if (inputs.empty()) {
        io.error("imageResize", "imageResize: provide input image path(s)", 1);
        return BlockResult::Error;
    }

    if (io.cancelled())
        return BlockResult::Fatal;

    const std::string input_spec = join_semicolons(inputs);
    const std::string output = io.str("outputPath");
    const media::ResizeOptions opt = resize_options_from_io(io);
    media::ResizeBatchResult batch;
    std::string err;
    const bool ok = media::resize_batch(input_spec, output, opt, err, &batch,
        [&](std::size_t index, std::size_t total) {
            if (total > 1)
                io.info("imageResize: " + std::to_string(index + 1) + "/" + std::to_string(total));
        });

    if (!ok) {
        io.error("imageResize", "imageResize: " + err, 1)
          .set("input", input_spec)
          .set("outputPath", output);
        return BlockResult::Error;
    }

    nlohmann::json result = batch.outputs.size() == 1
        ? nlohmann::json(batch.outputs.front())
        : nlohmann::json(batch.outputs);

    io.ok("imageResize", "resized " + std::to_string(batch.count) + " image(s)")
      .set("count", static_cast<long long>(batch.count))
      .set("outputs", Value::from_json(batch.outputs))
      .result(Value::from_json(result), batch.outputs.size() == 1 ? ParamKind::image_path : ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult image_create_block(BlockIO& io)
{
    media::TransformOptions opts = transform_options_from_io(io);
    if (opts.provider.empty() || opts.model.empty()) {
        io.error("imageCreate", "imageCreate: provider/model required (set block fields or App Settings)", 1)
          .set("provider", opts.provider)
          .set("model", opts.model);
        return BlockResult::Error;
    }
    if (opts.prompt.empty()) {
        io.error("imageCreate", "imageCreate: prompt is required", 1);
        return BlockResult::Error;
    }
    if (io.cancelled())
        return BlockResult::Fatal;

    media::BatchControl batch;
    auto overlay = maybe_overlay(io, "Create");
    if (overlay) {
        overlay->set_on_stop([&batch] { batch.request_cancel(); });
        overlay->busy(opts.model.empty() ? "working" : opts.model);
    }
    auto progress = [&](const std::string& msg) {
        io.info("imageCreate: " + msg);
        if (overlay)
            overlay->busy(msg.empty() ? (opts.model.empty() ? "working" : opts.model) : msg);
    };
    const media::TransformResult result =
        media::create_image(io.str("outputPath"), opts, progress, &batch, {});

    if (!result.ok) {
        if (io.cancelled() || (overlay && overlay->stop_requested())
            || result.error.find("cancelled") != std::string::npos) {
            batch.request_cancel();
            if (overlay)
                overlay->error("cancelled");
            return BlockResult::Fatal;
        }
        if (overlay)
            overlay->error(result.error);
        io.error("imageCreate", "imageCreate: " + result.error, 1);
        return BlockResult::Error;
    }

    if (overlay)
        overlay->completed(path_leaf(result.output_path));
    io.ok("imageCreate", "created image: " + result.output_path)
      .set("path", result.output_path)
      .set("provider", opts.provider)
      .set("model", opts.model)
      .payload(transform_row(result, {}))
      .result(result.output_path, ParamKind::image_path);
    return BlockResult::Ok;
}

BlockResult image_transform_block(BlockIO& io)
{
    std::vector<std::string> inputs = strings_from_param(io, "input");
    if (inputs.empty()) {
        io.error("imageTransform", "imageTransform: provide input image path(s)", 1);
        return BlockResult::Error;
    }

    const std::string output = io.str("outputPath");
    if (inputs.size() > 1 && !output.empty()) {
        io.error("imageTransform", "imageTransform: outputPath is only valid for a single input", 1);
        return BlockResult::Error;
    }

    media::TransformOptions opts = transform_options_from_io(io);
    if (opts.provider.empty() || opts.model.empty()) {
        io.error("imageTransform", "imageTransform: provider/model required (set block fields or App Settings)", 1)
          .set("provider", opts.provider)
          .set("model", opts.model);
        return BlockResult::Error;
    }
    if (opts.prompt.empty()) {
        io.error("imageTransform", "imageTransform: prompt is required", 1);
        return BlockResult::Error;
    }

    media::BatchControl batch;
    auto overlay = maybe_overlay(io, "Transform");
    if (overlay) {
        overlay->set_on_stop([&batch] { batch.request_cancel(); });
        overlay->busy(opts.model.empty() ? "working" : opts.model);
    }
    nlohmann::json outputs = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();
    int ok_count = 0;
    int fail_count = 0;

    for (size_t i = 0; i < inputs.size(); ++i) {
        const std::string& input = inputs[i];
        if (io.cancelled() || (overlay && overlay->stop_requested())) {
            batch.request_cancel();
            if (overlay)
                overlay->error("cancelled");
            return BlockResult::Fatal;
        }

        if (overlay) {
            if (inputs.size() > 1)
                overlay->progress(static_cast<uint64_t>(i), static_cast<uint64_t>(inputs.size()),
                                  path_leaf(input));
            else
                overlay->busy(path_leaf(input));
        }

        const std::string one_output = inputs.size() == 1 ? output : std::string{};
        auto progress = [&](const std::string& msg) {
            io.info("imageTransform: " + msg);
            if (overlay)
                overlay->busy(msg.empty() ? path_leaf(input) : msg);
        };
        const media::TransformResult result =
            media::transform_image(input, one_output, opts, progress, &batch, {});

        if (!result.ok) {
            if (io.cancelled() || (overlay && overlay->stop_requested())
                || result.error.find("cancelled") != std::string::npos) {
                batch.request_cancel();
                if (overlay)
                    overlay->error("cancelled");
                return BlockResult::Fatal;
            }
            errors.push_back({{"source", input}, {"error", result.error}});
            ++fail_count;
            continue;
        }

        outputs.push_back(transform_row(result, input));
        ++ok_count;
    }

    if (fail_count > 0) {
        if (overlay)
            overlay->error(std::to_string(fail_count) + " failed");
        io.error("imageTransform", "imageTransform: " + std::to_string(fail_count) + " image(s) failed", 1)
          .set("ok", static_cast<long long>(ok_count))
          .set("failed", static_cast<long long>(fail_count))
          .set("errors", Value::from_json(errors));
        return BlockResult::Error;
    }

    nlohmann::json result;
    if (outputs.size() == 1)
        result = outputs.front().value("path", std::string{});
    else
        result = outputs;
    if (overlay)
        overlay->completed(std::to_string(ok_count) + " done");
    io.ok("imageTransform", "transformed " + std::to_string(ok_count) + " image(s)")
      .set("count", static_cast<long long>(ok_count))
      .set("outputs", Value::from_json(outputs))
      .result(Value::from_json(result), outputs.size() == 1 ? ParamKind::image_path : ParamKind::json_value);
    return BlockResult::Ok;
}

nlohmann::json understand_row(const std::string& path,
                              const std::string& answer,
                              const std::string& provider,
                              const std::string& model,
                              const nlohmann::json& extra = nlohmann::json::object())
{
    nlohmann::json row{
        {"kind", "understand"},
        {"path", path},
        {"answer", answer},
        {"provider", provider},
        {"model", model},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it)
        row[it.key()] = it.value();
    return row;
}

BlockResult image_understand_block(BlockIO& io)
{
    std::vector<std::string> inputs = strings_from_param(io, "input");
    if (inputs.empty()) {
        io.error("imageUnderstand", "imageUnderstand: provide input image path(s)", 1);
        return BlockResult::Error;
    }

    const std::string prompt = io.str("prompt");
    if (prompt.empty()) {
        io.error("imageUnderstand", "imageUnderstand: prompt is required", 1);
        return BlockResult::Error;
    }

    media::MetaOptions opts = meta_options_from_io(io);
    if (opts.provider.empty() || opts.model.empty()) {
        io.error("imageUnderstand",
                 "imageUnderstand: recognition provider/model required (set block fields or App Settings)",
                 1)
          .set("provider", opts.provider)
          .set("model", opts.model);
        return BlockResult::Error;
    }

    if (io.cancelled())
        return BlockResult::Fatal;

    auto overlay = maybe_overlay(io, "Understand");
    if (overlay)
        overlay->busy(opts.model.empty() ? "working" : opts.model);

    const bool json_output = io.flag("json", false);
    nlohmann::json outputs = nlohmann::json::array();
    nlohmann::json errors = nlohmann::json::array();
    int ok_count = 0;
    int fail_count = 0;

    if (opts.provider == "llama") {
        if (!media::llm::vlm_client::is_vlm_available("vision") && opts.model.empty()) {
            io.error("imageUnderstand",
                     "imageUnderstand: no local vision VLM available; add a VLM with the 'vision' slot or pass model",
                     1);
            return BlockResult::Error;
        }

        media::llm::vlm_client::VlmRequest req;
        req.image_paths = inputs;
        req.prompt = prompt;
        req.model_id = opts.model;
        req.slot = "vision";
        req.n_predict = static_cast<int>(io.i64("maxTokens", 2048));
        req.n_threads = static_cast<int>(io.i64("threads", 4));
        req.n_ctx = static_cast<int>(io.i64("ctx", inputs.size() > 1 ? 8192 : 4096));
        req.n_gpu_layers = static_cast<int>(io.i64("gpuLayers", -1));
        req.cancel_check = [&io, &overlay] {
            return io.cancelled() || (overlay && overlay->stop_requested());
        };

        const auto res = media::llm::vlm_client::execute(req);
        if (io.cancelled() || (overlay && overlay->stop_requested())) {
            if (overlay)
                overlay->error("cancelled");
            return BlockResult::Fatal;
        }
        if (!res.ok) {
            if (overlay)
                overlay->error(res.error);
            io.error("imageUnderstand", "imageUnderstand: " + res.error, 1);
            return BlockResult::Error;
        }

        const std::string model = res.model_id.empty() ? opts.model : res.model_id;
        nlohmann::json extra{
            {"elapsedMs", res.elapsed_ms},
            {"nGpuLayers", res.n_gpu_layers},
        };
        for (const std::string& input : inputs) {
            outputs.push_back(understand_row(input, res.text, "llama", model, extra));
            ++ok_count;
        }
    } else {
        media::llm::ensure_api_key_for_image_provider(opts.provider, opts.api_key);
        media::llm::ensure_base_url_for_image_provider(opts.provider, opts.base_url);

        for (size_t i = 0; i < inputs.size(); ++i) {
            const std::string& input = inputs[i];
            if (io.cancelled() || (overlay && overlay->stop_requested())) {
                if (overlay)
                    overlay->error("cancelled");
                return BlockResult::Fatal;
            }

            if (overlay) {
                if (inputs.size() > 1)
                    overlay->progress(static_cast<uint64_t>(i), static_cast<uint64_t>(inputs.size()),
                                      path_leaf(input));
                else
                    overlay->busy(path_leaf(input));
            }

            auto progress = [&](const std::string& msg) {
                io.info("imageUnderstand: " + msg);
                if (overlay)
                    overlay->busy(msg.empty() ? path_leaf(input) : msg);
            };
            const media::UnderstandResult result = media::image_understand(input, opts, progress);
            if (!result.ok) {
                errors.push_back({{"source", input}, {"error", result.error}});
                ++fail_count;
                continue;
            }
            outputs.push_back(understand_row(input, result.text, opts.provider, opts.model, {}));
            ++ok_count;
        }
    }

    if (fail_count > 0) {
        if (overlay)
            overlay->error(std::to_string(fail_count) + " failed");
        io.error("imageUnderstand", "imageUnderstand: " + std::to_string(fail_count) + " image(s) failed", 1)
          .set("ok", static_cast<long long>(ok_count))
          .set("failed", static_cast<long long>(fail_count))
          .set("errors", Value::from_json(errors));
        return BlockResult::Error;
    }

    nlohmann::json result;
    if (outputs.size() == 1) {
        result = json_output
            ? outputs.front()
            : nlohmann::json(outputs.front().value("answer", std::string{}));
    } else {
        result = outputs;
    }

    if (overlay)
        overlay->completed(std::to_string(ok_count) + " done");
    io.ok("imageUnderstand", "understood " + std::to_string(ok_count) + " image(s)")
      .set("count", static_cast<long long>(ok_count))
      .set("provider", opts.provider)
      .set("model", opts.model)
      .set("outputs", Value::from_json(outputs))
      .result(Value::from_json(result),
              outputs.size() == 1 && !json_output ? ParamKind::string_ : ParamKind::json_value);
    return BlockResult::Ok;
}

std::vector<ParamDef> ai_image_params(bool include_input)
{
    std::vector<ParamDef> params;
    if (include_input) {
        params.push_back(pd_image("input").in().prev().globs().dflt("")
            .desc("Input image path or glob. Uses PREVIOUS when unset."));
    }
    params.push_back(pd_prompt("prompt").in().req()
        .desc("Prompt for image generation/editing."));
    params.push_back(pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
        .desc("Output path. Empty = core default naming."));
    params.push_back(pd_provider().grp("model").dflt("")
        .ui({{"options_path", "providers.image"}})
        .desc("Image generation provider. Empty = App Settings image provider."));
    params.push_back(pd_model().grp("model").dflt("")
        .dyn({
            {"kind",                  "options_list"},
            {"options_path_template", "providers.replicate.models.{replicateCollection}"},
            {"provider_match",        "replicate"},
            {"provider_field",        "provider"},
            {"options_method",        "xbloxUiOptionsGet"},
        })
        .desc("Image generation model. Empty = App Settings image model."));
    params.push_back(pd_str("replicateCollection").grp("model").lbl("Collection").dflt("official")
        .ui({
            {"control",      "select"},
            {"options_path", "providers.replicate.collections"},
            {"empty_label",  "official"},
            {"visible_when", {{"provider", "replicate"}}},
        })
        .desc("Replicate collection for browsing models. Only relevant when provider = replicate."));
    params.push_back(pd_api_key().grp("auth").dflt(""));
    params.push_back(pd_str("baseUrl").adv().grp("auth").dflt("")
        .desc("Provider base URL override. Empty = provider default/App Settings."));
    params.push_back(pd_enum("aspectRatio", {"", "1:1", "16:9", "9:16", "4:3", "3:4", "21:9"}).grp("request").dflt("")
        .desc("Aspect ratio shortcut  -  not all providers use this; for Replicate use providerOptions instead."));
    params.push_back(pd_enum("imageSize", {"", "512", "1K", "2K", "4K"}).grp("request").dflt("")
        .desc("Size shortcut  -  not all providers use this; for Replicate use providerOptions instead."));
    params.push_back(pd_args("references").grp("request").dflt(nlohmann::json::array())
        .ui({{"picks_path", true}})
        .desc("Reference image paths (one per row). Each supports the file picker."));
    params.push_back(pd_bool("resizeFirst").adv().grp("pre_resize").dflt(false)
        .desc("Pre-resize input/reference raster before sending to the provider."));
    params.push_back(pd_int("resizeWidth").adv().grp("pre_resize").dflt(0).range(0, 8192)
        .desc("Longest edge for pre-resize when resizeFirst is on. 0 = provider/default."));
    params.push_back(pd_bool("preresizeRawOnly").adv().grp("pre_resize").dflt(false)
        .desc("When pre-resizing, only force RAW/HEIC rasters through the explicit long-edge resize."));
    params.push_back(pd_json("providerOptions").adv().grp("model").lbl("Model options")
        .dflt(nlohmann::json::object())
        .dyn({
            {"kind",          "provider_options"},
            {"provider_field","provider"},
            {"model_field",   "model"},
            {"schema_path",   "providers.replicate"},
            {"schema_method", "xbloxProviderOptionsSchemaGet"},
            {"value_field",   "providerOptions"},
        })
        .desc("Replicate model-specific input fields (seed, num_inference_steps, ...). Resolved per model via OpenAPI cache."));
    params.push_back(pd_bool("hud").grp("hud").dflt(false)
        .desc("Show the movable overlay HUD while this block runs. Default off."));
    params.push_back(pd_store_as().dflt("imagePath"));
    return params;
}

std::vector<ParamDef> ai_understand_params()
{
    return {
        pd_image("input").in().prev().globs().req()
            .desc("Input image path or glob. Uses PREVIOUS when unset. "
                  "Pass multiple images for comparison prompts."),
        pd_prompt("prompt").in().req()
            .desc("Vision question or instruction for the recognition model."),
        pd_provider().grp("model").dflt("")
            .ui({{"options_path", "providers.vision"}})
            .desc("Image recognition provider. Empty = App Settings image recognition provider."),
        pd_model().grp("model").dflt("")
            .dyn({
                {"kind",                  "options_list"},
                {"options_path_template", "providers.replicate.models.{replicateCollection}"},
                {"provider_match",        "replicate"},
                {"provider_field",        "provider"},
                {"options_method",        "xbloxUiOptionsGet"},
            })
            .desc("Image recognition model. Empty = App Settings image recognition model. "
                  "For provider=llama, local VLMs come from vision.models.vlm."),
        pd_str("replicateCollection").grp("model").lbl("Collection").dflt("official")
            .ui({
                {"control",      "select"},
                {"options_path", "providers.replicate.collections"},
                {"empty_label",  "official"},
                {"visible_when", {{"provider", "replicate"}}},
            })
            .desc("Replicate collection for browsing vision models. Only relevant when provider = replicate."),
        pd_api_key().grp("auth").dflt(""),
        pd_str("baseUrl").adv().grp("auth").dflt("")
            .desc("Provider base URL override. Empty = provider default/App Settings."),
        pd_bool("resizeFirst").adv().grp("pre_resize").dflt(true)
            .desc("Pre-resize images before upload (default true, matching image_understand)."),
        pd_int("resizeWidth").adv().grp("pre_resize").dflt(512).range(64, 2048)
            .desc("Longest edge for pre-resize when resizeFirst is on."),
        pd_bool("json").grp("output").dflt(false)
            .desc("When true, result is structured JSON; when false, result is answer text only."),
        pd_int("threads").adv().grp("runtime").dflt(4).range(1, 64)
            .desc("CPU threads for local VLM (provider=llama)."),
        pd_int("gpuLayers").adv().grp("runtime").dflt(-1).range(-1, 999)
            .desc("GPU layers for local VLM. -1 = all available, 0 = CPU-only."),
        pd_int("ctx").adv().grp("runtime").dflt(4096).range(1024, 65536)
            .desc("Context size for local VLM. Use 8192+ for multi-image prompts."),
        pd_int("maxTokens").adv().grp("runtime").dflt(2048).range(1, 32768)
            .desc("Maximum generated tokens for local VLM."),
        pd_bool("hud").grp("hud").dflt(false)
            .desc("Show the movable overlay HUD while this block runs. Default off."),
        pd_store_as().dflt("answer"),
    };
}

// -- imageScreenshot ------------------------------------------------------------
// Captures a single still image from a screen monitor, window, or webcam and
// saves it to disk. The input source string follows the same spec as videoDetect
// and is typically provided by a Picker block:
//
//   screen:0                        primary monitor
//   screen:1                        second monitor
//   screen:0:wintitle=Chrome*       window whose title matches glob
//   screen:0:hwnd=<decimal>         specific window handle
//   screen:0:rect=x,y,w,h           fixed screen rectangle
//   <empty> | <device-name>         webcam / capture device
//
// PREVIOUS / storeAs ? output image file path (image_path).

#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO
inline void screenshot_ensure_com() noexcept {
    static thread_local bool s_done = false;
    if (!s_done) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        s_done = true;
    }
}

// Copies a captured VideoFrame to the system clipboard as CF_DIB (24-bit BGR).
// Bottom-up DIB row order is required by the CF_DIB clipboard format.
static bool clipboard_copy_bitmap(const pm::video::VideoFrame& frame, std::string& err)
{
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) {
        err = "empty frame"; return false;
    }
    const bool is_rgb = (frame.format == pm::video::PixelFormat::RGB24);
    if (!is_rgb && frame.format != pm::video::PixelFormat::BGR24) {
        err = "clipboard copy requires BGR24 or RGB24 frame"; return false;
    }

    const int w          = frame.width;
    const int h          = frame.height;
    const int src_stride = frame.stride > 0 ? frame.stride : w * 3;
    const int dst_stride = (w * 3 + 3) & ~3;   // DWORD-aligned rows

    const std::size_t dib_size =
        sizeof(BITMAPINFOHEADER) + static_cast<std::size_t>(dst_stride) * h;
    HGLOBAL hMem = ::GlobalAlloc(GHND, dib_size);
    if (!hMem) { err = "GlobalAlloc failed"; return false; }

    auto* bmi = static_cast<BITMAPINFOHEADER*>(::GlobalLock(hMem));
    if (!bmi) { ::GlobalFree(hMem); err = "GlobalLock failed"; return false; }

    std::memset(bmi, 0, sizeof(BITMAPINFOHEADER));
    bmi->biSize     = sizeof(BITMAPINFOHEADER);
    bmi->biWidth    = w;
    bmi->biHeight   = h;    // positive = bottom-up (standard CF_DIB)
    bmi->biPlanes   = 1;
    bmi->biBitCount = 24;
    bmi->biCompression = BI_RGB;

    auto*       dst = reinterpret_cast<uint8_t*>(bmi + 1);
    const auto* src = frame.data.data();
    for (int y = 0; y < h; ++y) {
        const auto* src_row = src + (h - 1 - y) * src_stride;   // bottom-up flip
        auto*       dst_row = dst + y * dst_stride;
        if (is_rgb) {
            for (int x = 0; x < w; ++x) {
                dst_row[x*3 + 0] = src_row[x*3 + 2];
                dst_row[x*3 + 1] = src_row[x*3 + 1];
                dst_row[x*3 + 2] = src_row[x*3 + 0];
            }
        } else {
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(w) * 3);
        }
    }
    ::GlobalUnlock(hMem);

    if (!::OpenClipboard(nullptr)) {
        ::GlobalFree(hMem);
        err = "OpenClipboard failed: " + std::to_string(::GetLastError());
        return false;
    }
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_DIB, hMem)) {
        err = "SetClipboardData(CF_DIB) failed: " + std::to_string(::GetLastError());
        ::CloseClipboard();
        ::GlobalFree(hMem);
        return false;
    }
    ::CloseClipboard();
    return true;
}
#endif

#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO
std::string utf8_from_wide(const std::wstring& ws)
{
    if (ws.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

bool screenshot_run_picker(BlockIO& io, std::string& spec, std::string& err,
                           pm::video::VideoFrame* live_hud_frame)
{
    media::win::inspect::PickOptions opts;
    opts.highlight = io.flag("highlight", true);
    opts.hud = io.flag("hud", true);
    opts.resolve_uia = io.flag("resolveUia", true);
    opts.show_hidden = io.flag("showHidden", true);
    opts.siblings = io.flag("siblings", false);
    opts.magnifier = io.flag("magnifier", false);
    opts.chrome_dom = false;
    opts.resolve_cmdline = false;
    const bool include_open = io.flag("includeOpenWindows", true);
    const bool hud_visible = io.str("hudCapture", "auto") == "visible";
    const int timeout_ms = static_cast<int>(io.i64("pickTimeoutMs", 60000));

    media::win::inspect::PickResult pick;
    if (hud_visible && live_hud_frame) {
        opts.before_teardown = [&](const media::win::inspect::PickResult& committed) {
            const std::string live_spec = pm::video::video_input_spec_from_pick(
                committed, utf8_from_wide(committed.title), include_open);
            std::string cap_err;
            *live_hud_frame = pm::video::capture_still_screen(
                live_spec, cap_err, true, true);
        };
    }

    auto fut = std::async(std::launch::async, [&]() {
        return pm::host::inspect::pick_window(opts, pick, err);
    });
    const auto status = fut.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        INPUT esc[2]{};
        esc[0].type = INPUT_KEYBOARD;
        esc[0].ki.wVk = VK_ESCAPE;
        esc[1] = esc[0];
        esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(2, esc, sizeof(INPUT));
        fut.wait();
        err = "picker timed out after " + std::to_string(timeout_ms / 1000) + "s";
        return false;
    }
    if (!fut.get()) {
        if (err.empty())
            err = "picker failed";
        return false;
    }
    if (pick.cancelled) {
        err = "picker cancelled";
        return false;
    }
    spec = pm::video::video_input_spec_from_pick(
        pick, utf8_from_wide(pick.title), include_open);
    return !spec.empty();
}
#endif

BlockResult image_screenshot_block(BlockIO& io)
{
#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO
    screenshot_ensure_com();

    std::string input_spec = io.str("input");
    const std::string output_path = io.str("outputPath");
    const bool copy_clipboard = io.flag("copyToClipboard", false);
    const bool picker_hud_visible = io.str("hudCapture", "auto") == "visible";
    const std::string pick_mode = lower_ascii(io.str("pick", "off"));

    pm::video::VideoFrame frame;
    pm::video::VideoFrame live_hud;
    std::string cap_err;
#if defined(_WIN32)
    media::win::PickerHudCaptureScope picker_hud(picker_hud_visible);
#endif
    if (pick_mode == "interactive") {
        std::string pick_err;
        if (!screenshot_run_picker(io, input_spec, pick_err, &live_hud)) {
            io.error("imageScreenshot",
                     "imageScreenshot: " + (pick_err.empty() ? std::string("pick failed")
                                                             : pick_err),
                     1);
            return BlockResult::Error;
        }
        if (live_hud.width > 0)
            frame = std::move(live_hud);
    } else if (pick_mode != "off" && !pick_mode.empty()) {
        io.error("imageScreenshot",
                 "imageScreenshot: pick must be off or interactive", 1)
          .set("pick", pick_mode);
        return BlockResult::Error;
    }

    const bool is_screen = !input_spec.empty() && input_spec.rfind("screen:", 0) == 0;
    if (frame.width <= 0) {
        if (is_screen) {
            frame = pm::video::capture_still_screen(
                input_spec, cap_err, true, picker_hud_visible);
        } else {
            const int timeout_ms = static_cast<int>(io.i64("timeoutMs", 5000));
            try {
                frame = pm::video::capture_still(
                    input_spec, 0, 0, timeout_ms, [&] { return io.cancelled(); });
            } catch (const std::exception& ex) {
                io.error("imageScreenshot",
                         std::string("imageScreenshot: capture failed: ") + ex.what(), 1);
                return BlockResult::Error;
            }
        }
    }
    if (frame.width <= 0) {
        io.error("imageScreenshot",
                 "imageScreenshot: capture failed" +
                 (cap_err.empty() ? std::string{} : ": " + cap_err), 1);
        return BlockResult::Error;
    }

    // Save to file (optional)
    if (!output_path.empty()) {
        {
            std::error_code ec;
            std::filesystem::create_directories(
                media::path::path_from_utf8(output_path).parent_path(), ec);
        }
        std::string save_err;
        if (!pm::video::save_frame(frame, output_path, save_err)) {
            io.error("imageScreenshot", "imageScreenshot: save failed: " + save_err, 1);
            return BlockResult::Error;
        }
    }

    // Copy to clipboard (optional)
    if (copy_clipboard) {
        std::string clip_err;
        if (!clipboard_copy_bitmap(frame, clip_err)) {
            io.error("imageScreenshot",
                     "imageScreenshot: clipboard copy failed: " + clip_err, 1);
            return BlockResult::Error;
        }
    }

    const std::string msg = output_path.empty()
        ? (copy_clipboard ? "screenshot copied to clipboard" : "screenshot captured")
        : "screenshot saved: " + output_path;
    io.ok("imageScreenshot", msg)
      .set("path",   output_path)
      .set("width",  static_cast<long long>(frame.width))
      .set("height", static_cast<long long>(frame.height))
      .set("input",  input_spec)
      .result(output_path, ParamKind::image_path);
    return BlockResult::Ok;
#else
    io.error("imageScreenshot", "imageScreenshot requires Win32 + FEATURE_VIDEO", 1);
    return BlockResult::Error;
#endif
}

void apply_junk_tune_from_io(BlockIO& io, media::ImageQualityOptions& q)
{
    q.edge_energy_blur_hi = io.f64("edgeEnergyBlur", q.edge_energy_blur_hi);
    q.edge_energy_p90_blur_hi = io.f64("edgeEnergyP90Blur", q.edge_energy_p90_blur_hi);
    q.edge_energy_p99_blur_hi = io.f64("edgeEnergyP99Blur", q.edge_energy_p99_blur_hi);
    q.edge_orient_blur_lo = io.f64("edgeOrientBlur", q.edge_orient_blur_lo);
    q.edge_density_content_lo = io.f64("edgeDensityLo", q.edge_density_content_lo);
    q.analyze_max_side = static_cast<int>(io.i64("analyzeMaxSide", q.analyze_max_side));
    q.tiny_min_side = static_cast<int>(io.i64("tinyMinSide", q.tiny_min_side));
    q.edges.threshold = static_cast<int>(io.i64("edgeThreshold", q.edges.threshold));
}

nlohmann::json find_match_json(const media::FindMatch& m)
{
    nlohmann::json row = {
        {"path", m.path},
        {"score", m.score},
        {"source", m.source},
        {"reason", m.reason},
    };
    if (!m.details.empty())
        row["quality"] = m.details;
    return row;
}

media::FindResult run_junk_or_name_find(BlockIO& io, const std::string& mode)
{
    media::FindOptions opts;
    if (mode == "name")
        opts.mode = media::FindMode::Name;
    else
        opts.mode = media::FindMode::Junk;
    opts.prompt = io.str("prompt");
    opts.recursive = io.flag("recursive", true);
    opts.junk_min_score = io.f64("minScore", mode == "name" ? 0.0 : 0.7);
    opts.junk_kinds = io.str("junkKinds");
    opts.max_results = static_cast<int>(io.i64("maxResults", 0));
    apply_junk_tune_from_io(io, opts.junk_quality);

    const std::vector<std::string> inputs = strings_from_param(io, "input");
    return media::find_images(inputs, opts, [&](const std::string& msg) {
        io.info("imageFind: " + msg);
    });
}

BlockResult image_analyze_block(BlockIO& io)
{
    const std::vector<std::string> inputs = strings_from_param(io, "input");
    if (inputs.empty()) {
        io.error("imageAnalyze", "imageAnalyze: provide input image path(s)", 1);
        return BlockResult::Error;
    }
    if (io.cancelled())
        return BlockResult::Fatal;

    media::FindOptions opts;
    opts.mode = media::FindMode::Junk;
    opts.recursive = io.flag("recursive", true);
    opts.junk_min_score = io.f64("minScore", 0.0);
    opts.junk_kinds = io.str("junkKinds");
    opts.max_results = static_cast<int>(io.i64("maxResults", 0));
    apply_junk_tune_from_io(io, opts.junk_quality);

    const auto r = media::find_images(inputs, opts, [&](const std::string& msg) {
        io.info("imageAnalyze: " + msg);
    });
    if (!r.ok) {
        io.error("imageAnalyze", "imageAnalyze: " + r.error, 1);
        return BlockResult::Error;
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : r.matches) {
        nlohmann::json row = {{"path", m.path}};
        if (!m.details.empty())
            row["quality"] = m.details;
        row["junkScore"] = m.score;
        row["junkKind"] = m.source;
        arr.push_back(std::move(row));
    }

    io.ok("imageAnalyze", "analyzed " + std::to_string(r.scanned)
                            + ", kept " + std::to_string(r.matches.size()))
      .set("scanned", static_cast<long long>(r.scanned))
      .set("count", static_cast<long long>(r.matches.size()))
      .result(Value::from_json(arr), ParamKind::json_value);
    return BlockResult::Ok;
}

BlockResult image_find_block(BlockIO& io)
{
    const std::vector<std::string> inputs = strings_from_param(io, "input");
    if (inputs.empty()) {
        io.error("imageFind", "imageFind: provide input image path(s)", 1);
        return BlockResult::Error;
    }
    if (io.cancelled())
        return BlockResult::Fatal;

    const std::string mode = lower_ascii(io.str("mode", "junk"));
    if (mode != "junk" && mode != "name") {
        io.error("imageFind", "imageFind: mode must be junk or name (duplicates parked)", 1)
          .set("mode", mode);
        return BlockResult::Error;
    }
    if (mode == "name" && io.str("prompt").empty()) {
        io.error("imageFind", "imageFind: prompt is required for mode=name", 1);
        return BlockResult::Error;
    }

    const auto r = run_junk_or_name_find(io, mode);
    if (!r.ok) {
        io.error("imageFind", "imageFind: " + r.error, 1);
        return BlockResult::Error;
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : r.matches)
        arr.push_back(find_match_json(m));

    nlohmann::json paths = nlohmann::json::array();
    for (const auto& m : r.matches)
        paths.push_back(m.path);

    io.ok("imageFind", "matched " + std::to_string(r.matches.size())
                         + " / " + std::to_string(r.scanned))
      .set("scanned", static_cast<long long>(r.scanned))
      .set("count", static_cast<long long>(r.matches.size()))
      .set("mode", mode)
      .set("paths", Value::from_json(paths))
      .result(Value::from_json(arr), ParamKind::json_value);
    return BlockResult::Ok;
}

} // namespace

void register_image_blocks(BlockRegistry& registry)
{
    using J = nlohmann::json;

#if defined(FEATURE_COMMAND_RESIZE) && FEATURE_COMMAND_RESIZE
    registry["imageResize"] = bdb(
        "imageResize", image_resize_block,
        "Resize Image", "Image",
        "Resize/transform one or more image files using the same libvips pipeline as the resize CLI command.",
        {{"kind","imageResize"}, {"input",""}, {"outputPath",""}, {"maxWidth",0}, {"maxHeight",0},
         {"aspect",""}, {"format",""}, {"fit","inside"}, {"position","centre"}, {"kernel","lanczos3"},
         {"quality",85}, {"pngCompression",6}, {"background","#ffffff"}, {"rotate",0},
         {"autoRotate",false}, {"flip",false}, {"flop",false}, {"autorotate",true}, {"stripMetadata",true},
         {"allowEnlargement",false}, {"cache",true}, {"cacheDir",""}, {"suffix",""},
         {"urlTimeoutSec",5}, {"urlMaxRedirects",20}, {"storeAs","imagePath"}})
        .params({
            pd_image("input").in().prev().globs().req()
                .desc("Input image path or glob. Uses PREVIOUS when unset."),
            pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
                .desc("Output file, directory, or dst template. Empty = core default output path(s)."),
            pd_str("suffix").grp("output_file").dflt("").desc("Suffix for implicit output names, e.g. _small."),
            pd_int("maxWidth").grp("dimensions").dflt(0).range(0, 100000)
                .desc("Target/max width. 0 = unconstrained."),
            pd_int("maxHeight").grp("dimensions").dflt(0).range(0, 100000)
                .desc("Target/max height. 0 = unconstrained. Filled from aspect when width is set."),
            pd_str("aspect").grp("dimensions").dflt("")
                .desc("Target ratio W:H (4:5, 1:1, 16:9). Sets the unset side from maxWidth or maxHeight."),
            pd_enum("format", {"", "jpg", "jpeg", "png", "webp", "avif", "tif", "tiff", "heic"}).grp("format").dflt("")
                .desc("Output format. Empty = infer from output path/input."),
            pd_enum("fit", {"inside", "cover", "contain", "fill", "outside"}).grp("format").dflt("inside")
                .desc("Resize fit mode, matching Sharp/libvips semantics."),
            pd_str("position").adv().grp("format").dflt("centre")
                .desc("For cover: centre, attention, entropy, low, high, etc."),
            pd_enum("kernel", {"nearest", "cubic", "mitchell", "lanczos2", "lanczos3"}).adv().grp("format").dflt("lanczos3")
                .desc("Resize kernel."),
            pd_int("quality").adv().grp("codec").dflt(85).range(1, 100)
                .desc("JPEG/WebP/AVIF quality."),
            pd_int("pngCompression").adv().grp("codec").dflt(6).range(0, 9)
                .desc("PNG DEFLATE compression level."),
            pd_str("background").adv().grp("format").dflt("#ffffff")
                .desc("Letterbox color for contain, e.g. #ffffff."),
            pd_int("rotate").adv().grp("transform").dflt(0).range(0, 270)
                .desc("Rotate 0, 90, 180, or 270 degrees after autorotate."),
            pd_bool("autoRotate").adv().grp("transform").dflt(false)
                .desc("Rotate 90 when image orientation does not match the target box."),
            pd_bool("flip").adv().grp("transform").dflt(false).desc("Vertical flip."),
            pd_bool("flop").adv().grp("transform").dflt(false).desc("Horizontal flop."),
            pd_bool("autorotate").adv().grp("transform").dflt(true).desc("Apply EXIF orientation."),
            pd_bool("stripMetadata").adv().grp("codec").dflt(true).desc("Strip metadata on output where supported."),
            pd_bool("allowEnlargement").adv().grp("dimensions").dflt(false).desc("Allow upscaling."),
            pd_bool("cache").adv().grp("cache").dflt(true).desc("Enable resize output cache."),
            pd_dir("cacheDir").adv().grp("cache").dflt("").desc("Cache directory. Empty = core default."),
            pd_int("urlTimeoutSec").adv().grp("network").dflt(5).range(0, 3600).desc("HTTP(S) input timeout in seconds."),
            pd_int("urlMaxRedirects").adv().grp("network").dflt(20).range(0, 100).desc("Max HTTP redirects."),
            pd_store_as().dflt("imagePath"),
        })
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::image_path, ParamKind::json_value},
            "Resized image")
        .exec({false, true, true});
#endif

#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM \
 && defined(FEATURE_COMMAND_CREATE) && FEATURE_COMMAND_CREATE
    registry["imageCreate"] = bdb(
        "imageCreate", image_create_block,
        "Create Image", "Image",
        "Generate an image from a prompt using the same core create_image API as the CLI create command.",
        {{"kind","imageCreate"}, {"prompt",""}, {"outputPath",""}, {"provider",""}, {"model",""},
         {"replicateCollection","official"},
         {"apiKey",""}, {"baseUrl",""}, {"aspectRatio",""}, {"imageSize",""},
         {"references", J::array()}, {"resizeFirst",false}, {"resizeWidth",0},
         {"preresizeRawOnly",false}, {"hud",false}, {"storeAs","imagePath"}})
        .params(ai_image_params(false))
        .primary_output(ParamKind::image_path, "Created image")
        .exec({true, true, true});
#endif

#if defined(FEATURE_COMMAND_TRANSFORM) && FEATURE_COMMAND_TRANSFORM
    registry["imageTransform"] = bdb(
        "imageTransform", image_transform_block,
        "Transform Image", "Image",
        "Edit one or more images with an AI prompt using the same core transform_image API as the CLI transform command.",
        {{"kind","imageTransform"}, {"input",""}, {"prompt",""}, {"outputPath",""}, {"provider",""},
         {"model",""}, {"replicateCollection","official"},
         {"apiKey",""}, {"baseUrl",""}, {"aspectRatio",""}, {"imageSize",""},
         {"references", J::array()}, {"resizeFirst",false}, {"resizeWidth",0},
         {"preresizeRawOnly",false}, {"hud",false}, {"storeAs","imagePath"}})
        .params(ai_image_params(true))
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::image_path, ParamKind::json_value},
            "Transformed image")
        .exec({true, true, true});
#endif

#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM \
 && ((defined(FEATURE_COMMAND_META) && FEATURE_COMMAND_META) \
 || (defined(FEATURE_LLAMA) && FEATURE_LLAMA) \
 || (defined(FEATURE_PROVIDER_GOOGLE) && FEATURE_PROVIDER_GOOGLE) \
 || (defined(FEATURE_PROVIDER_OPENAI) && FEATURE_PROVIDER_OPENAI) \
 || (defined(FEATURE_PROVIDER_REPLICATE) && FEATURE_PROVIDER_REPLICATE) \
 || (defined(FEATURE_PROVIDER_TANIT) && FEATURE_PROVIDER_TANIT))
    registry["imageUnderstand"] = bdb(
        "imageUnderstand", image_understand_block,
        "Image Understand", "Image",
        "Ask the vision/recognition model a question about one or more images. "
        "Same pipeline as the image_understand agent tool: cloud via image_understand "
        "(plain markdown/text), local VLM via llama (provider=llama). Pure query, no sidecars.",
        {{"kind","imageUnderstand"}, {"input",""}, {"prompt",""}, {"provider",""}, {"model",""},
         {"replicateCollection","official"}, {"apiKey",""}, {"baseUrl",""},
         {"resizeFirst",true}, {"resizeWidth",512}, {"json",false},
         {"threads",4}, {"gpuLayers",-1}, {"ctx",4096}, {"maxTokens",2048},
         {"hud",false}, {"storeAs","answer"}})
        .params(ai_understand_params())
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::string_, ParamKind::json_value},
            "Answer")
        .exec({true, true, true});
#endif

    registry["imageAnalyze"] = bdb(
        "imageAnalyze", image_analyze_block,
        "Analyze Image", "Image",
        "Decode each image once (libvips thumbnail) and emit quality / junk scores. "
        "Same analyzer as `find --junk`. Fingerprints will attach here later — no second decode.",
        {{"kind","imageAnalyze"}, {"input",""}, {"recursive",true}, {"minScore",0.0},
         {"junkKinds","all"}, {"edgeEnergyBlur",12.0}, {"edgeEnergyP90Blur",40.0},
         {"edgeEnergyP99Blur",88.0}, {"edgeOrientBlur",0.20},
         {"edgeDensityLo",0.06},
         {"analyzeMaxSide",128}, {"tinyMinSide",128}, {"edgeThreshold",24},
         {"maxResults",0}, {"storeAs","analyses"}})
        .params({
            pd_image("input").in().prev().globs().req()
                .desc("Image file, folder, or glob. Uses PREVIOUS when unset."),
            pd_bool("recursive").grp("scan").dflt(true).desc("Recurse into folder inputs."),
            pd_float("minScore").grp("filter").dflt(0.0).range(0.0, 1.0)
                .desc("Keep rows with junk.hint >= this. 0 = every decoded image."),
            pd_str("junkKinds").adv().grp("filter").dflt("all")
                .desc("dark,bright,blur,flat,tiny or all."),
            pd_float("edgeEnergyBlur").adv().grp("tune").dflt(12.0).range(0.0, 128.0)
                .desc("Blur: mean |dx|+|dy| at/above this → that term is 0."),
            pd_float("edgeEnergyP90Blur").adv().grp("tune").dflt(40.0).range(0.0, 256.0)
                .desc("Blur OOF path: p90 |dx|+|dy| at/above this → that term is 0."),
            pd_float("edgeEnergyP99Blur").adv().grp("tune").dflt(88.0).range(0.0, 510.0)
                .desc("Blur: p99 |dx|+|dy| at/above this → blur 0 (screenshots / sharp islands)."),
            pd_float("edgeOrientBlur").adv().grp("tune").dflt(0.20).range(0.0, 1.0)
                .desc("Blur motion path: orientation peak at/below this → that term is 0."),
            pd_float("edgeDensityLo").adv().grp("tune").dflt(0.06).range(0.0, 1.0)
                .desc("edgeDensity at/above this counts as having edges."),
            pd_int("analyzeMaxSide").adv().grp("tune").dflt(128).range(16, 1024)
                .desc("Analysis thumbnail longest side."),
            pd_int("tinyMinSide").adv().grp("tune").dflt(128).range(1, 4096)
                .desc("tiny score 0 when min(width,height) >= this."),
            pd_int("edgeThreshold").adv().grp("tune").dflt(24).range(0, 510)
                .desc("|dx|+|dy| above this is an edge pixel."),
            pd_int("maxResults").adv().grp("filter").dflt(0).range(0, 100000)
                .desc("0 = unlimited."),
            pd_store_as().dflt("analyses"),
        })
        .primary_output(ParamKind::json_value, "Analyses")
        .exec({false, true, true});

    registry["imageFind"] = bdb(
        "imageFind", image_find_block,
        "Find Images", "Image",
        "Find images by junk/accident scores or by filename. Same core as `tanit-cli find`. "
        "Duplicate / near-duplicate mode comes next and will reuse imageAnalyze features.",
        {{"kind","imageFind"}, {"input",""}, {"mode","junk"}, {"prompt",""},
         {"recursive",true}, {"minScore",0.7}, {"junkKinds","all"},
         {"edgeEnergyBlur",12.0}, {"edgeEnergyP90Blur",40.0},
         {"edgeEnergyP99Blur",88.0}, {"edgeOrientBlur",0.20}, {"edgeDensityLo",0.06},
         {"analyzeMaxSide",128}, {"tinyMinSide",128}, {"edgeThreshold",24},
         {"maxResults",0}, {"storeAs","matches"}})
        .params({
            pd_image("input").in().prev().globs().req()
                .desc("Image file, folder, or glob. Uses PREVIOUS when unset."),
            pd_enum("mode", {"junk", "name"}).grp("query").dflt("junk")
                .desc("junk = quality scores. name = filename / folder substring. "
                      "duplicates = parked."),
            pd_str("prompt").grp("query").dflt("")
                .desc("Required for mode=name."),
            pd_bool("recursive").grp("scan").dflt(true).desc("Recurse into folder inputs."),
            pd_float("minScore").grp("filter").dflt(0.7).range(0.0, 1.0)
                .desc("Junk: minimum hint to match."),
            pd_str("junkKinds").adv().grp("filter").dflt("all")
                .desc("dark,bright,blur,flat,tiny or all."),
            pd_float("edgeEnergyBlur").adv().grp("tune").dflt(12.0).range(0.0, 128.0)
                .desc("Same as find --edge-energy-blur."),
            pd_float("edgeEnergyP90Blur").adv().grp("tune").dflt(40.0).range(0.0, 256.0)
                .desc("Same as find --edge-energy-p90-blur."),
            pd_float("edgeEnergyP99Blur").adv().grp("tune").dflt(88.0).range(0.0, 510.0)
                .desc("Same as find --edge-energy-p99-blur."),
            pd_float("edgeOrientBlur").adv().grp("tune").dflt(0.20).range(0.0, 1.0)
                .desc("Same as find --edge-orient-blur."),
            pd_float("edgeDensityLo").adv().grp("tune").dflt(0.06).range(0.0, 1.0)
                .desc("Same as find --edge-density-lo."),
            pd_int("analyzeMaxSide").adv().grp("tune").dflt(128).range(16, 1024),
            pd_int("tinyMinSide").adv().grp("tune").dflt(128).range(1, 4096),
            pd_int("edgeThreshold").adv().grp("tune").dflt(24).range(0, 510),
            pd_int("maxResults").adv().grp("filter").dflt(0).range(0, 100000),
            pd_store_as().dflt("matches"),
        })
        .primary_output(ParamKind::json_value, "Matches")
        .exec({false, true, true});

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    {
        nlohmann::json shot_defaults = {
            {"kind","imageScreenshot"}, {"input","screen:0"}, {"outputPath",""},
            {"copyToClipboard",false}, {"timeoutMs",5000}, {"hudCapture","auto"},
            {"storeAs","imagePath"}
        };
        merge_screenshot_picker_defaults(shot_defaults);
        registry["imageScreenshot"] = bdb(
        "imageScreenshot", image_screenshot_block,
        "Screenshot", "Image",
        "Capture a monitor, window, or webcam to an image file and/or clipboard. "
        "Set pick=interactive to choose the region in the same block (click window/UIA, "
        "drag a rectangle). pick=off uses input or PREVIOUS from a Picker block.",
        std::move(shot_defaults))
        .params([] {
            std::vector<ParamDef> p = {
            pd_screen_input("input").in().prev().dflt("screen:0")
                .desc("Capture source when pick=off. Same format as videoDetect. "
                      "Ignored when pick=interactive (the pick becomes the source)."),
            };
            auto pick = pd_screenshot_picker_params();
            p.insert(p.end(),
                     std::make_move_iterator(pick.begin()),
                     std::make_move_iterator(pick.end()));
            p.push_back(pd_outpath("outputPath").grp("output").lbl("Output file").dflt("")
                .desc("Output path (.jpg or .png). Optional leave empty to skip saving."));
            p.push_back(pd_bool("copyToClipboard").grp("output").dflt(false)
                .desc("Copy the captured image to the clipboard as CF_DIB (bitmap)."));
            p.push_back(pd_int("timeoutMs").adv().dflt(5000).range(100, 60000)
                .desc("Webcam capture timeout in milliseconds (unused for screen sources)."));
            p.push_back(pd_enum("hudCapture", {"auto", "exclude", "visible"}).adv().grp("capture_options")
                .dflt("auto")
                .desc("Overlay capture policy. With pick=interactive, visible BitBlts the "
                      "live picker HUD/highlight into the still before the overlay closes. "
                      "auto and exclude omit overlays. Idle picker/status HUDs stay visible "
                      "to Windows Snap and other-process recorders."));
            p.push_back(pd_store_as().dflt("imagePath"));
            return p;
        }())
        .primary_output(ParamKind::image_path, "Screenshot")
        .exec({false, true, true});
    }
#endif
}

} // namespace media::xblox::blocks
