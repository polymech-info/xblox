#pragma once
// block_provider.hpp  -  credential resolution for provider-aware xblox blocks.
//
// Blocks that call LLM / STT / TTS APIs only override *routing choices*
// (router, model, voice)  -  never API keys.  Credentials always come from
// settings.json["providers"] via merge_provider_credentials.
//
// Usage (STT block example):
//
//   auto stt = bp::resolve_stt({ au_jstr(block, "provider"),
//                                au_jstr(block, "model") });
//   if (!stt.error.empty()) { /* emit error event and return */ }
//   transcript = pm::stt::proxy_stt_transcribe(wav_path, stt.base_url,
//                                              stt.api_key, stt.model);

#include "audio/audio_constants.hpp"
#include "core/settings_runtime.hpp"

#include <string>

namespace media::xblox::bp {

// ── Resolved credential bundles ───────────────────────────────────────────────

struct ResolvedLlm {
    std::string router;
    std::string model;
    std::string api_key;
    std::string base_url;
    std::string error; // non-empty -> block should emit error event and return
};

struct ResolvedStt {
    std::string provider;
    std::string model;
    std::string api_key;
    std::string base_url;
    std::string error;
};

struct ResolvedTts {
    std::string provider;
    std::string model;
    std::string voice;
    std::string api_key;
    std::string base_url;
    std::string error;
};

// ── Override bundles (filled from block JSON fields) ─────────────────────────

struct LlmOverride  { std::string router; std::string model; };
struct AudioOverride { std::string provider; std::string model; std::string voice; };

// ── Resolvers ─────────────────────────────────────────────────────────────────

inline ResolvedLlm resolve_llm(const LlmOverride& ovr)
{
    media::runtime_settings::ChatProviderSettings chat;
    std::string err;
    if (!media::runtime_settings::load_chat_provider(chat, err))
        return {{}, {}, {}, {}, "block_provider: load settings failed: " + err};

    ResolvedLlm r;
    r.router = !ovr.router.empty() ? ovr.router : chat.router;
    r.model  = !ovr.model.empty()  ? ovr.model  : chat.model;

    if (r.router.empty()) {
        r.error = "no LLM router configured "
                  "(set in App Settings -> Chat, or pass `router` in the block)";
        return r;
    }

    media::runtime_settings::merge_provider_credentials(r.router, false, r.api_key, r.base_url);
    if (r.api_key.empty()) {
        r.error = "no API key for router '" + r.router + "' "
                  "(configure it in App Settings -> API Providers)";
    }
    return r;
}

inline ResolvedStt resolve_stt(const AudioOverride& ovr)
{
    media::runtime_settings::ChatProviderSettings chat;
    std::string err;
    if (!media::runtime_settings::load_chat_provider(chat, err))
        return {{}, {}, {}, {}, "block_provider: load settings failed: " + err};

    ResolvedStt r;
    r.provider = !ovr.provider.empty() ? ovr.provider : chat.stt_provider;

    if (r.provider.empty()) {
        r.error = "no STT provider configured "
                  "(set in App Settings -> Chat -> Voice & Audio, or pass `provider`)";
        return r;
    }

    // Only inherit the saved chat.stt_model when it belongs to the *active*
    // provider. Otherwise (e.g. a block overrides provider to something other
    // than the configured one) fall back to that provider's default, so we never
    // feed an ElevenLabs/pixlwiz model id to whisper (or vice versa).
    const bool saved_model_matches =
        !chat.stt_model.empty() && (chat.stt_provider.empty() || chat.stt_provider == r.provider);

    // Whisper is fully local  -  no API key / base URL, model is a ggml model id.
    if (r.provider == "whisper") {
        r.model = !ovr.model.empty()    ? ovr.model
                  : saved_model_matches ? chat.stt_model
                  : std::string("base.en");
        return r;
    }
    if (pm::audio::constants::is_windows_ai_stt_provider(r.provider)) {
        r.model.clear();
        return r;
    }

    r.model    = !ovr.model.empty()    ? ovr.model
                 : saved_model_matches ? chat.stt_model
                 : (r.provider == "openai"
                        ? std::string(pm::audio::constants::k_openai_stt_model)
                        : (r.provider == "openrouter"
                               ? std::string(pm::audio::constants::k_openrouter_stt_model)
                               : std::string("tanit-speech-to-text")));

    media::runtime_settings::merge_provider_credentials(r.provider, false, r.api_key, r.base_url);
    pm::audio::constants::ensure_speech_base(r.provider, r.base_url);
    if (r.api_key.empty()) {
        r.error = "no API key for STT provider '" + r.provider + "' "
                  "(configure it in App Settings -> API Providers)";
    }
    return r;
}

inline ResolvedTts resolve_tts(const AudioOverride& ovr)
{
    media::runtime_settings::ChatProviderSettings chat;
    std::string err;
    if (!media::runtime_settings::load_chat_provider(chat, err))
        return {{}, {}, {}, {}, {}, "block_provider: load settings failed: " + err};

    ResolvedTts r;
    r.provider = !ovr.provider.empty() ? ovr.provider : chat.tts_provider;
    r.model    = !ovr.model.empty()    ? ovr.model
                 : !chat.tts_model.empty() ? chat.tts_model
                 : (r.provider == "openai"
                        ? std::string(pm::audio::constants::k_openai_tts_model)
                        : (r.provider == "openrouter"
                               ? std::string(pm::audio::constants::k_openrouter_tts_model)
                               : (r.provider == "tanit" ? std::string("tanit-speech") : std::string{})));
    r.voice    = pm::audio::constants::speech_tts_voice(
        r.provider, !ovr.voice.empty() ? ovr.voice : chat.tts_voice_id);

    if (r.provider.empty()) {
        r.error = "no TTS provider configured "
                  "(set in App Settings -> Chat -> Voice & Audio, or pass `provider`)";
        return r;
    }

    // Local engines — no API key or base_url needed.
    if (r.provider == "vibevoice" || r.provider == "moss") return r;

    media::runtime_settings::merge_provider_credentials(r.provider, false, r.api_key, r.base_url);
    pm::audio::constants::ensure_speech_base(r.provider, r.base_url);
    if (r.api_key.empty()) {
        r.error = "no API key for TTS provider '" + r.provider + "' "
                  "(configure it in App Settings -> API Providers)";
    }
    return r;
}

} // namespace media::xblox::bp
