#include "blocks/block_params_ui.hpp"

#include "blocks/block_params_mapping.hpp"
#include "blocks/block_registry.hpp"
#include "commands/command_registry.hpp"
#include "commands/options_registry.hpp"
#include "blocks/mcp_server_config_normalize.hpp"
#include "cli/pm_image_command_info.hpp"
#if defined(_WIN32)
#include "core/host_backends.hpp"
#endif
#include "core/app_exe_directory.hpp"
#include "core/command_variables.hpp"
#include "core/local_model_catalog.hpp"
#include "core/openrouter_provider_models_cli.hpp"
#include "core/replicate_provider_models_cli.hpp"
#include "core/settings_runtime.hpp"
#include "logging/logger.h"

#if FEATURE_MCP_CLIENT
#include "llm/mcp_config.hpp"
#endif
#if defined(PM_PLUGIN_CHAT) && PM_PLUGIN_CHAT
#include "plugins/chat/host_chat.hpp"
#elif defined(PM_CHAT_SURFACE) && PM_CHAT_SURFACE
#include "llm/path_tool_catalog.hpp"
#endif

#if PM_HAS_AUDIO_IO
#include "core/audio_devices.hpp"
#endif
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "core/video.hpp"
#if defined(_WIN32)
#include "core/screen_input.hpp"
#endif
#endif

#include <algorithm>
#include <cctype>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "fs/path_sanitizer.hpp"

