// XBlox audioTtsScripted — SRT/VTT/JSON cues → 48 kHz PCM voice-over bed.
// I/O only. Merge / place / overflow live in pm::tts::voiceover.
// Do not hang this on audioSpeak.

#include "blocks/audio_blocks_tts_scripted.hpp"

#include "logging/logger.h"

#include <string>

#if defined(FEATURE_STT) && FEATURE_STT

#include "audio/audio_constants.hpp"
#include "audio/tts/elevenlabs_tts.hpp"
#include "audio/tts/proxy_tts.hpp"
#include "audio/tts/voiceover.hpp"
#include "blocks/block_provider.hpp"
#include "core/cli_cancel.hpp"
#include "win/overlay_hud.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <vector>

#endif

namespace media::xblox::blocks {
namespace {

#if defined(FEATURE_STT) && FEATURE_STT

namespace C = pm::audio::constants;
namespace bp = media::xblox::bp;
namespace vo = pm::tts::voiceover;

BlockResult audio_tts_scripted_block(BlockIO& io)
{
    const std::string input = io.str("input");
    const std::string out_path = io.str("outputPath");
    if (out_path.empty()) {
        io.error("audioTtsScripted",
                 "audioTtsScripted: outputPath is required", 2);
        return BlockResult::Error;
    }

    std::vector<pm::subs::Cue> cues;
    std::string cue_err;
    if (!vo::load_cues(input, cues, cue_err)) {
        io.error("audioTtsScripted",
                 "audioTtsScripted: " + cue_err, 1);
        return BlockResult::Error;
    }

    const auto tts = bp::resolve_tts({
        io.str("provider"), io.str("model"), io.str("voice")
    });
    if (!tts.error.empty()) {
        io.error("audioTtsScripted", "audioTtsScripted: " + tts.error, 1);
        return BlockResult::Error;
    }

    vo::Options opt;
    const std::string overflow = io.str("overflow");
    opt.overflow = (overflow == "truncate")
        ? vo::OverflowMode::Truncate : vo::OverflowMode::Resolve;
    opt.min_inter_gap_ms = static_cast<int>(io.i64("minGapMs", C::k_voiceover_min_inter_gap_ms));
    opt.max_stretch = io.f64("maxStretchPercent", C::k_voiceover_max_stretch * 100.0) / 100.0;
    if (opt.max_stretch < 0.0)
        opt.max_stretch = 0.0;
    if (opt.max_stretch > 0.25)
        opt.max_stretch = 0.25;
    opt.max_utterance_ms = static_cast<int>(io.i64("maxUtteranceMs", C::k_voiceover_max_utterance_ms));
    opt.speed = static_cast<float>(io.f64("speed", 1.0));
    opt.seed = io.i64("seed", -1);
    opt.alignment = io.flag("alignment", false);
    opt.provider = tts.provider;
    opt.model = tts.model;
    opt.voice = tts.voice;
    opt.cache_extra = std::to_string(io.f64("stability", C::k_voiceover_stability))
        + "|" + std::to_string(io.f64("similarityBoost", C::k_voiceover_similarity_boost))
        + "|" + std::to_string(io.f64("style", C::k_voiceover_style))
        + "|" + (io.flag("speakerBoost", C::k_voiceover_speaker_boost) ? "1" : "0")
        + "|" + io.str("textNormalization");
    const std::string cache_dir = io.str("cacheDir");
    if (!cache_dir.empty())
        opt.cache_dir = cache_dir;

    vo::TtsCapabilities caps;
    if (tts.provider == "elevenlabs") {
        caps.speed_control = true;
        caps.request_stitching = true;
        caps.character_alignment = false;
    }

    std::unique_ptr<media::win::OverlayJob> overlay;
    if (io.flag("hud", false)) {
        overlay = std::make_unique<media::win::OverlayJob>("Voice-over");
        overlay->busy("scripted TTS");
    }

    std::string format = "pcm_22050";
    bool pcm_ok = true;

    auto synth = [&](const vo::SynthesizeRequest& req) -> vo::VoiceOverTake {
        vo::VoiceOverTake take;
        take.generation_speed = req.speed;
        take.seed = req.seed;
        std::vector<std::uint8_t> bytes;
        std::string used_fmt = "mp3";

        if (tts.provider == "elevenlabs") {
            pm::tts::ElevenLabsTTSConfig cfg;
            cfg.api_key = tts.api_key;
            if (!tts.voice.empty())
                cfg.voice_id = tts.voice;
            if (!tts.model.empty())
                cfg.model_id = tts.model;
            cfg.chunk_sentences = false;
            cfg.send_voice_settings = io.flag("sendVoiceSettings", true);
            cfg.stability = io.f64("stability", C::k_voiceover_stability);
            cfg.similarity_boost = io.f64("similarityBoost", C::k_voiceover_similarity_boost);
            cfg.style = io.f64("style", C::k_voiceover_style);
            cfg.use_speaker_boost = io.flag("speakerBoost", C::k_voiceover_speaker_boost);
            {
                std::string norm = io.str("textNormalization");
                if (norm.empty())
                    norm = "auto";
                cfg.apply_text_normalization = norm;
            }
            cfg.speed = req.speed;
            cfg.seed = req.seed;
            cfg.context_previous_text = req.previous_text;
            cfg.context_next_text = req.next_text;
            cfg.context_previous_request_ids = req.previous_request_ids;
            cfg.context_next_request_ids = req.next_request_ids;
            cfg.should_cancel = []() -> bool { return media::cli::cancel_requested(); };

            auto try_format = format;
            if (!pcm_ok)
                try_format = C::k_elevenlabs_output_format;
            cfg.output_format = try_format;
            try {
                const auto result = pm::tts::elevenlabs_tts_synthesize_result(req.text, cfg);
                bytes = std::move(result.audio);
                take.request_id = result.request_id;
                used_fmt = try_format;
            } catch (const std::exception& ex) {
                if (pcm_ok && try_format.rfind("pcm_", 0) == 0) {
                    logger::info(std::string("[audioTtsScripted] pcm format unavailable, falling back to mp3: ")
                                 + ex.what());
                    pcm_ok = false;
                    format = C::k_elevenlabs_output_format;
                    cfg.output_format = format;
                    const auto result = pm::tts::elevenlabs_tts_synthesize_result(req.text, cfg);
                    bytes = std::move(result.audio);
                    take.request_id = result.request_id;
                    used_fmt = format;
                } else {
                    throw;
                }
            }
        } else {
            bytes = pm::tts::proxy_tts_synthesize(
                req.text, tts.base_url, tts.api_key, tts.model, tts.voice, "mp3");
            used_fmt = "mp3";
        }

        std::string dec_err;
        if (!vo::decode_tts_bytes(bytes, used_fmt, take.pcm, dec_err))
            throw std::runtime_error(dec_err);
        take.duration_ms = take.pcm.empty()
            ? 0
            : (static_cast<std::int64_t>(take.pcm.size()) * 1000)
                / C::k_voiceover_sample_rate;
        return take;
    };

    vo::OverflowReport report;
    std::string render_err;
    try {
        if (!vo::render_timeline(cues, opt, caps, synth, out_path, report, render_err)) {
            if (overlay)
                overlay->error(render_err);
            io.error("audioTtsScripted", "audioTtsScripted: " + render_err, 1);
            return BlockResult::Error;
        }
    } catch (const std::exception& ex) {
        if (overlay)
            overlay->error(ex.what());
        io.error("audioTtsScripted",
                 std::string("audioTtsScripted: synthesis failed: ") + ex.what(), 1);
        return BlockResult::Error;
    }

    if (overlay)
        overlay->completed(std::to_string(report.utterances) + " utterances");

    auto ev = io.ok("audioTtsScripted",
        "voice-over " + std::to_string(report.utterances) + " utterances → " + out_path);
    ev.set("path", out_path)
      .set("provider", tts.provider)
      .set("model", tts.model)
      .set("utterances", static_cast<long long>(report.utterances))
      .set("natural", static_cast<long long>(report.natural))
      .set("usedGap", static_cast<long long>(report.used_gap))
      .set("stretched", static_cast<long long>(report.stretched))
      .set("regenerated", static_cast<long long>(report.regenerated))
      .set("unresolved", static_cast<long long>(report.unresolved));
    const auto payload = vo::report_to_json(report);
    ev.set("report", payload.dump());
    if (io.flag("json", false))
        ev.result(Value::from_json(payload), ParamKind::json_value);
    else
        ev.result(out_path, ParamKind::audio_path);
    return BlockResult::Ok;
}

#endif // FEATURE_STT

} // namespace

void register_audio_tts_scripted_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_STT) && FEATURE_STT
    namespace C = pm::audio::constants;

    registry["audioTtsScripted"] = bdb(
        "audioTtsScripted", audio_tts_scripted_block,
        "TTS Scripted", "Audio",
        "Place scripted TTS from an SRT/VTT file or transcribe JSON onto a 48 kHz PCM timeline. "
        "Merges cues with a punctuation-aware score, then resolves overflow with gap, mild stretch, "
        "and optional ElevenLabs speed regeneration. Does not play audio (use audioPlay).",
        {{"kind","audioTtsScripted"}, {"input",""}, {"outputPath",""},
         {"provider",""}, {"model",""}, {"voice",""},
         {"overflow","resolve"}, {"maxStretchPercent",12.0}, {"minGapMs",80},
         {"maxUtteranceMs",8000}, {"speed",1.0}, {"seed",-1},
         {"alignment",false}, {"json",false}, {"hud",false},
         {"sendVoiceSettings", true},
         {"stability", C::k_voiceover_stability},
         {"similarityBoost", C::k_voiceover_similarity_boost},
         {"style", C::k_voiceover_style},
         {"speakerBoost", true},
         {"textNormalization", "auto"},
         {"storeAs","voiceoverPath"}})
        .params({
            pd_file("input").in().lbl("Script").prev().dflt("")
                .desc("SRT/VTT/JSON path, or PREVIOUS transcribe JSON. Default: empty → PREVIOUS."),
            pd_outpath("outputPath").grp("output_file").lbl("Output WAV").req().dflt("")
                .desc("Canonical 48 kHz mono PCM WAV. Required."),
            pd_provider().grp("voice_model").dflt("")
                .ui({{"options_path", "providers.tts"}})
                .desc("TTS provider. Default: empty → App Settings tts_provider."),
            pd_model().grp("voice_model").dflt("")
                .dyn({{"kind", "options_list"},
                      {"options_path_template", "providers.tts.models.{provider}"}})
                .desc("TTS model. Default: empty → App Settings."),
            pd_str("voice").adv().grp("voice_model").dflt("")
                .desc("Voice id. Default: empty → App Settings tts_voice_id."),
            pd_enum("overflow", {"resolve", "truncate"}).adv().grp("timeline").dflt("resolve")
                .desc("resolve = gap → stretch ≲12% → speed regen ≤1.2 → overlap+warn. "
                      "truncate = hard-cut (power user)."),
            pd_float("maxStretchPercent").adv().grp("timeline").dflt(12.0).range(0.0, 25.0)
                .desc("Max DSP time-compress percent before regeneration. Default: 12."),
            pd_ms("minGapMs").adv().grp("timeline").dflt(80).range(0, 500)
                .desc("Minimum inter-utterance silence. Default: 80."),
            pd_ms("maxUtteranceMs").adv().grp("timeline").dflt(8000).range(500, 25000)
                .desc("Do not merge cues past this estimated duration. Default: 8000."),
            pd_float("speed").adv().grp("elevenlabs").dflt(1.0).range(0.7, 1.2)
                .desc("Initial ElevenLabs speed (0.7–1.2). Default: 1.0."),
            pd_int("seed").adv().grp("elevenlabs").dflt(-1)
                .desc("Best-effort seed. Default: -1 → derived from text+voice."),
            pd_bool("alignment").adv().grp("elevenlabs").dflt(false)
                .desc("Reserved: character alignment. V1 places from PCM duration only."),
            pd_bool("sendVoiceSettings").adv().grp("elevenlabs").dflt(true)
                .desc("Send voice_settings. Default: true."),
            pd_float("stability").adv().grp("elevenlabs").dflt(C::k_voiceover_stability).range(0.0, 1.0)
                .desc("Lower is more expressive (chat TTS uses 0.9, which sounds flat on short cues). Default: 0.45."),
            pd_float("similarityBoost").adv().grp("elevenlabs").lbl("Similarity boost")
                .dflt(C::k_voiceover_similarity_boost).range(0.0, 1.0)
                .desc("Stay on-voice. Default: 0.80."),
            pd_float("style").adv().grp("elevenlabs").dflt(C::k_voiceover_style).range(0.0, 1.0)
                .desc("Light style exaggeration for narration. Default: 0.20. High values can add artifacts."),
            pd_bool("speakerBoost").adv().grp("elevenlabs").dflt(true)
                .desc("ElevenLabs use_speaker_boost. Default: true."),
            pd_enum("textNormalization", {"auto", "on", "off"}).adv().grp("elevenlabs").dflt("auto")
                .desc("apply_text_normalization. Default: auto."),
            pd_str("cacheDir").adv().grp("output").dflt("")
                .desc("Take cache directory. Default: empty → <output>/.voiceover-cache."),
            pd_bool("json").adv().grp("output").dflt(false)
                .desc("Return overflow-report JSON instead of the WAV path."),
            pd_bool("hud").grp("hud").dflt(false)
                .desc("Show overlay HUD while generating. Default off."),
            pd_store_as().dflt("voiceoverPath")
                .desc("Variable for PREVIOUS. Default: voiceoverPath."),
        })
        .dynamic_primary_output(
            ParamKind::audio_path,
            {ParamKind::audio_path, ParamKind::json_value},
            "Voice-over")
        .exec({false, true, true});
#else
    (void)registry;
#endif
}

} // namespace media::xblox::blocks
