#include "blocks/model_blocks.hpp"

#include <nlohmann/json.hpp>
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
#include "llm/llama_agent_client.hpp"
#endif
#if defined(FEATURE_VLM) && FEATURE_VLM
#include "llm/llama_vlm_client.hpp"
#endif
#if defined(FEATURE_ONNX) && FEATURE_ONNX
#include "lib/onnx/OnnxSession.hpp"
#endif
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER && defined(FEATURE_STT) && FEATURE_STT
#include "audio/stt/whisper_local.hpp"
#endif
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
#include "audio/tts/vibevoice_tts.hpp"
#endif
#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
#include "audio/tts/moss_tts.hpp"
#endif

#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <algorithm>

namespace media::xblox::blocks {
namespace {

std::string default_key_for_kind(const std::string& kind)
{
    if (kind == "audioModelControl") {
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
        return "vibevoice:tts";
#elif defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
        return "moss:tts";
#else
        return "vibevoice:tts";
#endif
    }
    if (kind == "imageModelControl") return "llama:vlm";
    if (kind == "visionModelControl") return "llama:vlm";
    if (kind == "ocrModelControl") return "llama:vlm";
    return {};
}

nlohmann::json loaded_models_current_xblox_process_json()
{
    nlohmann::json arr = nlohmann::json::array();
    auto append = [&](const nlohmann::json& rows) {
        if (!rows.is_array())
            return;
        for (const auto& row : rows)
            arr.push_back(row);
    };

#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
    append(media::llm::agent::llama_client::loaded_models_json());
#endif
#if defined(FEATURE_VLM) && FEATURE_VLM
    append(media::llm::vlm_client::loaded_models_json());
#endif
#if defined(FEATURE_ONNX) && FEATURE_ONNX
    append(media::onnx::OnnxSession::loaded_models_json());
#endif
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER && defined(FEATURE_STT) && FEATURE_STT
    append(pm::stt::whisper_loaded_models_json());
#endif
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
    append(pm::tts::VibevoiceEngine::global().loaded_models_json());
#endif
#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
    append(pm::tts::MossNanoEngine::global().loaded_models_json());
#endif
    return arr;
}

bool stop_loaded_model_current_xblox_process(std::string_view key, std::string& err)
{
    if (key == "llama:text") {
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
        return media::llm::agent::llama_client::unload_loaded_model(key, err);
#else
        err = "llama text models are not built into this binary";
        return false;
#endif
    }
    if (key == "llama:vlm") {
#if defined(FEATURE_VLM) && FEATURE_VLM
        return media::llm::vlm_client::unload_loaded_model(key, err);
#else
        err = "vlm models are not built into this binary";
        return false;
#endif
    }
#if defined(FEATURE_ONNX) && FEATURE_ONNX
    if (key.rfind("onnx:", 0) == 0)
        return media::onnx::OnnxSession::unload_loaded_model(key, err);
#endif
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER && defined(FEATURE_STT) && FEATURE_STT
    if (key.rfind("whisper:", 0) == 0)
        return pm::stt::whisper_unload_loaded_model(key, err);
#endif
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
    if (key == "vibevoice:tts")
        return pm::tts::VibevoiceEngine::global().unload_loaded_model(key, err);
#endif
#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
    if (key == "moss:tts")
        return pm::tts::MossNanoEngine::global().unload_loaded_model(key, err);
#endif
    err = "unknown loaded model key: " + std::string(key);
    return false;
}

bool unload_all_loaded_models(std::vector<std::string>& unloaded,
                              std::vector<std::string>& failures)
{
    unloaded.clear();
    failures.clear();

    const nlohmann::json models = loaded_models_current_xblox_process_json();
    std::vector<std::string> keys;
    for (const auto& row : models) {
        if (!row.is_object() || !row.contains("key") || !row["key"].is_string())
            continue;
        if (row.value("stopSupported", true) == false)
            continue;
        const std::string k = row["key"].get<std::string>();
        if (std::find(keys.begin(), keys.end(), k) != keys.end())
            continue;
        keys.push_back(k);
    }

    for (const std::string& k : keys) {
        std::string err;
        if (stop_loaded_model_current_xblox_process(k, err))
            unloaded.push_back(k);
        else
            failures.push_back(k + ": " + (err.empty() ? "unload failed" : err));
    }
    return failures.empty();
}

BlockResult model_control_block(BlockIO& io)
{
    const std::string kind = io.str("kind", "modelControl");
    const std::string action = io.str("action", "List");

    if (action == "List") {
        nlohmann::json models = loaded_models_current_xblox_process_json();
        io.ok(kind, "listed " + std::to_string(models.size()) + " loaded model(s)")
          .set("models", ParamKind::json_value, Value::from_json(models))
          .result(Value::from_json(std::move(models)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "Unload") {
        std::string key = io.str("key");
        if (key.empty())
            key = default_key_for_kind(kind);
        if (key.empty()) {
            io.error(kind, "modelControl: key is required for Unload", 2);
            return BlockResult::Error;
        }

        if (key == "*") {
            std::vector<std::string> unloaded;
            std::vector<std::string> failures;
            const bool all_ok = unload_all_loaded_models(unloaded, failures);

            nlohmann::json unloaded_json = nlohmann::json::array();
            for (const auto& k : unloaded)
                unloaded_json.push_back(k);

            if (!all_ok && unloaded.empty()) {
                std::string err = failures.empty() ? "modelControl: unload all failed" : failures.front();
                io.error(kind, "modelControl: " + err, 1)
                  .set("unloaded", ParamKind::json_value, Value::from_json(unloaded_json));
                return BlockResult::Error;
            }

            std::string msg = "unloaded " + std::to_string(unloaded.size()) + " model(s)";
            if (!failures.empty())
                msg += " (" + std::to_string(failures.size()) + " failed)";
            io.ok(kind, msg)
              .set("key", "*")
              .set("unloaded", ParamKind::json_value, Value::from_json(unloaded_json))
              .result(Value::from_json(std::move(unloaded_json)), ParamKind::json_value);
            return BlockResult::Ok;
        }

        std::string err;
        const bool ok = stop_loaded_model_current_xblox_process(key, err);
        if (!ok) {
            io.error(kind, err.empty() ? "modelControl: unload failed" : "modelControl: " + err, 1)
              .set("key", key);
            return BlockResult::Error;
        }

        io.ok(kind, "unloaded model: " + key)
          .set("key", key)
          .result(key);
        return BlockResult::Ok;
    }

    io.error(kind, "modelControl: unsupported action '" + action + "'", 2);
    return BlockResult::Error;
}

std::vector<ParamDef> model_control_params(std::string default_key = {}, std::string store_default = "models")
{
    return {
        pd_enum("action", {"List", "Unload"}).grp("lifecycle").dflt("List")
            .desc("List loaded models in this xBlox process, or unload one by key."),
        pd_str("key").grp("lifecycle").dflt(std::move(default_key))
            .desc("Loaded model key to unload, e.g. llama:vlm, llama:text, vibevoice:tts, moss:tts, onnx:*, "
                  "or * to unload every stoppable model in this process. Empty aliases use their group default."),
        pd_store_as().dflt(std::move(store_default)),
    };
}

void register_model_alias(BlockRegistry& registry,
                          const std::string& kind,
                          const std::string& label,
                          const std::string& group,
                          const std::string& default_key)
{
    registry[kind] = bdb(
        kind, model_control_block, label, group,
        "List or unload loaded local model instances for this xBlox process.",
        {{"kind", kind}, {"action", "Unload"}, {"key", default_key}, {"storeAs", "modelKey"}})
        .params(model_control_params(default_key, "modelKey"))
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::json_value, ParamKind::string_},
            "Model result")
        .output("models", ParamKind::json_value, "Models", "models", true)
        .output("unloaded", ParamKind::json_value, "Unloaded", "unloaded", true)
        .exec({false, false, true});
}

} // namespace

void register_model_blocks(BlockRegistry& registry)
{
    registry["modelControl"] = bdb(
        "modelControl", model_control_block, "Model Control", "AI",
        "List or unload loaded local model instances for this xBlox process.",
        {{"kind", "modelControl"}, {"action", "List"}, {"key", ""}, {"storeAs", "models"}})
        .params(model_control_params())
        .dynamic_primary_output(
            ParamKind::json_value,
            {ParamKind::json_value, ParamKind::string_},
            "Model result")
        .output("models", ParamKind::json_value, "Models", "models", true)
        .output("unloaded", ParamKind::json_value, "Unloaded", "unloaded", true)
        .exec({false, false, true});

    register_model_alias(registry, "audioModelControl", "Unload Model", "Audio",
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
                        "vibevoice:tts"
#elif defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
                        "moss:tts"
#else
                        "vibevoice:tts"
#endif
    );
    register_model_alias(registry, "imageModelControl", "Unload Model", "Image", "llama:vlm");
    register_model_alias(registry, "visionModelControl", "Unload Model", "Vision", "llama:vlm");
    register_model_alias(registry, "ocrModelControl", "Unload Model", "OCR", "llama:vlm");
    register_model_alias(registry, "videoModelControl", "Unload Model", "Video", "");
}

} // namespace media::xblox::blocks