namespace media::xblox::blocks::ui {
namespace {

namespace fs = std::filesystem;

bool file_exists(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

nlohmann::json ocr_onnx_model_options()
{
    nlohmann::json options = nlohmann::json::array();
    std::vector<std::string> seen;
    for (const auto& root : media::local_models::local_model_roots()) {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;

        const fs::path det = root / "en_PP-OCRv3_det_infer.onnx";
        const fs::path rec_v4 = root / "en_PP-OCRv4_rec_infer.onnx";
        const fs::path rec_v3 = root / "en_PP-OCRv3_rec_infer.onnx";
        const fs::path rec = file_exists(rec_v4) ? rec_v4 : rec_v3;
        const fs::path dict = root / "en_dict.txt";
        if (!file_exists(det) || !file_exists(rec) || !file_exists(dict))
            continue;

        const std::string key = "paddleocr-en";
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);
        options.push_back({
            {"value", "paddleocr-en"},
            {"label", "PaddleOCR English (" + media::path::path_to_utf8(root.filename()) + ")"},
            {"provider", "onnx"},
            {"detModel", media::path::path_to_utf8(det)},
            {"recModel", media::path::path_to_utf8(rec)},
            {"dict", media::path::path_to_utf8(dict)},
        });
    }
    return options;
}

nlohmann::json vlm_model_options_for_slot(const std::string& slot, const std::string& preferred_name)
{
    nlohmann::json options = nlohmann::json::array();
    auto models = media::local_models::discover_llama_vlm_models(false);
    std::stable_sort(models.begin(), models.end(), [&](const auto& a, const auto& b) {
        const auto contains = [&](const auto& m) {
            const std::string hay = m.id + " " + m.display_name + " " + media::path::path_to_utf8(m.model_path);
            return hay.find(preferred_name) != std::string::npos || hay.find(preferred_name == "MiniCPM" ? "minicpm" : "paddleocr") != std::string::npos;
        };
        return contains(a) && !contains(b);
    });
    for (const auto& m : models) {
        bool has_slot = false;
        for (const auto& s : m.slots)
            if (s == slot)
                has_slot = true;
        if (!has_slot)
            continue;
        options.push_back({
            {"value", m.id.empty() ? media::path::path_to_utf8(m.model_path) : m.id},
            {"label", m.display_name.empty() ? (m.id.empty() ? media::path::path_to_utf8(m.model_path.filename()) : m.id) : m.display_name},
            {"provider", slot},
            {"modelPath", media::path::path_to_utf8(m.model_path)},
            {"mmprojPath", media::path::path_to_utf8(m.mmproj_path)},
        });
    }
    return options;
}

nlohmann::json ocr_vlm_model_options()
{
    return vlm_model_options_for_slot("ocr", "PaddleOCR");
}

nlohmann::json vision_vlm_model_options()
{
    return vlm_model_options_for_slot("vision", "MiniCPM");
}

nlohmann::json audio_sample_rate_options()
{
    return nlohmann::json::array({
        {{"value", "8000"},  {"label", "8000 Hz"}},
        {{"value", "16000"}, {"label", "16000 Hz (STT)"}},
        {{"value", "24000"}, {"label", "24000 Hz"}},
        {{"value", "44100"}, {"label", "44100 Hz"}},
        {{"value", "48000"}, {"label", "48000 Hz"}},
        {{"value", "96000"}, {"label", "96000 Hz"}},
    });
}

nlohmann::json audio_channel_options()
{
    return nlohmann::json::array({
        {{"value", "1"}, {"label", "Mono"}},
        {{"value", "2"}, {"label", "Stereo"}},
    });
}

nlohmann::json option_row(const char* value, const char* label)
{
    return {{"value", value}, {"label", label}};
}

nlohmann::json stt_provider_options()
{
    nlohmann::json options = nlohmann::json::array();
    options.push_back(option_row("tanit", "Tanit (proxy)"));
    options.push_back(option_row("openai", "OpenAI (direct)"));
    options.push_back(option_row("openrouter", "OpenRouter"));
    options.push_back(option_row("elevenlabs", "ElevenLabs (direct)"));
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
    options.push_back(option_row("whisper", "Whisper (local, offline)"));
#endif
    return options;
}

nlohmann::json tts_provider_options()
{
    nlohmann::json options = nlohmann::json::array();
    options.push_back(option_row("tanit", "Tanit (proxy)"));
    options.push_back(option_row("openai", "OpenAI (direct)"));
    options.push_back(option_row("openrouter", "OpenRouter"));
    options.push_back(option_row("elevenlabs", "ElevenLabs (direct)"));
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
    options.push_back(option_row("vibevoice", "VibeVoice (local, offline)"));
#endif
#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
    options.push_back(option_row("moss", "MOSS-TTS-Nano (local, offline)"));
#endif
    return options;
}

nlohmann::json stt_model_options(const std::string& provider)
{
    if (provider == "whisper") {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
        return media::local_models::whisper_stt_model_options_json();
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "elevenlabs")
        return nlohmann::json::array({
            option_row("scribe_v2_realtime", "Scribe v2 (real-time streaming)"),
        });
    if (provider == "openai")
        return nlohmann::json::array({
            option_row("gpt-4o-mini-transcribe", "GPT-4o Mini Transcribe"),
            option_row("gpt-4o-transcribe", "GPT-4o Transcribe"),
            option_row("whisper-1", "Whisper"),
        });
    if (provider == "openrouter")
        return nlohmann::json::array({
            option_row("openai/whisper-large-v3-turbo", "Whisper Large V3 Turbo"),
            option_row("openai/whisper-large-v3", "Whisper Large V3"),
            option_row("openai/gpt-4o-mini-transcribe", "GPT-4o Mini Transcribe"),
            option_row("deepgram/nova-3", "Deepgram Nova-3"),
            option_row("google/chirp-3", "Google Chirp 3"),
        });
    if (provider.empty() || provider == "tanit")
        return nlohmann::json::array({
            option_row("tanit-speech-to-text", "Tanit STT (Whisper-1)"),
        });
    return nlohmann::json::array();
}

nlohmann::json image_provider_options()
{
    nlohmann::json options = nlohmann::json::array();
#if defined(FEATURE_PROVIDER_TANIT) && FEATURE_PROVIDER_TANIT
    options.push_back(option_row("tanit", "Tanit (proxy)"));
#endif
#if defined(FEATURE_PROVIDER_GOOGLE) && FEATURE_PROVIDER_GOOGLE
    options.push_back(option_row("google", "Google (Imagen)"));
#endif
#if defined(FEATURE_PROVIDER_OPENAI) && FEATURE_PROVIDER_OPENAI
    options.push_back(option_row("openai", "OpenAI (direct)"));
#endif
#if defined(FEATURE_PROVIDER_REPLICATE) && FEATURE_PROVIDER_REPLICATE
    options.push_back(option_row("replicate", "Replicate"));
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA && defined(FEATURE_VLM) && FEATURE_VLM
    options.push_back(option_row("llama", "Llama.cpp VLM (local)"));
#endif
    return options;
}

nlohmann::json vision_provider_options()
{
    nlohmann::json options = nlohmann::json::array();
#if defined(FEATURE_PROVIDER_TANIT) && FEATURE_PROVIDER_TANIT
    options.push_back(option_row("tanit", "Tanit (proxy)"));
#endif
#if defined(FEATURE_PROVIDER_GOOGLE) && FEATURE_PROVIDER_GOOGLE
    options.push_back(option_row("google", "Google (Gemini)"));
#endif
#if defined(FEATURE_PROVIDER_OPENAI) && FEATURE_PROVIDER_OPENAI
    options.push_back(option_row("openai", "OpenAI (GPT-4o)"));
#endif
#if defined(FEATURE_PROVIDER_REPLICATE) && FEATURE_PROVIDER_REPLICATE
    options.push_back(option_row("replicate", "Replicate"));
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA && defined(FEATURE_VLM) && FEATURE_VLM
    options.push_back(option_row("llama", "Llama.cpp VLM (local)"));
#endif
    return options;
}

nlohmann::json image_model_options(const std::string& provider)
{
    if (provider == "google") {
#if defined(FEATURE_PROVIDER_GOOGLE) && FEATURE_PROVIDER_GOOGLE
        return nlohmann::json::array({
            option_row("imagen-4.0-generate-preview-06-06", "Imagen 4 Preview"),
            option_row("imagen-3.0-generate-002", "Imagen 3"),
            option_row("imagen-3.0-fast-generate-001", "Imagen 3 Fast"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "openai") {
#if defined(FEATURE_PROVIDER_OPENAI) && FEATURE_PROVIDER_OPENAI
        return nlohmann::json::array({
            option_row("gpt-image-1", "GPT Image 1 (gpt-image-1)"),
            option_row("dall-e-3", "DALL-E 3"),
            option_row("dall-e-2", "DALL-E 2"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "replicate")
        return nlohmann::json::array();
    if (provider == "llama") {
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA && defined(FEATURE_VLM) && FEATURE_VLM
        return vision_vlm_model_options();
#else
        return nlohmann::json::array();
#endif
    }
    if (provider.empty() || provider == "tanit") {
#if defined(FEATURE_PROVIDER_TANIT) && FEATURE_PROVIDER_TANIT
        return nlohmann::json::array({
            option_row("image-generation-fast", "Tanit Image Fast"),
            option_row("image-generation-deep", "Tanit Image Deep"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    return nlohmann::json::array();
}

nlohmann::json vision_model_options(const std::string& provider)
{
    if (provider == "google") {
#if defined(FEATURE_PROVIDER_GOOGLE) && FEATURE_PROVIDER_GOOGLE
        return nlohmann::json::array({
            option_row("gemini-2.5-pro", "Gemini 2.5 Pro"),
            option_row("gemini-2.5-flash", "Gemini 2.5 Flash"),
            option_row("gemini-2.0-flash", "Gemini 2.0 Flash"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "openai") {
#if defined(FEATURE_PROVIDER_OPENAI) && FEATURE_PROVIDER_OPENAI
        return nlohmann::json::array({
            option_row("gpt-4.1", "GPT-4.1"),
            option_row("gpt-4o", "GPT-4o"),
            option_row("gpt-4o-mini", "GPT-4o mini"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "replicate")
        return nlohmann::json::array();
    if (provider == "llama") {
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA && defined(FEATURE_VLM) && FEATURE_VLM
        return vision_vlm_model_options();
#else
        return nlohmann::json::array();
#endif
    }
    if (provider.empty() || provider == "tanit") {
#if defined(FEATURE_PROVIDER_TANIT) && FEATURE_PROVIDER_TANIT
        return nlohmann::json::array({
            option_row("image-vision-deep", "Tanit Vision Deep"),
            option_row("image-vision-free", "Tanit Vision Free"),
            option_row("multimodal-free", "Tanit Multimodal Free"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    return nlohmann::json::array();
}

nlohmann::json tts_model_options(const std::string& provider)
{
    if (provider == "vibevoice") {
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
        return nlohmann::json::array({
            option_row("vibevoice-realtime-0.5B-q8_0", "VibeVoice Realtime 0.5B (q8, ~600 MB)"),
            option_row("vibevoice-1.5B-q8_0", "VibeVoice 1.5B voice-clone (q8, ~1.8 GB)"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "moss") {
#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
        return nlohmann::json::array({
            option_row("moss-tts-nano", "MOSS-TTS-Nano 100M (local GGUF bundle)"),
        });
#else
        return nlohmann::json::array();
#endif
    }
    if (provider == "elevenlabs")
        return nlohmann::json::array({
            option_row("eleven_v3", "ElevenLabs v3"),
            option_row("eleven_multilingual_v2", "Multilingual v2"),
            option_row("eleven_turbo_v2_5", "Turbo v2.5 (low latency)"),
            option_row("eleven_flash_v2_5", "Flash v2.5 (ultra-fast)"),
        });
    if (provider == "openai")
        return nlohmann::json::array({
            option_row("gpt-4o-mini-tts", "GPT-4o Mini TTS"),
            option_row("tts-1", "TTS-1"),
            option_row("tts-1-hd", "TTS-1 HD"),
        });
    if (provider == "openrouter")
        return nlohmann::json::array({
            option_row("x-ai/grok-voice-tts-1.0", "Grok Voice TTS 1.0"),
            option_row("mistralai/voxtral-mini-tts-2603", "Voxtral Mini TTS"),
            option_row("hexgrad/kokoro-82m", "Kokoro 82M"),
            option_row("deepgram/flux-tts:free", "Deepgram Flux TTS (free)"),
            option_row("google/gemini-3.1-flash-tts-preview", "Gemini 3.1 Flash TTS"),
        });
    if (provider.empty() || provider == "tanit")
        return nlohmann::json::array({
            option_row("tanit-speech", "Tanit TTS (Multilingual v2)"),
            option_row("tanit-speech-turbo", "Tanit TTS Turbo (v2.5)"),
        });
    return nlohmann::json::array();
}

nlohmann::json openrouter_tts_voice_options(const std::string& model)
{
    if (model == "mistralai/voxtral-mini-tts-2603")
        return nlohmann::json::array({
            option_row("en_paul_neutral", "Paul (neutral)"),
            option_row("en_paul_cheerful", "Paul (cheerful)"),
        });
    if (model == "hexgrad/kokoro-82m")
        return nlohmann::json::array({
            option_row("af_heart", "Heart"),
            option_row("af_bella", "Bella"),
            option_row("am_adam", "Adam"),
        });
    if (model == "deepgram/flux-tts:free")
        return nlohmann::json::array({
            option_row("flux-alexis-en", "Alexis"),
            option_row("flux-bruce-en", "Bruce"),
        });
    if (model == "google/gemini-3.1-flash-tts-preview")
        return nlohmann::json::array({
            option_row("Kore", "Kore"),
            option_row("Puck", "Puck"),
            option_row("Charon", "Charon"),
        });
    return nlohmann::json::array({
        option_row("eve", "Eve"),
        option_row("ara", "Ara"),
        option_row("rex", "Rex"),
        option_row("sal", "Sal"),
        option_row("leo", "Leo"),
    });
}

nlohmann::json video_fps_options()
{
    return nlohmann::json::array({
        {{"value", "24"}, {"label", "24 fps (film)"}},
        {{"value", "30"}, {"label", "30 fps (default)"}},
        {{"value", "60"}, {"label", "60 fps (smooth UI)"}},
        {{"value", "120"}, {"label", "120 fps"}},
    });
}

nlohmann::json video_bitrate_kbps_options()
{
    return nlohmann::json::array({
        {{"value", "8000"},  {"label", "8 Mbps (light)"}},
        {{"value", "15000"}, {"label", "15 Mbps (UI detail)"}},
        {{"value", "18000"}, {"label", "18 Mbps (HQ square)"}},
        {{"value", "20000"}, {"label", "20 Mbps (portrait/short)"}},
        {{"value", "25000"}, {"label", "25 Mbps (max detail)"}},
    });
}

nlohmann::json audio_device_options(bool output)
{
    nlohmann::json options = nlohmann::json::array();
#if PM_HAS_AUDIO_IO
    try {
        const auto devices = output
            ? pm::audio::enumerate_playback_devices()
            : pm::audio::enumerate_capture_devices();
        for (const auto& d : devices) {
            const std::string label = d.name + (d.is_default ? " (default)" : "");
            options.push_back({
                {"value", d.name},
                {"label", label},
                {"is_default", d.is_default},
            });
        }
    } catch (...) {
    }
#else
    (void)output;
#endif
    return options;
}

nlohmann::json audio_loopback_device_options()
{
    nlohmann::json options = nlohmann::json::array();
#if PM_HAS_AUDIO_IO
    try {
        for (const auto& d : pm::audio::enumerate_loopback_devices()) {
            const std::string label = d.name + (d.is_default ? " (default)" : "");
            options.push_back({
                {"value", d.name},
                {"label", label},
                {"is_default", d.is_default},
                {"is_loopback", true},
            });
        }
    } catch (...) {
    }
#endif
    return options;
}

bool is_scope_ident(std::string_view name)
{
    if (name.empty())
        return false;
    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(first) || first == '_'))
        return false;
    for (size_t i = 1; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!(std::isalnum(c) || c == '_'))
            return false;
    }
    return true;
}

/// `${audioSource}` → `audioSource`. Rejects `${ENV:…}` / `${KNOWNFOLDER:…}` / templates.
std::string sole_scope_var_ref(const nlohmann::json& value)
{
    if (!value.is_string())
        return {};
    const std::string& s = value.get_ref<const std::string&>();
    if (s.size() < 4 || s[0] != '$' || s[1] != '{' || s.back() != '}')
        return {};
    const std::string inner = s.substr(2, s.size() - 3);
    if (inner.find(':') != std::string::npos || inner == "PREVIOUS")
        return {};
    return is_scope_ident(inner) ? inner : std::string{};
}

std::string json_inferred_type(const nlohmann::json& value)
{
    if (value.is_boolean())
        return "bool";
    if (value.is_number_integer())
        return "int";
    if (value.is_number())
        return "number";
    if (value.is_array())
        return "array";
    if (value.is_object())
        return "object";
    return "string";
}

bool is_block_meta_or_child_key(const BlockDescriptor* desc, const std::string& key)
{
    static const std::set<std::string> k_meta = {
        "kind", "blockId", "enabled", "storeAs", "storeResult",
        "consequent", "alternate", "body", "cases", "children", "items",
        "target", "event", "roots", "loop",
    };
    if (k_meta.count(key))
        return true;
    if (!desc)
        return false;
    for (const auto& child : desc->child_lists) {
        if (child.key == key)
            return true;
    }
    return false;
}

void visit_document_blocks(const nlohmann::json& node,
                           const std::function<void(const nlohmann::json&)>& fn)
{
    if (node.is_array()) {
        for (const auto& item : node)
            visit_document_blocks(item, fn);
        return;
    }
    if (!node.is_object() || !node.contains("kind") || !node["kind"].is_string())
        return;
    fn(node);
    const std::string kind = node.value("kind", std::string{});
    const BlockDescriptor* desc = builtin_block_descriptor(kind);
    if (desc) {
        for (const auto& child : desc->child_lists) {
            const auto it = node.find(child.key);
            if (it != node.end())
                visit_document_blocks(*it, fn);
        }
        return;
    }
    for (const char* key : {"items", "consequent", "alternate", "body", "children", "cases"}) {
        const auto it = node.find(key);
        if (it != node.end())
            visit_document_blocks(*it, fn);
    }
}

struct BoundSourceParam {
    nlohmann::json param = nullptr;
    std::string block_kind;
    std::string block_id;
    std::string field;
    bool name_matches = false;
};

void consider_bound_param(std::map<std::string, BoundSourceParam>& bound,
                          const nlohmann::json& block,
                          const std::string& field,
                          const ParamDef& def,
                          const std::string& var)
{
    if (var.empty())
        return;
    BoundSourceParam& slot = bound[var];
    const bool name_matches = (field == var);
    if (!slot.param.is_null() && !(name_matches && !slot.name_matches))
        return;
    slot.param = def.to_json();
    slot.block_kind = block.value("kind", std::string{});
    slot.block_id = block.value("blockId", std::string{});
    slot.field = field;
    slot.name_matches = name_matches;
    logger::trace("[xblox-script-info] bind var=" + var
                  + " field=" + field
                  + " kind=" + slot.block_kind
                  + " block=" + slot.block_id
                  + " paramKind=" + slot.param.value("kind", std::string{})
                  + " nameMatch=" + std::string(name_matches ? "1" : "0"));
}

void collect_bound_source_params(const nlohmann::json& roots,
                                 std::map<std::string, BoundSourceParam>& bound)
{
    visit_document_blocks(roots, [&](const nlohmann::json& block) {
        const std::string kind = block.value("kind", std::string{});
        const std::vector<ParamDef>* defs = builtin_block_param_defs(kind);
        const BlockDescriptor* desc = builtin_block_descriptor(kind);
        if (!defs) {
            logger::trace("[xblox-script-info] no param defs kind=" + kind
                          + " block=" + block.value("blockId", std::string{}));
            return;
        }
        if (kind == "videoCapture") {
            bool has_audio = false;
            bool has_hud = false;
            for (const ParamDef& def : *defs) {
                if (def.name == "audioSource") has_audio = true;
                if (def.name == "hud") has_hud = true;
            }
            logger::trace(std::string("[xblox-script-info] videoCapture defs=")
                          + std::to_string(defs->size())
                          + " audioSourceDef=" + (has_audio ? "1" : "0")
                          + " hudDef=" + (has_hud ? "1" : "0")
                          + " audioSourceRef=" + sole_scope_var_ref(block.value("audioSource", nlohmann::json{}))
                          + " hudRef=" + sole_scope_var_ref(block.value("hud", nlohmann::json{})));
        }
        for (const ParamDef& def : *defs) {
            if (def.source == ParamSource::Variable && is_scope_ident(def.source_ref))
                consider_bound_param(bound, block, def.name, def, def.source_ref);
            const auto field_it = block.find(def.name);
            if (field_it == block.end())
                continue;
            consider_bound_param(bound, block, def.name, def, sole_scope_var_ref(*field_it));
        }
        for (auto it = block.begin(); it != block.end(); ++it) {
            if (is_block_meta_or_child_key(desc, it.key()))
                continue;
            const std::string var = sole_scope_var_ref(it.value());
            if (var.empty())
                continue;
            const auto found = std::find_if(defs->begin(), defs->end(),
                [&](const ParamDef& def) { return def.name == it.key(); });
            if (found != defs->end())
                consider_bound_param(bound, block, it.key(), *found, var);
        }
    });
}

void trace_script_context_info(const nlohmann::json& out, const std::map<std::string, BoundSourceParam>& bound)
{
    std::string line = "[xblox-script-info] done path=" + out.value("path", std::string{})
        + " requested=" + out.value("requestedPath", std::string{})
        + " bound=" + std::to_string(bound.size())
        + " vars=" + std::to_string(out.contains("variables") && out["variables"].is_array()
            ? out["variables"].size() : 0);
    if (out.contains("error"))
        line += " error=" + out.value("error", std::string{});
    logger::trace(line);
    if (out.contains("variables") && out["variables"].is_array()) {
        for (const auto& variable : out["variables"]) {
            if (!variable.is_object() || !variable.value("public", false))
                continue;
            logger::trace("[xblox-script-info] public name=" + variable.value("name", std::string{})
                          + " type=" + variable.value("type", std::string{})
                          + " valueType=" + std::string(variable.contains("value") ? variable["value"].type_name() : "missing")
                          + " hasParam=" + std::string(variable.contains("param") ? "1" : "0")
                          + " paramKind=" + (variable.contains("param")
                              ? variable["param"].value("kind", std::string{}) : std::string{})
                          + " source=" + (variable.contains("source")
                              ? variable["source"].value("blockKind", std::string{}) + "."
                                + variable["source"].value("field", std::string{})
                              : std::string{}));
        }
    }
}

nlohmann::json script_context_info_impl(const std::string& path)
{
    nlohmann::json out{
        {"path", path},
        {"requestedPath", path},
        {"variables", nlohmann::json::array()},
    };
    logger::trace("[xblox-script-info] begin path=" + path);
    if (path.empty()) {
        out["error"] = "missing path";
        logger::trace("[xblox-script-info] missing path");
        return out;
    }

    try {
        media::commands::VariableContext variable_context;
        std::error_code cwd_ec;
        const auto cwd = fs::current_path(cwd_ec);
        if (!cwd_ec)
            variable_context.cwd = media::path::path_to_utf8(cwd);
        const auto vars = media::commands::make_variable_map(variable_context);
        std::string resolve_error;
        const std::string resolved = media::commands::deep_resolve_variables(path, vars, &resolve_error);
        if (!resolve_error.empty()) {
            out["error"] = "cannot resolve XBlox path: " + resolve_error;
            logger::trace("[xblox-script-info] resolve failed path=" + path + " error=" + resolve_error);
            return out;
        }
        out["path"] = resolved;
        logger::trace("[xblox-script-info] resolved path=" + resolved);

        const fs::path file_path = media::path::path_from_utf8(resolved);
        std::ifstream ifs(file_path, std::ios::binary);
        if (!ifs) {
            out["error"] = "cannot read XBlox file";
            logger::trace("[xblox-script-info] cannot read " + resolved);
            return out;
        }
        std::ostringstream ss;
        ss << ifs.rdbuf();
        const nlohmann::json doc = nlohmann::json::parse(ss.str());
        if (!doc.is_object())
            return out;

        std::map<std::string, BoundSourceParam> bound;
        if (doc.contains("roots"))
            collect_bound_source_params(doc["roots"], bound);

        static const std::set<std::string> k_reserved_context_keys = {
            "args", "variable-description", "variable-public",
        };

        auto push_var = [&](const std::string& name, const nlohmann::json& value,
                            const std::string& description, bool is_public,
                            const std::string& explicit_type) {
            if (name.empty())
                return;
            nlohmann::json var_entry = {
                {"name", name},
                {"value", value},
                {"text", value.is_string() ? value.get<std::string>() : value.dump()},
                {"public", is_public},
            };
            const auto bound_it = bound.find(name);
            if (bound_it != bound.end() && !bound_it->second.param.is_null()) {
                var_entry["param"] = bound_it->second.param;
                var_entry["source"] = {
                    {"blockKind", bound_it->second.block_kind},
                    {"blockId", bound_it->second.block_id},
                    {"field", bound_it->second.field},
                };
                const std::string kind = bound_it->second.param.value("kind", std::string{});
                var_entry["type"] = kind.empty() ? json_inferred_type(value) : kind;
                if (description.empty()) {
                    const std::string desc = bound_it->second.param.value("description", std::string{});
                    if (!desc.empty())
                        var_entry["description"] = desc;
                }
            } else {
                var_entry["type"] = explicit_type.empty() ? json_inferred_type(value) : explicit_type;
            }
            if (!description.empty())
                var_entry["description"] = description;
            out["variables"].push_back(std::move(var_entry));
        };

        if (doc.contains("context") && doc["context"].is_object()) {
            nlohmann::json descriptions = nlohmann::json::object();
            const auto desc_it = doc["context"].find("variable-description");
            if (desc_it != doc["context"].end() && desc_it->is_object())
                descriptions = *desc_it;
            nlohmann::json public_flags = nlohmann::json::object();
            const auto public_it = doc["context"].find("variable-public");
            if (public_it != doc["context"].end() && public_it->is_object())
                public_flags = *public_it;
            for (auto it = doc["context"].begin(); it != doc["context"].end(); ++it) {
                if (k_reserved_context_keys.count(it.key()))
                    continue;
                const auto pub_it = public_flags.find(it.key());
                const bool is_public = pub_it != public_flags.end()
                    && pub_it->is_boolean() && pub_it->get<bool>();
                std::string description;
                const auto d_it = descriptions.find(it.key());
                if (d_it != descriptions.end() && d_it->is_string())
                    description = d_it->get<std::string>();
                push_var(it.key(), it.value(), description, is_public, {});
            }
            trace_script_context_info(out, bound);
            return out;
        }

        if (doc.contains("variables") && doc["variables"].is_array()) {
            for (const auto& v : doc["variables"]) {
                if (!v.is_object())
                    continue;
                const std::string name = v.value("name", std::string{});
                nlohmann::json def = nullptr;
                if (v.contains("value"))
                    def = v["value"];
                else if (v.contains("text"))
                    def = v["text"];
                push_var(name, def,
                         v.value("description", std::string{}),
                         v.value("public", false),
                         v.value("type", std::string{}));
            }
        }
    } catch (const std::exception& e) {
        out["error"] = std::string("XBlox parse failed: ") + e.what();
        logger::trace("[xblox-script-info] parse failed path=" + path + " error=" + e.what());
    } catch (...) {
        out["error"] = "XBlox parse failed";
        logger::trace("[xblox-script-info] parse failed path=" + path);
    }
    logger::trace("[xblox-script-info] done path=" + out.value("path", std::string{})
                  + " vars=" + std::to_string(out.contains("variables") && out["variables"].is_array()
                      ? out["variables"].size() : 0)
                  + (out.contains("error") ? " error=" + out.value("error", std::string{}) : ""));
    return out;
}

nlohmann::json xblox_script_context_options(const std::string& path)
{
    nlohmann::json options = nlohmann::json::array();
    const nlohmann::json info = script_context_info_impl(path);
    if (!info.contains("variables") || !info["variables"].is_array())
        return options;
    for (const auto& variable : info["variables"]) {
        if (!variable.is_object())
            continue;
        const std::string name = variable.value("name", std::string{});
        if (name.empty())
            continue;
        nlohmann::json option = {
            {"value", name},
            {"label", name},
            {"name", name},
            {"type", variable.value("type", std::string{})},
            {"default", variable.contains("value") ? variable["value"] : nlohmann::json{}},
            {"public", variable.value("public", false)},
        };
        if (variable.contains("param"))
            option["param"] = variable["param"];
        if (variable.contains("source"))
            option["source"] = variable["source"];
        if (variable.contains("description"))
            option["description"] = variable["description"];
        options.push_back(std::move(option));
    }
    return options;
}

nlohmann::json empty_provider_options_schema(const std::string& block_kind,
                                             const std::string& provider,
                                             const std::string& model)
{
    return {
        {"block", block_kind},
        {"provider", provider},
        {"model", model},
        {"value_field", "providerOptions"},
        {"schema", nullptr},
        {"ui", nlohmann::json::object()},
    };
}

nlohmann::json whisper_provider_options_schema(const std::string& block_kind,
                                               const std::string& provider,
                                               const std::string& model)
{
    return {
        {"block", block_kind},
        {"mode", "stt"},
        {"provider", provider},
        {"model", model},
        {"value_field", "providerOptions"},
        {"schema", {
            {"type", "object"},
            {"properties", {
                {"backend", {
                    {"type", "string"},
                    {"title", "Backend"},
                    {"description", "Local whisper.cpp inference backend. gpu falls back to cpu when the device cannot load the model."},
                    {"enum", {"cpu", "gpu"}},
                    {"default", "gpu"},
                }},
                {"gpuDevice", {
                    {"type", "integer"},
                    {"title", "GPU device"},
                    {"description", "GPU device index. Only meaningful with backend=gpu."},
                    {"minimum", 0},
                    {"default", 0},
                }},
                {"threads", {
                    {"type", "integer"},
                    {"title", "Threads"},
                    {"description", "CPU thread count. 0 means auto."},
                    {"minimum", 0},
                    {"default", 0},
                }},
                {"language", {
                    {"type", "string"},
                    {"title", "Language"},
                    {"description", "Whisper language code. auto enables language detection for multilingual models."},
                    {"default", "auto"},
                }},
                {"flashAttn", {
                    {"type", "string"},
                    {"title", "Flash attention"},
                    {"description", "Flash attention policy. auto leaves the whisper.cpp default untouched."},
                    {"enum", {"auto", "on", "off"}},
                    {"default", "auto"},
                }},
            }},
            {"required", nlohmann::json::array()},
        }},
        {"ui", {
            {"order", {"backend", "gpuDevice", "threads", "language", "flashAttn"}},
            {"groups", {
                {"runtime", {{"title", "Whisper Runtime"}}},
            }},
            {"fields", {
                {"backend", {{"group", "runtime"}}},
                {"gpuDevice", {{"group", "runtime"}, {"visible_when", {{"backend", "gpu"}}}}},
                {"threads", {{"group", "runtime"}}},
                {"language", {{"group", "runtime"}}},
                {"flashAttn", {{"group", "runtime"}}},
            }},
        }},
    };
}

nlohmann::json replicate_collections_options()
{
    std::string api_key, base_url;
    media::runtime_settings::merge_provider_credentials("replicate", false, api_key, base_url);

    std::vector<media::replicate_cli::CollectionInfo> cols;
    std::string err;
    media::replicate_cli::fetch_collections(api_key, base_url, cols, err);
    media::replicate_cli::sort_collections_alpha(cols);

    nlohmann::json options = nlohmann::json::array();
    for (const auto& c : cols)
        options.push_back({{"value", c.slug}, {"label", c.name.empty() ? c.slug : c.name}});
    return options;
}

nlohmann::json replicate_models_options(const std::string& collection)
{
    const std::string slug = collection.empty() ? "official" : collection;
    std::string api_key, base_url;
    media::runtime_settings::merge_provider_credentials("replicate", false, api_key, base_url);

    std::vector<media::replicate_cli::ModelInfo> models;
    std::string err;
    media::replicate_cli::fetch_collection_models(api_key, base_url, slug, models, err);
    media::replicate_cli::sort_models_alpha(models);

    nlohmann::json options = nlohmann::json::array();
    for (const auto& m : models)
        options.push_back({{"value", m.slug}, {"label", m.slug}});
    return options;
}

// Fields produced unconditionally by call_replicate_core  -  hiding them avoids
// confusing duplication in the property panel.
static const std::vector<std::string> k_replicate_builtin_image_keys = {
    "prompt", "image_input", "output_format", "aspect_ratio",
};

nlohmann::json replicate_provider_options_schema(const std::string& model,
                                                 const std::string& sub_kind)
{
    nlohmann::json schema_props = nlohmann::json::object();
    nlohmann::json required     = nlohmann::json::array();

    if (!model.empty()) {
        nlohmann::json flat;
        nlohmann::json req_arr;
        std::string err;
        if (media::replicate_cli::lookup_replicate_openapi_input_flat(model, flat, req_arr, err)
            && flat.is_object())
        {
            for (const auto& [key, val] : flat.items()) {
                const bool builtin = std::find(
                    k_replicate_builtin_image_keys.begin(),
                    k_replicate_builtin_image_keys.end(), key)
                    != k_replicate_builtin_image_keys.end();
                if (!builtin)
                    schema_props[key] = val;
            }
            if (req_arr.is_array()) {
                for (const auto& r : req_arr) {
                    if (!r.is_string()) continue;
                    const std::string k = r.get<std::string>();
                    const bool builtin = std::find(
                        k_replicate_builtin_image_keys.begin(),
                        k_replicate_builtin_image_keys.end(), k)
                        != k_replicate_builtin_image_keys.end();
                    if (!builtin)
                        required.push_back(k);
                }
            }
        }
    }

    // UI-only overlay: map known param kinds (image_path, prompt, ...) onto the
    // raw OpenAPI fields so the property panel renders native widgets.
    annotate_schema_param_kinds(schema_props);

    nlohmann::json result = {
        {"block",       ""},
        {"provider",    "replicate"},
        {"model",       model},
        {"sub_kind",    sub_kind.empty() ? "image" : sub_kind},
        {"value_field", "providerOptions"},
        {"schema", {
            {"type",       "object"},
            {"properties", schema_props},
            {"required",   required},
        }},
        {"ui", nlohmann::json::object()},
    };
    if (schema_props.empty())
        result["schema"] = nullptr;
    return result;
}

#if FEATURE_MCP_CLIENT

const nlohmann::json* find_mcp_probe_server(const nlohmann::json& probe, const std::string& name)
{
    if (!probe.contains("servers") || !probe["servers"].is_array())
        return nullptr;
    for (const auto& srv : probe["servers"]) {
        if (srv.is_object() && srv.value("name", std::string()) == name)
            return &srv;
    }
    return nullptr;
}

// Hot-path probe: never triggers a live subprocess spawn.  The cache is
// populated via the explicit "Refresh servers" user action or the first run of
// a profile server block.  UI dropdowns stay empty on a cold cache; this is
// intentional (avoids auto-launching arbitrary stdio processes on panel open).
nlohmann::json mcp_probe_for_ui()
{
    std::string note;
    return media::llm::mcp::mcp_probe_cached(/*allow_probe=*/false, &note, {});
}

// Load server names from the bundled dist/data/mcp.json (if present) without
// doing any MCP handshake.  Returns array of {value, label, group:"system"}.
nlohmann::json load_bundled_system_mcp_servers()
{
    namespace fs = std::filesystem;
    nlohmann::json result = nlohmann::json::array();

    const fs::path parent = media::app::exe_parent_directory();
    if (parent.empty())
        return result;

    const fs::path bundled = parent / "data" / "mcp.json";
    std::error_code ec;
    if (!fs::is_regular_file(bundled, ec))
        return result;

    std::ifstream f(bundled, std::ios::binary);
    if (!f)
        return result;

    nlohmann::json root;
    try {
        f >> root;
    } catch (...) {
        return result;
    }

    if (!root.is_object() || !root.contains("mcpServers") || !root["mcpServers"].is_object())
        return result;

    for (auto it = root["mcpServers"].begin(); it != root["mcpServers"].end(); ++it) {
        const std::string name = it.key();
        // Never expose __custom__ as a system server name (sentinel collision guard).
        if (name.empty() || name == "__custom__")
            continue;
        nlohmann::json row{{"value", name}, {"label", name}, {"group", "system"}};
        if (it.value().is_object() && it.value().contains("transport")
                && it.value()["transport"].is_string())
            row["transport"] = it.value()["transport"];
        result.push_back(std::move(row));
    }
    return result;
}

nlohmann::json mcp_servers_options()
{
    const nlohmann::json probe = mcp_probe_for_ui();

    nlohmann::json options = nlohmann::json::array();
    std::set<std::string> seen;

    // ── User / profile servers (from cached probe) ────────────────────────
    if (probe.contains("servers") && probe["servers"].is_array()) {
        for (const auto& srv : probe["servers"]) {
            if (!srv.is_object() || srv.value("skipped", false))
                continue;
            const std::string name = srv.value("name", std::string{});
            // Guard: user cannot have a server literally named __custom__
            // (would collide with the custom sentinel).
            if (name.empty() || name == "__custom__")
                continue;
            nlohmann::json row{{"value", name}, {"label", name}, {"group", "user"}};
            if (srv.contains("transport") && srv["transport"].is_string())
                row["transport"] = srv["transport"].get<std::string>();
            seen.insert(name);
            options.push_back(std::move(row));
        }
    }

    // ── System / bundled servers (dist/data/mcp.json) ────────────────────
    // User wins on name collision (system server already exists in profile).
    for (const auto& sys_row : load_bundled_system_mcp_servers()) {
        const std::string name = sys_row.value("value", std::string{});
        if (!name.empty() && !seen.count(name))
            options.push_back(sys_row);
    }

    // ── Custom sentinel ───────────────────────────────────────────────────
    // Always last so it never collides with a real server name from above.
    options.push_back({
        {"value", "__custom__"},
        {"label", "Custom\u2026"},
        {"group", "custom"},
    });

    return options;
}

nlohmann::json mcp_tools_options(const std::string& server)
{
    nlohmann::json options = nlohmann::json::array();
    // __custom__ tool catalog is populated via xbloxMcpProbeInline RPC, not here.
    if (server.empty() || server == "__custom__")
        return options;

    const nlohmann::json probe = mcp_probe_for_ui();
    const nlohmann::json* srv  = find_mcp_probe_server(probe, server);
    if (!srv || !srv->contains("tools") || !(*srv)["tools"].is_array())
        return options;

    for (const auto& tool : (*srv)["tools"]) {
        if (!tool.is_object())
            continue;
        const std::string name = tool.value("name", std::string{});
        if (name.empty())
            continue;
        nlohmann::json row{{"value", name}, {"label", name}};
        const std::string desc = tool.value("description", std::string{});
        if (!desc.empty())
            row["description"] = desc;
        options.push_back(std::move(row));
    }
    return options;
}

// Static schema for the inline custom MCP server configuration form.
// Field widgets mirror fetch (args_list rows, JSON headers/env).  Runtime
// normalize_mcp_server_config() still accepts legacy comma/JSON-string values.
nlohmann::json mcp_server_config_schema()
{
    const char* k_fetch_headers_desc =
        "Extra request headers — JSON object {\"Name\":\"value\"} or array of "
        "\"Name: value\" strings. Supports ${USER:…} for secrets. "
        "Merged after auth fields; duplicate keys override auth.";

    // Include "" so auto-detect (blank transport) still exposes HTTP + stdio fields.
    const auto k_stdio_transports = nlohmann::json::array({"", "stdio"});
    const auto k_http_transports  = nlohmann::json::array({"", "sse", "streamableHttp"});

    return nlohmann::json{
        {"block",       "mcpCall"},
        {"value_field", "serverConfig"},
        {"title",       "Connection"},
        {"schema", {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type",        "string"},
                    {"title",       "Transport"},
                    {"description", "Leave blank to auto-detect from command / url."},
                    {"enum",        nlohmann::json::array({"", "stdio", "sse", "streamableHttp"})},
                    {"default",     ""},
                }},
                {"command", {
                    {"type",        "string"},
                    {"title",       "Command"},
                    {"description", "Executable path (stdio). Supports ${USER:\u2026} substitution."},
                }},
                {"args", {
                    {"type",        "array"},
                    {"title",       "Arguments"},
                    {"description", "Argv tokens for stdio transport (one per row). Supports ${USER:\u2026}."},
                    {"items",       {{"type", "string"}}},
                    {"default",     nlohmann::json::array()},
                    {"x-xblox",     {{"kind", "args_list"}}},
                }},
                {"url", {
                    {"type",        "string"},
                    {"title",       "URL"},
                    {"description", "HTTP endpoint (sse / streamableHttp). Supports ${USER:\u2026}."},
                    {"format",      "uri"},
                }},
                {"auth", {
                    {"type",        "string"},
                    {"title",       "Authentication"},
                    {"description", "Authentication mode. none = no auth header added."},
                    {"enum",        nlohmann::json::array({"none", "bearer", "basic", "apikey"})},
                    {"default",     "none"},
                }},
                {"authToken", {
                    {"type",        "string"},
                    {"title",       "Token / API key"},
                    {"description", "Bearer token or API key value. Supports ${USER:\u2026}."},
                }},
                {"authUser", {
                    {"type",        "string"},
                    {"title",       "Username"},
                    {"description", "HTTP Basic auth username."},
                }},
                {"authPass", {
                    {"type",        "string"},
                    {"title",       "Password"},
                    {"description", "HTTP Basic auth password. Supports ${USER:\u2026}."},
                }},
                {"authHeader", {
                    {"type",        "string"},
                    {"title",       "API key header"},
                    {"description", "Header name used for apikey auth. Default: X-Api-Key."},
                    {"default",     "X-Api-Key"},
                }},
                {"headers", {
                    {"type",                 "object"},
                    {"title",                "Headers"},
                    {"description",          k_fetch_headers_desc},
                    {"additionalProperties", {{"type", "string"}}},
                    {"default",              nlohmann::json::object()},
                    {"x-xblox",              {{"kind", "json_value"}}},
                }},
                {"env", {
                    {"type",                 "object"},
                    {"title",                "Environment"},
                    {"description",          "Extra environment variables as a JSON object, e.g. {\"DEBUG\":\"1\"}."},
                    {"additionalProperties", {{"type", "string"}}},
                    {"default",              nlohmann::json::object()},
                    {"x-xblox",              {{"kind", "json_value"}}},
                }},
                {"tool_timeout", {
                    {"type",    "integer"},
                    {"title",   "Tool timeout (s)"},
                    {"default", 30},
                    {"minimum", 1},
                }},
            }},
        }},
        {"ui", {
            {"order", nlohmann::json::array({
                "type", "command", "args", "url",
                "auth", "authToken", "authUser", "authPass", "authHeader",
                "headers", "env", "tool_timeout"})},
            {"fields", {
                {"command",    {{"visible_when", {{"type", k_stdio_transports}}}}},
                {"args",       {{"visible_when", {{"type", k_stdio_transports}}}}},
                {"url",        {{"visible_when", {{"type", k_http_transports}}}}},
                {"auth",       {{"visible_when", {{"type", k_http_transports}}}}},
                {"authToken",  {{"visible_when", {{"type", k_http_transports}}}}},
                {"authUser",   {{"visible_when", {{"type", k_http_transports}}}}},
                {"authPass",   {{"visible_when", {{"type", k_http_transports}}}}},
                {"authHeader", {{"visible_when", {{"type", k_http_transports}}}}},
                {"headers",    {{"visible_when", {{"type", k_http_transports}}}}},
                {"env",        {{"visible_when", {{"type", k_stdio_transports}}}}},
            }},
        }},
    };
}

const nlohmann::json* find_mcp_inline_probe_server(const nlohmann::json& probe)
{
    if (!probe.contains("servers") || !probe["servers"].is_array())
        return nullptr;
    for (const auto& srv : probe["servers"]) {
        if (srv.is_object() && srv.value("name", std::string{}) == "__inline__")
            return &srv;
    }
    return nullptr;
}

const nlohmann::json* find_mcp_tool_row(const nlohmann::json& srv, const std::string& tool)
{
    if (!srv.contains("tools") || !srv["tools"].is_array())
        return nullptr;
    for (const auto& row : srv["tools"]) {
        if (row.is_object() && row.value("name", std::string{}) == tool)
            return &row;
    }
    return nullptr;
}

void fill_mcp_tool_options_schema(nlohmann::json& schema_props,
                                  nlohmann::json& required,
                                  const nlohmann::json& srv,
                                  const std::string& tool)
{
    const nlohmann::json* row = find_mcp_tool_row(srv, tool);
    if (!row)
        return;
    const nlohmann::json schema = media::llm::mcp::parameters_from_mcp_tool(*row);
    if (!schema.is_object())
        return;
    if (schema.contains("properties") && schema["properties"].is_object())
        schema_props = schema["properties"];
    if (schema.contains("required") && schema["required"].is_array())
        required = schema["required"];
}

nlohmann::json mcp_tool_options_schema(const std::string& server,
                                       const std::string& tool,
                                       const nlohmann::json& server_cfg = nlohmann::json::object())
{
    nlohmann::json schema_props = nlohmann::json::object();
    nlohmann::json required     = nlohmann::json::array();

    if (!tool.empty()) {
        if (server == "__custom__" && server_cfg.is_object() && !server_cfg.empty()) {
            nlohmann::json probe;
            const std::string slug = media::llm::mcp::mcp_inline_server_slug(server_cfg);
            if (media::llm::mcp::load_mcp_inline_probe_cache(slug, probe)) {
                if (const nlohmann::json* srv = find_mcp_inline_probe_server(probe))
                    fill_mcp_tool_options_schema(schema_props, required, *srv, tool);
            }
        } else if (!server.empty() && server != "__custom__") {
            const nlohmann::json probe = mcp_probe_for_ui();
            if (const nlohmann::json* srv = find_mcp_probe_server(probe, server))
                fill_mcp_tool_options_schema(schema_props, required, *srv, tool);
        }
    }

    annotate_schema_param_kinds(schema_props);

    nlohmann::json result = {
        {"block",       "mcpCall"},
        {"provider",    server},
        {"model",       tool},
        {"value_field", "arguments"},
        {"title",       "Tool arguments"},
        {"schema", {
            {"type",       "object"},
            {"properties", schema_props},
            {"required",   required},
        }},
        {"ui", nlohmann::json::object()},
    };
    if (schema_props.empty())
        result["schema"] = nullptr;
    return result;
}

#endif // FEATURE_MCP_CLIENT

nlohmann::json agent_path_tools_options()
{
#if defined(PM_PLUGIN_CHAT) && PM_PLUGIN_CHAT
    return pm::plugins::chat_host::path_tools_options();
#elif defined(PM_CHAT_SURFACE) && PM_CHAT_SURFACE
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tool : media::llm::path::tool_catalog()) {
        if (tool.name.empty())
            continue;
        out.push_back({
            {"id",    tool.name},
            {"label", tool.name},
        });
    }
    return out;
#else
    return nlohmann::json::array();
#endif
}

nlohmann::json agent_consent_ui_options()
{
    return nlohmann::json::array({
        option_row("win32", "Native dialog (default)"),
        option_row("auto", "Auto (cli / win32 / deny)"),
        option_row("preset", "Chat preset"),
        option_row("cli", "Stdio prompt"),
        option_row("auto-deny", "Deny every Ask"),
        option_row("auto-allow", "Allow every Ask (Yolo)"),
    });
}

nlohmann::json agent_runner_options()
{
    return nlohmann::json::array({
        option_row("native", "Native (in-process)"),
        option_row("claude-code", "Claude Code"),
        option_row("codex", "Codex"),
        // option_row("cursor", "Cursor"), // incomplete; keep for later
    });
}

nlohmann::json agent_preset_options()
{
    nlohmann::json options = nlohmann::json::array();
    std::vector<media::runtime_settings::ChatProviderPreset> presets;
    std::string err;
    if (!media::runtime_settings::load_chat_provider_presets(presets, err))
        return options;
    for (const auto& preset : presets) {
        const std::string name = preset.name.empty() ? preset.id : preset.name;
        if (name.empty())
            continue;
        options.push_back({
            {"value", name},
            {"label", preset.is_default ? name + " (default)" : name},
        });
    }
    return options;
}

nlohmann::json agent_router_options()
{
    return nlohmann::json::array({
        option_row("tanit", "tanit"),
        option_row("openrouter", "OpenRouter"),
        option_row("openai", "OpenAI"),
        option_row("gemini", "Google Gemini"),
        option_row("llama", "Llama.cpp (local)"),
        option_row("custom", "Custom (OpenAI-compatible)"),
    });
}

nlohmann::json agent_router_model_options(const std::string& router)
{
    std::string router_id = router;
    if (router_id.empty()) {
        media::runtime_settings::ChatProviderPreset preset;
        std::string err;
        if (media::runtime_settings::resolve_chat_provider_preset("", preset, err))
            router_id = preset.chat.router;
    }
    nlohmann::json options = nlohmann::json::array();
    if (router_id.empty())
        return options;
    if (router_id == "llama" || router_id == "local") {
        for (const auto& model : media::local_models::discover_llama_text_models(false)) {
            if (model.id.empty())
                continue;
            options.push_back({
                {"value", model.id},
                {"label", model.display_name.empty() ? model.id : model.display_name},
            });
        }
        return options;
    }
    if (router_id == "openrouter") {
        std::string err;
        std::vector<media::openrouter_cli::OpenRouterCatalogModelRow> models;
        if (media::openrouter_cli::list_openrouter_catalog_models("", "", models, err, false)) {
            for (const auto& model : models) {
                if (model.id.empty())
                    continue;
                options.push_back({
                    {"value", model.id},
                    {"label", model.name.empty() ? model.id : model.name},
                });
            }
        }
    }
    return options;
}

nlohmann::json ocr_provider_options()
{
    nlohmann::json options = nlohmann::json::array();
#if defined(FEATURE_PROVIDER_TANIT) && FEATURE_PROVIDER_TANIT
    options.push_back(option_row("tanit", "Tanit (proxy)"));
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA && defined(FEATURE_VLM) && FEATURE_VLM
    options.push_back(option_row("llama", "Llama.cpp VLM (local)"));
#endif
#if defined(FEATURE_ONNX) && FEATURE_ONNX
    options.push_back(option_row("onnx", "PaddleOCR ONNX (local)"));
#endif
#if defined(FEATURE_PROVIDER_REPLICATE) && FEATURE_PROVIDER_REPLICATE
    options.push_back(option_row("replicate", "Replicate"));
#endif
    return options;
}

nlohmann::json ocr_model_options(const std::string& provider)
{
    if (provider == "onnx" || provider == "paddle")
        return ocr_onnx_model_options();
    if (provider == "llama" || provider == "vlm")
        return ocr_vlm_model_options();
    return nlohmann::json::array();
}

// Native webcam modes for camera overlay capture (`cameraResolution`).
nlohmann::json video_camera_mode_options(const std::string& device)
{
    nlohmann::json options = nlohmann::json::array();
    options.push_back({
        {"value", "auto"},
        {"label", "Auto (match PiP size)"},
    });
    options.push_back({
        {"value", "max"},
        {"label", "Max native (incl. 4K when available)"},
    });
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    std::set<std::string> seen;
    for (const auto& m : pm::video::enumerate_device_modes(device)) {
        if (m.width <= 0 || m.height <= 0 || m.fps_den <= 0) continue;
        const int fps = std::max(1, (m.fps_num + m.fps_den / 2) / m.fps_den);
        const std::string value =
            std::to_string(m.width) + "x" + std::to_string(m.height) +
            "@" + std::to_string(fps);
        if (!seen.insert(value).second) continue;
        std::string label =
            std::to_string(m.width) + "×" + std::to_string(m.height) +
            " @ " + std::to_string(fps) + " fps";
        if (!m.format.empty())
            label += " · " + m.format;
        options.push_back({
            {"value", value},
            {"label", std::move(label)},
        });
    }
#else
    (void)device;
#endif
    return options;
}

// Monitors + visible windows as select options for video/screen input params.
// Same enumeration as `video stream` `/sources` and `videoListScreens`.
nlohmann::json video_screen_source_options()
{
    nlohmann::json options = nlohmann::json::array();
#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO
    options.push_back({
        {"value", "screen:current"},
        {"label", "Current screen (host window monitor)"},
        {"kind", "monitor"},
    });
    for (const auto& m : pm::video::ScreenSource::enumerate_monitors()) {
        std::string label = "Monitor [" + std::to_string(m.index) + "] " + m.name
                          + "  " + std::to_string(m.w) + "x" + std::to_string(m.h);
        if (m.is_primary)
            label += " (primary)";
        options.push_back({
            {"value", "screen:" + std::to_string(m.index)},
            {"label", std::move(label)},
            {"kind", "monitor"},
        });
    }
    for (const auto& w : pm::video::ScreenSource::enumerate_windows()) {
        const auto hwnd_value = reinterpret_cast<std::uintptr_t>(w.hwnd);
        const std::string spec = "screen:0:hwnd=" + std::to_string(hwnd_value);
        std::string title = w.title;
        if (title.size() > 56)
            title = title.substr(0, 53) + "...";
        std::string label = title.empty()
            ? ("Window hwnd=" + std::to_string(hwnd_value))
            : ("Window: " + title);
        options.push_back({
            {"value", spec},
            {"label", std::move(label)},
            {"kind", "window"},
            {"pid", w.pid},
        });
    }
#endif
    return options;
}

nlohmann::json command_id_options_from_cli()
{
    nlohmann::json options = nlohmann::json::array();
#if defined(_WIN32)
    const auto commands = pm::host::cli::registered_cli_commands();
#else
    const auto commands = pm::cli::registered_cli_commands();
#endif
    for (const auto& c : commands) {
        const char* id = c.id ? c.id : "";
        if (!id[0])
            continue;
        options.push_back({
            {"value", id},
            {"label", (c.label && c.label[0]) ? c.label : id},
            {"available", c.available},
        });
    }
    return options;
}

nlohmann::json command_id_options_from_app()
{
    nlohmann::json options = nlohmann::json::array();
    auto& reg = media::commands::registry();
    std::set<std::string> available;
#if defined(_WIN32)
    const auto app_cmds = pm::host::cli::registered_app_commands();
#else
    const auto app_cmds = pm::cli::registered_app_commands();
#endif
    for (const auto& c : app_cmds) {
        const char* id = c.id ? c.id : "";
        if (id[0] && c.available)
            available.insert(id);
    }
    for (const auto* d : reg.list_in_group_order()) {
        if (!d || !available.count(d->id))
            continue;
        options.push_back(reg.project_picker_option(*d, true));
        available.erase(d->id);
    }
    for (const auto& c : app_cmds) {
        const char* id = c.id ? c.id : "";
        if (!id[0] || !available.count(id))
            continue;
        options.push_back({
            {"value", id},
            {"label", (c.label && c.label[0]) ? c.label : id},
            {"available", c.available},
        });
    }
    return options;
}

nlohmann::json command_id_options_from_custom()
{
    nlohmann::json options = nlohmann::json::array();
#if defined(_WIN32)
    const auto custom_cmds = pm::host::cli::visible_custom_commands();
#else
    const auto custom_cmds = pm::cli::visible_custom_commands();
#endif
    for (const auto& c : custom_cmds) {
        if (c.id.empty())
            continue;
        options.push_back({
            {"value", c.id},
            {"label", c.label.empty() ? c.id : c.label},
            {"group", c.group},
            {"available", true},
        });
    }
    return options;
}

void ensure_shared_options_registered()
{
    static const bool once = [] {
        media::commands::options_registry().add("audio.devices.input", [] {
            return audio_device_options(false);
        });
        media::commands::options_registry().add("commands.app", [] {
            return command_id_options_from_app();
        });
        return true;
    }();
    (void)once;
}

} // namespace

nlohmann::json script_context_info(const std::string& path)
{
    return script_context_info_impl(path);
}

nlohmann::json resolve_options_path(const std::string& path)
{
    ensure_shared_options_registered();
    if (media::commands::options_registry().contains(path))
        return media::commands::options_registry().resolve(path);
    if (path == "audio.devices.output")
        return audio_device_options(true);
    if (path == "audio.devices.loopback")
        return audio_loopback_device_options();
    if (path == "audio.sample_rates")
        return audio_sample_rate_options();
    if (path == "audio.channels")
        return audio_channel_options();
    if (path == "video.fps")
        return video_fps_options();
    if (path == "video.bitrate_kbps")
        return video_bitrate_kbps_options();
    if (path == "video.camera.modes" || path.rfind("video.camera.modes.", 0) == 0) {
        std::string device;
        if (path.size() > sizeof("video.camera.modes.") - 1)
            device = path.substr(sizeof("video.camera.modes.") - 1);
        return video_camera_mode_options(device);
    }
    if (path == "video.sources.screens")
        return video_screen_source_options();
    if (path == "ocr.models.onnx")
        return ocr_onnx_model_options();
    if (path == "ocr.models.vlm")
        return ocr_vlm_model_options();
    if (path == "vision.models.vlm")
        return vision_vlm_model_options();
    if (path == "agent.path_tools")
        return agent_path_tools_options();
    if (path == "agent.consent_ui")
        return agent_consent_ui_options();
    if (path == "agent.runners")
        return agent_runner_options();
    if (path == "agent.presets")
        return agent_preset_options();
    if (path == "agent.routers")
        return agent_router_options();
    if (path == "agent.models" || path.rfind("agent.models.", 0) == 0) {
        std::string rest;
        if (path.size() > sizeof("agent.models.") - 1)
            rest = path.substr(sizeof("agent.models.") - 1);
        std::string preset;
        std::string router = rest;
        const auto dot = rest.find('.');
        if (dot != std::string::npos) {
            preset = rest.substr(0, dot);
            router = rest.substr(dot + 1);
        }
        if (router.empty() && !preset.empty()) {
            media::runtime_settings::ChatProviderPreset found;
            std::string err;
            if (media::runtime_settings::resolve_chat_provider_preset(preset, found, err))
                router = found.chat.router;
        }
        return agent_router_model_options(router);
    }
    if (path == "providers.ocr")
        return ocr_provider_options();
    if (path == "providers.ocr.models" || path.rfind("providers.ocr.models.", 0) == 0) {
        std::string provider;
        if (path.size() > sizeof("providers.ocr.models.") - 1)
            provider = path.substr(sizeof("providers.ocr.models.") - 1);
        return ocr_model_options(provider);
    }
    if (path == "providers.stt")
        return stt_provider_options();
    if (path == "providers.tts")
        return tts_provider_options();
    if (path == "providers.image")
        return image_provider_options();
    if (path == "providers.vision")
        return vision_provider_options();
    if (path == "providers.image.models" || path.rfind("providers.image.models.", 0) == 0) {
        std::string provider;
        if (path.size() > sizeof("providers.image.models.") - 1)
            provider = path.substr(sizeof("providers.image.models.") - 1);
        return image_model_options(provider);
    }
    if (path == "providers.vision.models" || path.rfind("providers.vision.models.", 0) == 0) {
        std::string provider;
        if (path.size() > sizeof("providers.vision.models.") - 1)
            provider = path.substr(sizeof("providers.vision.models.") - 1);
        return vision_model_options(provider);
    }
    if (path == "providers.stt.models" || path.rfind("providers.stt.models.", 0) == 0) {
        std::string provider;
        if (path.size() > sizeof("providers.stt.models.") - 1)
            provider = path.substr(sizeof("providers.stt.models.") - 1);
        return stt_model_options(provider);
    }
    if (path == "providers.tts.models" || path.rfind("providers.tts.models.", 0) == 0) {
        std::string provider;
        if (path.size() > sizeof("providers.tts.models.") - 1)
            provider = path.substr(sizeof("providers.tts.models.") - 1);
        return tts_model_options(provider);
    }
    if (path == "providers.tts.voices" || path.rfind("providers.tts.voices.", 0) == 0) {
        std::string model;
        if (path.size() > sizeof("providers.tts.voices.") - 1)
            model = path.substr(sizeof("providers.tts.voices.") - 1);
        return openrouter_tts_voice_options(model);
    }
    if (path == "providers.replicate.collections")
        return replicate_collections_options();
    // providers.replicate.models  or  providers.replicate.models.{collection}
    if (path == "providers.replicate.models" || path.rfind("providers.replicate.models.", 0) == 0) {
        std::string collection;
        if (path.size() > sizeof("providers.replicate.models.") - 1)
            collection = path.substr(sizeof("providers.replicate.models.") - 1);
        return replicate_models_options(collection);
    }
    if (path == "commands.custom")
        return command_id_options_from_custom();
    if (path == "commands.cli")
        return command_id_options_from_cli();
    if (path == "commands.app")
        return command_id_options_from_app();
    if (path.rfind("xblox.scriptContext.", 0) == 0)
        return xblox_script_context_options(path.substr(sizeof("xblox.scriptContext.") - 1));
#if FEATURE_MCP_CLIENT
    if (path == "mcp.servers")
        return mcp_servers_options();
    if (path == "mcp.tools" || path.rfind("mcp.tools.", 0) == 0) {
        std::string server;
        if (path.size() > sizeof("mcp.tools.") - 1)
            server = path.substr(sizeof("mcp.tools.") - 1);
        return mcp_tools_options(server);
    }
#endif
    return nlohmann::json::array();
}

nlohmann::json resolve_request(const nlohmann::json& request)
{
    const std::string path = request.value("path", std::string{});
    return {
        {"path", path},
        {"options", resolve_options_path(path)},
    };
}

nlohmann::json resolve_provider_options_schema(const nlohmann::json& request)
{
    const std::string schema_path = request.value("schema_path", std::string{});
    const std::string provider    = request.value("provider",    std::string{});
    const std::string model       = request.value("model",       std::string{});
    const nlohmann::json server_cfg = request.value("serverConfig", nlohmann::json::object());

    // Route by schema_path prefix segments (preferred, provider-agnostic).
    // Any block declaring schema_path "providers.X" gets X's schema regardless
    // of block kind. Unknown paths return null schema silently.
    auto path_starts_with = [](const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    };

    if (path_starts_with(schema_path, "providers.whisper") || provider == "whisper")
        return whisper_provider_options_schema("", provider, model);

    if (path_starts_with(schema_path, "providers.replicate") || provider == "replicate") {
        // Derive sub-kind from trailing segment: providers.replicate.image -> "image"
        std::string sub_kind;
        const std::string replicate_prefix = "providers.replicate";
        if (schema_path.size() > replicate_prefix.size() + 1)
            sub_kind = schema_path.substr(replicate_prefix.size() + 1);
        return replicate_provider_options_schema(model, sub_kind);
    }

#if FEATURE_MCP_CLIENT
    // "mcp.server" → static schema for the inline custom server config editor.
    if (schema_path == "mcp.server")
        return mcp_server_config_schema();
    // "mcp.tools" (or "mcp.tools.{server}") → per-tool inputSchema.
    if (path_starts_with(schema_path, "mcp.tools"))
        return mcp_tool_options_schema(provider, model, server_cfg);
#endif

    return empty_provider_options_schema("", provider, model);
}

nlohmann::json bootstrap_payload()
{
    return {
        {"optionsByPath", {
            {"audio.devices.input", resolve_options_path("audio.devices.input")},
            {"audio.devices.output", resolve_options_path("audio.devices.output")},
            {"audio.devices.loopback", resolve_options_path("audio.devices.loopback")},
            {"providers.stt", resolve_options_path("providers.stt")},
            {"providers.tts", resolve_options_path("providers.tts")},
            {"providers.image", resolve_options_path("providers.image")},
            {"providers.vision", resolve_options_path("providers.vision")},
            {"audio.sample_rates", resolve_options_path("audio.sample_rates")},
            {"audio.channels", resolve_options_path("audio.channels")},
            {"video.fps", resolve_options_path("video.fps")},
            {"video.bitrate_kbps", resolve_options_path("video.bitrate_kbps")},
            {"video.sources.screens", resolve_options_path("video.sources.screens")},
            {"ocr.models.onnx", resolve_options_path("ocr.models.onnx")},
            {"ocr.models.vlm", resolve_options_path("ocr.models.vlm")},
            {"vision.models.vlm", resolve_options_path("vision.models.vlm")},
            {"agent.path_tools", resolve_options_path("agent.path_tools")},
            {"agent.consent_ui", resolve_options_path("agent.consent_ui")},
            {"agent.runners", resolve_options_path("agent.runners")},
            {"agent.presets", resolve_options_path("agent.presets")},
            {"agent.routers", resolve_options_path("agent.routers")},
            {"providers.ocr", resolve_options_path("providers.ocr")},
            {"commands.custom", resolve_options_path("commands.custom")},
            {"commands.cli", resolve_options_path("commands.cli")},
            {"commands.app", resolve_options_path("commands.app")},
        }},
    };
}

nlohmann::json resolve_mcp_probe_inline(const nlohmann::json& request)
{
#if !FEATURE_MCP_CLIENT
    return {
        {"ok",    false},
        {"error", "MCP client disabled in this build"},
        {"tools", nlohmann::json::array()},
    };
#else
    const nlohmann::json server_cfg = request.value("serverConfig", nlohmann::json::object());
    if (!server_cfg.is_object() || server_cfg.empty()) {
        return {
            {"ok",    false},
            {"error", "serverConfig is empty or not a JSON object"},
            {"tools", nlohmann::json::array()},
        };
    }

    const bool force      = request.value("force", false);
    const int  timeout_ms = request.value("timeoutMs", 0); // 0 → mcp_default_timeout_ms()

    // Compute slug before probing so we can return it for UI cache-key purposes.
    const std::string slug = media::llm::mcp::mcp_inline_server_slug(server_cfg);

    const nlohmann::json probe_cfg = media::xblox::blocks::normalize_mcp_server_config(
        nlohmann::json(server_cfg));

    nlohmann::json probe;
    try {
        probe = media::llm::mcp::mcp_probe_inline_server(probe_cfg, force, timeout_ms);
    } catch (const std::exception& e) {
        return {
            {"ok",    false},
            {"error", std::string("probe threw: ") + e.what()},
            {"tools", nlohmann::json::array()},
            {"slug",  slug},
        };
    } catch (...) {
        return {
            {"ok",    false},
            {"error", "probe threw unknown exception"},
            {"tools", nlohmann::json::array()},
            {"slug",  slug},
        };
    }

    // Find the single "__inline__" server entry in the probe result.
    const nlohmann::json* srv = nullptr;
    if (probe.contains("servers") && probe["servers"].is_array()) {
        for (const auto& s : probe["servers"]) {
            if (s.is_object() && s.value("name", std::string{}) == "__inline__") {
                srv = &s;
                break;
            }
        }
    }

    if (!srv) {
        return {
            {"ok",    false},
            {"error", "probe returned no server entry"},
            {"tools", nlohmann::json::array()},
            {"slug",  slug},
        };
    }

    if ((*srv).value("skipped", false)) {
        return {
            {"ok",    false},
            {"error", (*srv).value("skip_reason", std::string{"probe skipped"})},
            {"tools", nlohmann::json::array()},
            {"slug",  slug},
        };
    }

    if (!(*srv).value("handshake_ok", false)) {
        return {
            {"ok",    false},
            {"error", (*srv).value("handshake_error", std::string{"handshake failed"})},
            {"tools", nlohmann::json::array()},
            {"slug",  slug},
        };
    }

    if (!(*srv).value("tools_list_ok", false)) {
        return {
            {"ok",    false},
            {"error", (*srv).value("tools_list_error", std::string{"tools/list failed"})},
            {"tools", nlohmann::json::array()},
            {"slug",  slug},
        };
    }

    // Build tools-options array in the same shape as mcp_tools_options().
    nlohmann::json tools_options = nlohmann::json::array();
    if ((*srv).contains("tools") && (*srv)["tools"].is_array()) {
        for (const auto& tool : (*srv)["tools"]) {
            if (!tool.is_object())
                continue;
            const std::string name = tool.value("name", std::string{});
            if (name.empty())
                continue;
            nlohmann::json row{{"value", name}, {"label", name}};
            const std::string desc = tool.value("description", std::string{});
            if (!desc.empty())
                row["description"] = desc;
            tools_options.push_back(std::move(row));
        }
    }

    return {
        {"ok",    true},
        {"tools", std::move(tools_options)},
        {"slug",  slug},
    };
#endif
}

nlohmann::json resolve_mcp_refresh_catalog(const nlohmann::json& /*request*/)
{
#if !FEATURE_MCP_CLIENT
    return {{"ok", false}, {"error", "MCP client disabled in this build"}};
#else
    // Trigger a full live probe of all profile servers (allow_probe=true).
    // The result is written to the disk cache so subsequent mcp_probe_for_ui()
    // calls (allow_probe=false) will see fresh data.
    std::string note;
    const nlohmann::json probe =
        media::llm::mcp::mcp_probe_cached(/*allow_probe=*/true, &note, {});

    int server_count = 0;
    if (probe.contains("servers") && probe["servers"].is_array()) {
        for (const auto& srv : probe["servers"]) {
            if (srv.is_object() && !srv.value("skipped", false))
                ++server_count;
        }
    }

    return {
        {"ok",          true},
        {"serverCount", server_count},
        {"note",        note},
    };
#endif
}

} // namespace media::xblox::blocks::ui
