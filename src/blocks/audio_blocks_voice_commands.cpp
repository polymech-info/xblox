// XBlox Voice Command Center — voiceListen (Still / Start / Stop / Status / Replay).
// Named instances like videoCapture Start/Stop. On a wake+phrase match the
// handler runs `items` (videoDetect filter → Then). Live capture stays in
// VoiceListenHost; this file only maps BlockIO onto that host.

#include "blocks/audio_blocks_voice_commands.hpp"

#include "constants.hpp"
#include "logging/logger.h"
#include "string/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#if defined(FEATURE_VOICE_COMMANDS) && FEATURE_VOICE_COMMANDS && PM_HAS_AUDIO_IO

#include "audio/voice/voice_envelope.hpp"
#include "audio/voice/voice_match.hpp"
#include "audio/voice/voice_session.hpp"
#include "core/command_control.hpp"
#include "core/voice_commands.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#endif

namespace media::xblox::blocks {
namespace {

using media::strutil::lower_ascii;

#if defined(FEATURE_VOICE_COMMANDS) && FEATURE_VOICE_COMMANDS && PM_HAS_AUDIO_IO

using pm::audio::voice::CancelEvent;
using pm::audio::voice::ListenOptions;
using pm::audio::voice::MatchResult;
using pm::audio::voice::PipelineState;
using pm::audio::voice::Transcript;
using pm::audio::voice::TriggerEvent;
using pm::audio::voice::VoiceListenHost;
using pm::audio::voice::VoiceListenRoutes;
using pm::audio::voice::normalize_voice_text;
using pm::audio::voice::pipeline_state_name;
using pm::audio::voice::trigger_envelope_json;

std::string compact_ascii(std::string s)
{
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return std::isspace(c) != 0; }),
            s.end());
    return s;
}

bool phrase_matches(const std::string& phrase,
                    const std::string& spoken,
                    const media::voice_commands::Match& catalog)
{
    if (phrase.empty())
        return true;
    const std::string want = normalize_voice_text(phrase);
    const std::string got = normalize_voice_text(spoken);
    if (!want.empty() && (got == want || compact_ascii(got) == compact_ascii(want)))
        return true;
    if (!catalog.id.empty()
        && lower_ascii(catalog.id) == lower_ascii(phrase))
        return true;
    if (!catalog.label.empty()
        && normalize_voice_text(catalog.label) == want)
        return true;
    const auto wanted = media::voice_commands::find_voice_command(phrase);
    return wanted.kind != media::voice_commands::Kind::None
        && !wanted.id.empty()
        && wanted.id == catalog.id;
}

nlohmann::json enrich_trigger(const TriggerEvent& event, bool dispatch)
{
    media::voice_commands::remember_voice_utterance(
        event.wake_prefix, event.text,
        event.raw_text.empty() ? event.text : event.raw_text);
    nlohmann::json out = trigger_envelope_json(event);
    out["schema"] = pm::audio::voice::k_trigger_envelope_type;
    const auto match = media::voice_commands::find_voice_command(event.text);
    bool executed = false;
    if (match.kind != media::voice_commands::Kind::None) {
        executed = dispatch && media::voice_commands::dispatch_voice_command(match);
        nlohmann::json args = nlohmann::json::object();
        for (const auto& [k, v] : match.args)
            args[k] = v;
        out["intent"] = {
            {"kind", match.kind == media::voice_commands::Kind::App
                ? "command.app" : "command.custom"},
            {"execute", executed},
            {"text", event.text},
            {"commandId", match.id},
            {"label", match.label},
            {"appCommand", match.app_command},
            {"score", match.score},
            {"exact", match.exact},
            {"args", std::move(args)},
        };
    } else {
        out["intent"] = {
            {"kind", "command.candidate"},
            {"execute", false},
            {"text", event.text},
        };
    }
    return out;
}

struct VoiceCommandInstance {
    VoiceListenHost host;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> pending;
    std::string phrase;
    std::string error;
    bool dispatch = false;
    std::atomic<bool> failed{false};

    void push_trigger(nlohmann::json envelope)
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            pending.push_back(std::move(envelope));
        }
        cv.notify_all();
    }

    void fail(std::string message)
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            error = std::move(message);
            failed.store(true, std::memory_order_release);
        }
        cv.notify_all();
    }

    std::vector<nlohmann::json> take_pending()
    {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<nlohmann::json> out;
        out.swap(pending);
        return out;
    }
};

std::mutex& instances_mu()
{
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::string, std::shared_ptr<VoiceCommandInstance>>& instances()
{
    static std::unordered_map<std::string, std::shared_ptr<VoiceCommandInstance>> map;
    return map;
}

std::string default_instance_name(std::string name)
{
    if (name.empty())
        name = "default";
    return name;
}

std::shared_ptr<VoiceCommandInstance> find_instance(const std::string& name)
{
    std::lock_guard<std::mutex> lock(instances_mu());
    const auto it = instances().find(name);
    return it == instances().end() ? nullptr : it->second;
}

void forget_instance(const std::string& name)
{
    std::shared_ptr<VoiceCommandInstance> dying;
    {
        std::lock_guard<std::mutex> lock(instances_mu());
        const auto it = instances().find(name);
        if (it == instances().end())
            return;
        dying = std::move(it->second);
        instances().erase(it);
    }
    if (dying)
        dying->host.stop();
    media::command_control::external_command_end(
        "xblox-voice:" + name, false, "[xblox-voice]");
}

ListenOptions options_from_io(BlockIO& io)
{
    ListenOptions options = pm::audio::voice::listen_options_from_settings();
    nlohmann::json overlay = nlohmann::json::object();
    const auto put = [&](const char* key, const std::string& value) {
        if (!value.empty())
            overlay[key] = value;
    };
    put("route", io.str("route"));
    put("wakePhrase", io.str("wakePhrase"));
    put("filter", io.str("filter"));
    put("filterModel", io.str("filterModel"));
    put("provider", io.str("provider"));
    put("model", io.str("model"));
    put("language", io.str("language"));
    put("inputSource", io.str("inputSource"));
    put("input", io.str("device"));
    put("desktop", io.str("desktopDevice"));
    if (io.value("postFilter"))
        overlay["postFilter"] = io.flag("postFilter", options.post_filter);
    if (io.value("filterBlend"))
        overlay["filterBlend"] = io.f64("filterBlend", options.filter_blend);
    if (io.value("micGain"))
        overlay["micGain"] = io.f64("micGain", options.mic_gain);
    if (io.value("desktopGain"))
        overlay["desktopGain"] = io.f64("desktopGain", options.desktop_gain);
    pm::audio::voice::apply_listen_options_json(options, overlay);
    return options;
}

VoiceListenRoutes bind_routes(const std::shared_ptr<VoiceCommandInstance>& inst)
{
    VoiceListenRoutes routes;
    std::weak_ptr<VoiceCommandInstance> weak = inst;
    routes.on_trigger = [weak](const TriggerEvent& event) {
        auto live = weak.lock();
        if (!live)
            return;
        const auto catalog = media::voice_commands::find_voice_command(event.text);
        if (!phrase_matches(live->phrase, event.text, catalog)) {
            logger::debug("[xblox voiceListen] skip phrase filter text=\""
                          + event.text + "\"");
            return;
        }
        live->push_trigger(enrich_trigger(event, live->dispatch));
    };
    routes.on_cancel = [weak](const CancelEvent& event) {
        logger::info(std::string("[xblox voiceListen] cancel ") + event.reason);
        (void)weak;
    };
    routes.on_error = [weak](const std::string& message) {
        if (auto live = weak.lock())
            live->fail(message);
        else
            logger::warn("[xblox voiceListen] " + message);
    };
    routes.on_state = [](PipelineState state) {
        logger::debug(std::string("[xblox voiceListen] ") + pipeline_state_name(state));
    };
    return routes;
}

nlohmann::json status_payload(const std::string& name,
                              const std::shared_ptr<VoiceCommandInstance>& inst)
{
    nlohmann::json out = inst ? inst->host.status_json()
                              : nlohmann::json{
                                    {"running", false},
                                    {"state", pipeline_state_name(PipelineState::stopped)},
                                };
    out["schema"] = "pm.voice.status/v1";
    out["instance"] = name;
    if (inst)
        out["phrase"] = inst->phrase;
    return out;
}

bool start_instance(const std::string& name,
                    ListenOptions options,
                    const std::string& phrase,
                    bool dispatch,
                    std::shared_ptr<VoiceCommandInstance>& out,
                    std::string& error)
{
    auto inst = std::make_shared<VoiceCommandInstance>();
    inst->phrase = phrase;
    inst->dispatch = dispatch;
    if (!inst->host.start(std::move(options), bind_routes(inst), error))
        return false;
    {
        std::lock_guard<std::mutex> lock(instances_mu());
        if (instances().find(name) != instances().end()) {
            inst->host.stop();
            error = "voiceListen: instance already running: " + name;
            return false;
        }
        instances()[name] = inst;
    }
    media::command_control::external_command_begin(
        "xblox-voice:" + name,
        [name] { forget_instance(name); },
        "[xblox-voice]");
    out = std::move(inst);
    return true;
}

bool wait_matches(BlockIO& io,
                  const std::shared_ptr<VoiceCommandInstance>& inst,
                  long long duration_ms,
                  bool one_shot,
                  nlohmann::json& last)
{
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = duration_ms > 0
        ? started + std::chrono::milliseconds(duration_ms)
        : std::chrono::steady_clock::time_point::max();
    bool ran = false;
    while (!io.cancelled() && inst->host.is_running()
           && !inst->failed.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::vector<nlohmann::json> batch;
        {
            std::unique_lock<std::mutex> lock(inst->mu);
            inst->cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return !inst->pending.empty()
                    || inst->failed.load(std::memory_order_relaxed)
                    || !inst->host.is_running();
            });
            batch.swap(inst->pending);
        }
        for (auto& envelope : batch) {
            last = envelope;
            io.prime_previous(envelope);
            if (io.has_children("items"))
                io.run("items");
            ran = true;
            if (one_shot)
                return true;
            if (!io.runtime().result.ok)
                return false;
        }
    }
    return ran;
}

BlockResult voice_listen_block(BlockIO& io)
{
    const std::string kind = io.str("kind", "voiceListen");
    std::string action = lower_ascii(io.str("action", "Start"));
    if (kind == "voiceReplay")
        action = "replay";
    else if (kind == "voiceStop")
        action = "stop";
    else if (kind == "voiceStatus")
        action = "status";
    if (action.empty())
        action = "start";

    const std::string instance = default_instance_name(io.str("instance"));
    const std::string phrase = io.str("phrase");
    const bool dispatch = io.flag("dispatch", false);
    const long long duration_ms = io.i64("durationMs", 0);

    if (action == "stop") {
        auto inst = find_instance(instance);
        if (!inst) {
            io.warn(kind, "voiceListen: no instance named '" + instance + "'")
              .set("instance", instance);
            return BlockResult::Ok;
        }
        forget_instance(instance);
        io.ok(kind, "stopped voice listen '" + instance + "'")
          .set("instance", instance)
          .result(Value::from_json(status_payload(instance, nullptr)),
                  ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "status") {
        auto inst = find_instance(instance);
        auto status = status_payload(instance, inst);
        io.ok(kind, "voice listen '" + instance + "' "
                    + status.value("state", std::string{"stopped"}))
          .set("instance", instance)
          .payload(status)
          .result(Value::from_json(std::move(status)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "replay") {
        const std::string text = io.str("text");
        if (text.empty()) {
            io.error(kind, "voiceListen: Replay needs text (wake + command)", 1);
            return BlockResult::Error;
        }
        nlohmann::json last;
        bool matched = false;
        VoiceListenHost host;
        VoiceListenRoutes routes;
        routes.on_trigger = [&](const TriggerEvent& event) {
            const auto catalog = media::voice_commands::find_voice_command(event.text);
            if (!phrase_matches(phrase, event.text, catalog))
                return;
            last = enrich_trigger(event, dispatch);
            matched = true;
        };
        std::string error;
        if (!host.route_text(options_from_io(io), std::move(routes), text, error)) {
            io.error(kind, "voiceListen: " + error, 1);
            return BlockResult::Error;
        }
        if (!matched) {
            io.warn(kind, "voiceListen: no wake/phrase match for \"" + text + "\"")
              .set("text", text);
            return BlockResult::Ok;
        }
        io.ok(kind, "voice replay matched")
          .payload(last)
          .result(Value::from_json(last), ParamKind::json_value);
        if (io.has_children("items")) {
            io.prime_previous(last);
            io.run("items");
        }
        return BlockResult::Ok;
    }

    if (action != "still" && action != "start") {
        io.error(kind, "voiceListen: unknown action '" + action + "'", 1);
        return BlockResult::Error;
    }

    const bool one_shot = action == "still";
    std::shared_ptr<VoiceCommandInstance> inst = find_instance(instance);
    const bool attached = static_cast<bool>(inst) && inst->host.is_running();
    if (!attached) {
        if (inst)
            forget_instance(instance);
        std::string error;
        if (!start_instance(instance, options_from_io(io), phrase, dispatch,
                            inst, error)) {
            io.error(kind, error, 1).set("instance", instance);
            return BlockResult::Error;
        }
    } else {
        inst->phrase = phrase;
        inst->dispatch = dispatch;
    }

    if (action == "start" && !io.has_children("items") && !one_shot) {
        auto status = status_payload(instance, inst);
        io.ok(kind, "started voice listen '" + instance + "'")
          .set("instance", instance)
          .payload(status)
          .result(Value::from_json(std::move(status)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    nlohmann::json last;
    const bool matched = wait_matches(io, inst, duration_ms, one_shot, last);
    if (io.cancelled()) {
        if (!attached)
            forget_instance(instance);
        return BlockResult::Fatal;
    }
    if (inst->failed.load(std::memory_order_acquire)) {
        io.error(kind, "voiceListen: " + inst->error, 1)
          .set("instance", instance);
        if (!attached)
            forget_instance(instance);
        return BlockResult::Error;
    }
    if (!attached && (one_shot || !inst->host.is_running()))
        forget_instance(instance);

    if (!matched) {
        io.warn(kind, one_shot
            ? "voiceListen: no match before timeout/stop"
            : "voiceListen: listen ended without a match")
          .set("instance", instance);
        return BlockResult::Ok;
    }
    io.ok(kind, one_shot ? "voice listen matched" : "voice listen finished")
      .set("instance", instance)
      .payload(last)
      .result(Value::from_json(std::move(last)), ParamKind::json_value);
    return BlockResult::Ok;
}

#endif // FEATURE_VOICE_COMMANDS && PM_HAS_AUDIO_IO

#if PM_HAS_AUDIO_IO
BlockResult voice_listen_unavailable_block(BlockIO& io)
{
    io.error("voiceListen",
             "voiceListen: FEATURE_VOICE_COMMANDS is off in this build", 1);
    return BlockResult::Error;
}
#endif

} // namespace

void register_audio_voice_command_blocks(BlockRegistry& registry)
{
#if PM_HAS_AUDIO_IO
#if defined(FEATURE_VOICE_COMMANDS) && FEATURE_VOICE_COMMANDS
    logger::debug("[xblox] register voiceListen (FEATURE_VOICE_COMMANDS=on)");
#else
    logger::debug("[xblox] register voiceListen (FEATURE_VOICE_COMMANDS=off — unavailable handler)");
#endif
    using J = nlohmann::json;

    const nlohmann::json when_live{{"action", J{"Still", "Start"}}};
    const nlohmann::json when_replay{{"action", "Replay"}};
    const nlohmann::json when_match{{"action", J{"Still", "Start", "Replay"}}};

    const std::vector<ParamDef> params = {
        pd_enum("action", {"Still", "Start", "Stop", "Status", "Replay"})
            .grp("mode").dflt("Start")
            .desc("Still = one match then stop (or wait on a running instance). "
                  "Start keeps a named instance; with child items it runs them on each match until Stop. "
                  "Stop / Status address the same instance. Replay injects text (no mic)."),
        pd_str("instance").grp("mode").dflt("default")
            .desc("Named listen instance. Use the same name for Stop / Status / a later Still."),
        pd_ms("durationMs").adv().grp("mode").dflt(0).range(0, 86400000)
            .visible_when(when_live)
            .desc("Still / Start-with-children: stop waiting after this many ms. 0 = until Stop or cancel."),
        pd_str("phrase").in().lbl("Phrase").grp("matcher").dflt("")
            .visible_when(when_match)
            .desc("Optional spoken phrase or catalog id to accept. Empty = any wake+command. "
                  "On match, child items run with PREVIOUS = the trigger envelope."),
        pd_str("wakePhrase").grp("matcher").dflt("Tanit")
            .visible_when(when_match)
            .desc("Wake prefix. Default: Tanit (or Audio & Video voice_commands)."),
        pd_enum("route", {"trigger"}).adv().grp("matcher").dflt("trigger")
            .visible_when(when_match)
            .desc("XBlox mapping uses trigger. Children replace catalog dispatch unless dispatch is on."),
        pd_bool("dispatch").adv().grp("matcher").dflt(false)
            .visible_when(when_match)
            .desc("Also run the catalog command (hotkey / app_cmd) on a match. Default off — children are the mapping."),
        pd_str("text").in().grp("replay").dflt("")
            .visible_when(when_replay)
            .desc("Replay only: full utterance including the wake word, e.g. \"Tanit screenshot\"."),
        pd_enum("inputSource", {"mic", "desktop", "mix"}).grp("source").dflt("mic")
            .visible_when(when_live)
            .desc("Capture source. Default: mic (microphone). desktop = system loopback; mix = mic + desktop."),
        pd_audio_input_device("device").adv().grp("source").lbl("Microphone").dflt("")
            .visible_when(when_live)
            .desc("Mic device for mic/mix. Default: empty → voice_commands.audio_input_device, then App Settings audio_input_device."),
        pd_audio_loopback_device("desktopDevice").adv().grp("source").lbl("Desktop audio").dflt("")
            .visible_when(when_live)
            .desc("Loopback device for desktop/mix. Default: empty → default render device."),
        pd_float("micGain").adv().grp("source").dflt(1.0).range(0.0, 4.0)
            .visible_when(when_live)
            .desc("Mic level multiplier (linear) for mic/mix. Default: 1.0."),
        pd_float("desktopGain").adv().grp("source").dflt(1.0).range(0.0, 4.0)
            .visible_when(when_live)
            .desc("Desktop level multiplier for desktop/mix. Default: 1.0."),
        pd_enum("filter", {"auto", "off", "deepfilter", "gtcrn"}).grp("filter").dflt("auto")
            .visible_when(when_live)
            .desc("Enhance after VAD. auto = DeepFilter when available. off = dry/APM. "
                  "deepfilter / gtcrn are strict."),
        pd_str("filterModel").adv().grp("filter").dflt("")
            .visible_when(when_live)
            .desc("Optional DeepFilterNet tar.gz. Default: empty → ${MODELS_DIR}/deepfilter."),
        pd_bool("postFilter").adv().grp("filter").dflt(false)
            .visible_when(when_live)
            .desc("Over-attenuate very noisy sections (DeepFilter). Default: false."),
        pd_float("filterBlend").adv().grp("filter").dflt(1.0).range(0.0, 1.0)
            .visible_when(when_live)
            .desc("Wet/dry mix after enhancement. 0 = original, 1 = full filter (default)."),
        pd_provider().grp("speech_model").dflt("")
            .ui({{"options_path", "providers.stt"}})
            .desc("STT provider override. Default: empty → App Settings stt_provider."),
        pd_model().grp("speech_model").dflt("")
            .dyn({{"kind", "options_list"},
                  {"options_path_template", "providers.stt.models.{provider}"}})
            .desc("STT model override. Default: empty → App Settings stt_model when provider matches."),
        pd_str("language").adv().grp("speech_model").dflt("auto")
            .desc("Whisper language (auto, en, de, …). Default: auto."),
        pd_store_as().dflt("voice")
            .desc("Variable for PREVIOUS / downstream blocks. Default: voice."),
    };

    J defaults = {
        {"kind", "voiceListen"}, {"action", "Start"}, {"instance", "default"},
        {"phrase", ""}, {"text", ""}, {"wakePhrase", "Tanit"}, {"route", "trigger"},
        {"dispatch", false}, {"durationMs", 0}, {"filter", "auto"}, {"filterModel", ""},
        {"postFilter", false}, {"filterBlend", 1.0}, {"inputSource", "mic"}, {"device", ""},
        {"desktopDevice", ""}, {"micGain", 1.0}, {"desktopGain", 1.0},
        {"provider", ""}, {"model", ""}, {"language", "auto"}, {"storeAs", "voice"},
        {"items", J::array()},
    };

#if defined(FEATURE_VOICE_COMMANDS) && FEATURE_VOICE_COMMANDS
    const BlockHandler handler = voice_listen_block;
    const char* description =
        "Voice Command Center: named Still / Start / Stop listen loop. "
        "On a wake+phrase match, child items run with PREVIOUS = pm.voice.trigger/v1 "
        "(text, raw_text, intent.commandId). Multiple instances can run in parallel. "
        "Replay injects text without a microphone.";
#else
    const BlockHandler handler = voice_listen_unavailable_block;
    const char* description =
        "Voice Command Center (this build has FEATURE_VOICE_COMMANDS off).";
#endif

    registry["voiceListen"] = bdb(
        "voiceListen", handler,
        "Voice Listen", "Audio",
        description,
        defaults)
        .params(params)
        .primary_output(ParamKind::json_value, "Voice trigger")
        .children({{"items", "On Match", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});

    auto alias = [&](const char* kind, const char* action, const char* label) {
        J copy = defaults;
        copy["kind"] = kind;
        copy["action"] = action;
        registry[kind] = bdb(
            kind, handler,
            label, "Audio",
            "Alias for voiceListen.",
            copy)
            .params(params)
            .primary_output(ParamKind::json_value, "Voice trigger")
            .children({{"items", "On Match", "body", {"*"}, 0, -1}})
            .exec({false, true, true, true})
            .hidden();
    };
    alias("voiceReplay", "Replay", "Voice Replay");
    alias("voiceStop", "Stop", "Voice Stop");
    alias("voiceStatus", "Status", "Voice Status");
#else
    (void)registry;
    logger::debug("[xblox] skip voiceListen (PM_HAS_AUDIO_IO=off)");
#endif
}

} // namespace media::xblox::blocks
