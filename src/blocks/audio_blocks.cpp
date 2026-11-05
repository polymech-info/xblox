// XBlox audio blocks  -  audioListDevices, audioPlay, audioRecord,
//                      audioFilter, audioTranscribe (STT), audioSpeak (TTS).
//
// Gated on PM_HAS_AUDIO_IO (play/record/devices/filters). STT/TTS blocks additionally require FEATURE_STT.
//
// PREVIOUS / storeAs contract (same as all other blocks):
//   • audioListDevices  -> PREVIOUS = array of {name, is_default, channels, sample_rate}
//   • audioPlay         -> PREVIOUS = file path string
//   • audioRecord       -> PREVIOUS = output audio file path (.wav or .m4a/.aac)
//   • audioFilter       -> PREVIOUS = enhanced audio file path (.wav)
//   • audioTranscribe   -> PREVIOUS = transcript text string, or STT JSON when json=true
//   • audioSpeak        -> PREVIOUS = output file path (when outputPath set) or input text

#include "blocks/audio_blocks.hpp"

#include "logger/logger.h"
#include "xblox_constants.hpp"

#include <cctype>
#include <memory>
#include <string>

#include "constants.hpp"
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

#if PM_HAS_AUDIO_IO

#include "core/audio.hpp"
#include "core/audio_devices.hpp"
#include "core/mixed_audio_capture.hpp"
#include "core/pcm_record_buffer.hpp"
#if defined(_WIN32)
#  include "core/mf_aac_record_writer.hpp"
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <objbase.h>
#  include <windows.h>
#endif
#include "audio/audio_constants.hpp"
#include "audio/filter/audio_filter.hpp"
#include "blocks/block_provider.hpp"
#include "core/cli_cancel.hpp"
#include "core/settings_runtime.hpp"
#include "llm/path_tool_executor.hpp"
#include "win/overlay_hud.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#endif // PM_HAS_AUDIO_IO

#if defined(FEATURE_STT) && FEATURE_STT

#include "audio/stt/proxy_stt.hpp"
#include "audio/stt/elevenlabs.hpp"
#include "audio/stt/whisper_local.hpp"
#include "audio/stt/windows_ai_stt.hpp"
#include "audio/subs/subtitle_writer.hpp"
#include "video/stt_route.hpp"

#include <algorithm>
#include <filesystem>
#include "audio/tts/elevenlabs_tts.hpp"
#include "audio/tts/proxy_tts.hpp"
#include "audio/tts/tts_text_input.hpp"
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
#  include "audio/tts/vibevoice_tts.hpp"
#endif
#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
#  include "audio/tts/moss_tts.hpp"
#endif
#include "audio/audio_constants.hpp"
#include "core/local_model_catalog.hpp"
#include "core/mixed_audio_capture.hpp"
#if defined(_WIN32)
#  include "win/transcription_hud.hpp"
#endif

#endif // FEATURE_STT

namespace media::xblox::blocks {

namespace bp = media::xblox::bp;

namespace {

namespace C = pm::audio::constants;

#if PM_HAS_AUDIO_IO

// ── Device resolution helpers ─────────────────────────────────────────────────
// Load the user's preferred audio input device from settings (once per call  - 
// the settings store caches on the underlying OS store; reads are cheap).
// Returns the block `device` param if non-empty, otherwise the settings value.
static std::string resolve_audio_input(const std::string& block_device)
{
    if (!block_device.empty()) return block_device;
    media::runtime_settings::AudioVideoDeviceSettings avs;
    std::string err;
    media::runtime_settings::load_audio_video_device_settings(avs, err);
    return avs.audio_input_device;
}

static std::string resolve_audio_output(const std::string& block_device)
{
    if (!block_device.empty()) return block_device;
    media::runtime_settings::AudioVideoDeviceSettings avs;
    std::string err;
    media::runtime_settings::load_audio_video_device_settings(avs, err);
    return avs.audio_output_device;
}

#if defined(_WIN32)
struct ComMtaGuard {
    bool owned = false;
    ComMtaGuard()
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = (hr == S_OK || hr == S_FALSE);
    }
    ~ComMtaGuard()
    {
        if (owned)
            ::CoUninitialize();
    }
};
#endif

// In-process ribbon xblox shares one worker thread with the element picker.
// UIA leaves that thread in STA; WASAPI/miniaudio on STA often returns from
// play_sync without throwing and without audible output. Play on a fresh MTA.
static void play_tts_on_audio_thread(const std::function<void()>& play)
{
    std::exception_ptr ep;
    std::thread t([&] {
#if defined(_WIN32)
        ComMtaGuard com;
#endif
        try {
            play();
        } catch (...) {
            ep = std::current_exception();
        }
    });
    t.join();
    if (ep)
        std::rethrow_exception(ep);
}

static void play_registered_tts(pm::audio::AudioOutput& out,
                                const uint8_t* data, size_t bytes,
                                const std::function<bool()>& cancelled)
{
    media::llm::path::register_active_speak_output(&out);
    std::atomic<bool> done{false};
    std::thread watcher;
    if (cancelled) {
        watcher = std::thread([&] {
            while (!done.load(std::memory_order_relaxed)) {
                if (cancelled()) {
                    out.stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        });
    }
    try {
        out.play_sync(data, bytes);
    } catch (...) {
        done.store(true, std::memory_order_relaxed);
        if (watcher.joinable())
            watcher.join();
        media::llm::path::unregister_active_speak_output(&out);
        throw;
    }
    done.store(true, std::memory_order_relaxed);
    if (watcher.joinable())
        watcher.join();
    media::llm::path::unregister_active_speak_output(&out);
}

static void play_memory_tts(const uint8_t* data, size_t bytes, const std::string& device,
                            const std::function<bool()>& cancelled = {})
{
    if (!data || bytes == 0)
        throw std::runtime_error("TTS produced no audio bytes");
    play_tts_on_audio_thread([&] {
        pm::audio::AudioOutput out;
        if (!device.empty())
            out.set_device_name(device);
        play_registered_tts(out, data, bytes, cancelled);
    });
}

static void play_file_tts(const std::string& path,
                          const std::string& device,
                          const std::function<bool()>& cancelled)
{
    play_tts_on_audio_thread([&] {
        std::ifstream wav(media::path::path_from_utf8(path), std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(wav)), {});
        if (bytes.empty())
            throw std::runtime_error("audio file is empty: " + path);
        pm::audio::AudioOutput out;
        if (!device.empty())
            out.set_device_name(device);
        play_registered_tts(out, bytes.data(), bytes.size(), cancelled);
    });
}

// ── Simple WAV writer ─────────────────────────────────────────────────────────
// Writes a canonical RIFF/PCM WAV header then appends raw s16le mono 16 kHz.

static void write_wav_header(std::ofstream& f, uint32_t num_samples,
                              uint32_t sample_rate = 16000,
                              uint16_t channels    = 1,
                              uint16_t bits        = 16)
{
    const uint32_t data_bytes    = num_samples * channels * (bits / 8u);
    const uint32_t byte_rate     = sample_rate * channels * (bits / 8u);
    const uint16_t block_align   = static_cast<uint16_t>(channels * (bits / 8u));
    const uint32_t chunk2_size   = data_bytes;
    const uint32_t chunk1_size   = 16;
    const uint32_t riff_size     = 4 + (8 + chunk1_size) + (8 + chunk2_size);

    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };

    f.write("RIFF", 4);  w32(riff_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);  w32(chunk1_size);
    w16(1);              // PCM
    w16(channels);
    w32(sample_rate);
    w32(byte_rate);
    w16(block_align);
    w16(bits);
    f.write("data", 4);  w32(chunk2_size);
}

struct AudioRecordSession {
    pm::audio::MixedAudioCapture input;
    pm::audio::MixedCaptureConfig cfg;
    uint32_t pcm_rate     = pm::audio::constants::k_recording_sample_rate;
    uint32_t pcm_channels = pm::audio::constants::k_recording_channels;
    pm::audio::PcmRecordBuffer pcm_buf;
    bool write_aac = false;
#if defined(_WIN32)
    std::unique_ptr<pm::audio::MfAacRecordWriter> aac_writer;
#endif
    std::atomic<bool> done{false};
    std::string opened_mic;
    std::string opened_desktop;
    std::filesystem::path output_path;
};

std::mutex g_record_sessions_mu;
std::unordered_map<std::string, std::shared_ptr<AudioRecordSession>> g_record_sessions;
std::atomic<unsigned long long> g_record_session_seq{0};

std::string make_record_session_id()
{
    return "rec-" + std::to_string(++g_record_session_seq);
}

std::string temp_wav_path(const char* prefix)
{
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec);
    return (ec ? std::string(".") : media::path::path_to_utf8(tmp)) + "/" + prefix +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav";
}

// ── audioListDevices ──────────────────────────────────────────────────────────

BlockResult audio_list_devices_block(BlockIO& io)
{
    nlohmann::json devices = nlohmann::json::array();
    try {
        for (const auto& dev : pm::audio::enumerate_capture_devices()) {
            devices.push_back({
                {"name",        dev.name},
                {"is_default",  dev.is_default},
                {"is_loopback", false},
                {"kind",        "mic"},
                {"channels",    dev.channels},
                {"sample_rate", dev.sample_rate},
            });
        }
        for (const auto& dev : pm::audio::enumerate_loopback_devices()) {
            devices.push_back({
                {"name",        dev.name},
                {"is_default",  dev.is_default},
                {"is_loopback", true},
                {"kind",        "desktop"},
                {"channels",    dev.channels},
                {"sample_rate", dev.sample_rate},
            });
        }
    } catch (const std::exception& ex) {
        io.error("audioListDevices", std::string("audioListDevices failed: ") + ex.what(), 1);
        return BlockResult::Error;
    }

    const auto count = static_cast<long long>(devices.size());
    io.ok("audioListDevices", "found " + std::to_string(count) + " audio capture device(s)")
      .set("count", count)
      .result(Value::from_json(std::move(devices)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── audioPlay ─────────────────────────────────────────────────────────────────
// Plays an audio file (MP3/WAV/FLAC) asynchronously and waits for it to finish
// or for cancellation.

BlockResult audio_play_block(BlockIO& io)
{
    // `path` is declared .prev(), so io.str() already falls back to PREVIOUS and
    // interpolated ${var}; we only re-check emptiness for the friendly error.
    const std::string file_path = io.str("path");
    const std::string output_device = resolve_audio_output(io.str("outputDevice"));
    if (file_path.empty()) {
        io.error("audioPlay", "audioPlay: path is empty", 2);
        return BlockResult::Error;
    }
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec)) {
        io.error("audioPlay", "audioPlay: file not found: " + file_path, 1);
        return BlockResult::Error;
    }

    try {
        play_file_tts(file_path, output_device, [&]() { return io.cancelled(); });
    } catch (const std::exception& ex) {
        io.error("audioPlay", std::string("audioPlay failed: ") + ex.what(), 1);
        return BlockResult::Error;
    }

    io.ok("audioPlay", "playing: " + file_path)
      .set("path", file_path)
      .set("outputDevice", output_device)
      .result(file_path, ParamKind::audio_path);
    return BlockResult::Ok;
}

// ── audioFilter ───────────────────────────────────────────────────────────────
// File-to-file enhancer. Same lib as `audio filter` (DeepFilterNet @ 48 kHz now;
// GTCRN later). PREVIOUS / storeAs ← output WAV path.

BlockResult audio_filter_block(BlockIO& io)
{
    std::string input = io.str("input");
    if (input.empty())
        input = io.str("path");
    std::string output_path = io.str("outputPath");
    if (input.empty()) {
        io.error("audioFilter", "audioFilter: input is empty", 2);
        return BlockResult::Error;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(media::path::path_from_utf8(input), ec) || ec) {
        io.error("audioFilter", "audioFilter: file not found: " + input, 1);
        return BlockResult::Error;
    }
    if (output_path.empty()) {
        const auto src = media::path::path_from_utf8(input);
        std::filesystem::path name = src.stem();
        name += media::path::path_from_utf8("-filtered.wav");
        output_path = media::path::path_to_utf8(src.parent_path() / name);
    }

    pm::audio::filter::Options opt;
    opt.kind = pm::audio::filter::parse_kind(io.str("filter"));
    opt.model = io.str("model");
    opt.atten_lim_db = static_cast<float>(io.f64("attenLim", 100.0));
    opt.post_filter = io.flag("postFilter", false);
    opt.compensate_delay = io.flag("compensateDelay", true);

    if (!pm::audio::filter::available(opt.kind)) {
        io.error("audioFilter",
                 "audioFilter: backend not available for filter='"
                     + std::string(pm::audio::filter::kind_name(opt.kind)) + "'",
                 1);
        return BlockResult::Error;
    }

    const auto result = pm::audio::filter::apply_file(input, output_path, opt);
    if (!result.ok) {
        io.error("audioFilter", "audioFilter: " + result.error, 1);
        return BlockResult::Error;
    }

    io.ok("audioFilter", "filtered " + input + " → " + output_path)
      .set("path", output_path)
      .set("input", input)
      .set("filter", result.kind)
      .set("backend", result.backend)
      .set("sampleRate", static_cast<long long>(result.sample_rate))
      .set("channels", static_cast<long long>(result.channels))
      .set("frames", static_cast<long long>(result.frames))
      .result(output_path, ParamKind::audio_path);
    return BlockResult::Ok;
}

// ── audioRecord ───────────────────────────────────────────────────────────────
// Captures audio for `durationMs` ms and writes by outputPath extension:
//   .wav  — PCM (48 kHz stereo default; spills to disk after ~30 s in RAM)
//   .m4a / .aac — AAC-LC (Windows only; encoded during capture)
// If outputPath is empty, a temp .wav file is created.
// Optional filter (deepfilter) runs after the file is written — not on the callback.
// PREVIOUS / storeAs ← output file path string.

bool audio_record_wants_filter(BlockIO& io)
{
    const std::string filter = io.str("filter");
    if (!filter.empty() && filter != "off" && filter != "none")
        return true;
    if (io.flag("postFilter", false))
        return true;
    if (!io.str("filterModel").empty())
        return true;
    return false;
}

pm::audio::filter::Options audio_record_filter_options(BlockIO& io)
{
    pm::audio::filter::Options opt;
    const std::string filter = io.str("filter");
    opt.kind = pm::audio::filter::parse_kind(
        (filter.empty() || filter == "off" || filter == "none") ? "deepfilter" : filter);
    opt.model = io.str("filterModel");
    opt.atten_lim_db = static_cast<float>(io.f64("attenLim", 100.0));
    opt.post_filter = io.flag("postFilter", false);
    opt.compensate_delay = io.flag("compensateDelay", true);
    return opt;
}

BlockResult audio_record_block(BlockIO& io)
{
    const int         duration_ms  = std::max(100, static_cast<int>(io.i64("durationMs", 3000)));
    std::string       output_path  = io.str("outputPath");
    pm::audio::MixedCaptureConfig cfg = pm::audio::make_recording_capture_config(
        io.str("inputSource"),
        resolve_audio_input(io.str("device")),
        io.str("desktopDevice"),
        static_cast<float>(io.f64("micGain", 1.0)),
        static_cast<float>(io.f64("desktopGain", 1.0)),
        pm::audio::CaptureSource::mic);
    const int req_rate = static_cast<int>(io.i64("sampleRate", 0));
    const int req_ch   = static_cast<int>(io.i64("channels", 0));
    if (req_rate >= 8000) cfg.sample_rate = static_cast<uint32_t>(req_rate);
    if (req_ch == 1 || req_ch == 2) cfg.channels = static_cast<uint32_t>(req_ch);

    uint32_t pcm_rate = pm::audio::constants::k_recording_sample_rate;
    uint32_t pcm_channels = pm::audio::constants::k_recording_channels;
    pm::audio::resolve_capture_pcm_format(cfg, pcm_rate, pcm_channels);

    // Build a temp path when none supplied
    if (output_path.empty()) {
        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        output_path = (ec ? std::string(".") : media::path::path_to_utf8(tmp)) + "/xblox_record_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav";
    }

    const std::string ext = [&]() {
        std::string e = media::path::path_to_utf8(media::path::path_from_utf8(output_path).extension());
        e = media::strutil::lower_ascii(std::move(e));
        return e;
    }();
    const pm::audio::RecordContainer container =
        pm::audio::record_container_from_extension(ext);
    const bool write_aac = container == pm::audio::RecordContainer::Aac;
    const bool write_wav_file = !write_aac;
    const bool want_filter = audio_record_wants_filter(io);

#if defined(_WIN32)
    if (write_aac && !pm::audio::aac_record_supported()) {
        io.error("audioRecord", "audioRecord: AAC/M4A output requires Windows", 1);
        return BlockResult::Error;
    }
#endif
    if (want_filter && write_aac) {
        io.error("audioRecord",
                 "audioRecord: filter writes 48 kHz mono WAV; use a .wav outputPath", 1);
        return BlockResult::Error;
    }

    pm::audio::PcmRecordBuffer pcm_buf;
    if (write_wav_file) {
        pcm_buf.set_format(pcm_rate, static_cast<uint16_t>(pcm_channels));
        pcm_buf.set_spill_threshold_seconds(pm::audio::constants::k_record_spill_threshold_seconds);
        pcm_buf.reserve_seconds(static_cast<uint32_t>((duration_ms + 999) / 1000));
    }

#if defined(_WIN32)
    std::unique_ptr<pm::audio::MfAacRecordWriter> aac_writer;
    if (write_aac) {
        aac_writer = std::make_unique<pm::audio::MfAacRecordWriter>();
        std::string aac_err;
        if (!aac_writer->open(output_path, pcm_rate, static_cast<uint16_t>(pcm_channels), aac_err)) {
            io.error("audioRecord", std::string("audioRecord: ") + aac_err, 1);
            return BlockResult::Error;
        }
    }
#endif

    pm::audio::PcmLevelStats capture_levels;
    std::atomic<bool> done{false};

    pm::audio::MixedAudioCapture input;
    std::string cap_err;
    if (!input.start(cfg, [&](const int16_t* pcm, size_t frame_count) {
            if (done.load(std::memory_order_relaxed)) return;
            if (write_wav_file)
                pcm_buf.push(pcm, frame_count);
            else
                pm::audio::accumulate_pcm_levels(
                    capture_levels, pcm, frame_count * static_cast<size_t>(pcm_channels));
#if defined(_WIN32)
            if (aac_writer && aac_writer->is_open())
                aac_writer->write_pcm(pcm, frame_count);
#endif
        }, cap_err)) {
        io.error("audioRecord", std::string("audioRecord: open device failed: ") + cap_err, 1);
        return BlockResult::Error;
    }

    // Record for duration_ms, waking early on cancel / overlay or taskbar Stop.
    auto hud = media::win::StatusHud::begin(
        io.flag("hud", false), io.str("hudMode"), "Record");
    const std::string source_name = pm::audio::capture_source_name(cfg.source);
    if (hud)
        hud.busy(source_name);

    {
        namespace chr = std::chrono;
        const auto t0 = chr::steady_clock::now();
        const auto deadline = t0 + chr::milliseconds(duration_ms);
        while (chr::steady_clock::now() < deadline) {
            if (io.cancelled() || hud.stop_requested())
                break;
            if (hud) {
                const auto elapsed = chr::duration_cast<chr::milliseconds>(
                    chr::steady_clock::now() - t0).count();
                hud.progress(static_cast<uint64_t>(std::max<long long>(0, elapsed)),
                             static_cast<uint64_t>(duration_ms),
                             source_name);
            }
            const auto remaining = chr::duration_cast<chr::milliseconds>(
                deadline - chr::steady_clock::now()).count();
            if (remaining <= 0)
                break;
            std::this_thread::sleep_for(chr::milliseconds(
                remaining < 50 ? remaining : 50));
        }
    }
    done.store(true, std::memory_order_relaxed);
    input.stop();
    if (hud.stop_requested())
        hud.busy("stopping");

    std::error_code ec2;
    if (auto parent = media::path::path_from_utf8(output_path).parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec2);

    if (write_wav_file) {
        if (!pcm_buf.write_wav(output_path)) {
            if (hud)
                hud.error("cannot write");
            io.error("audioRecord", "audioRecord: cannot write: " + output_path, 1);
            return BlockResult::Error;
        }
    }
#if defined(_WIN32)
    else if (aac_writer) {
        std::string aac_err;
        if (!aac_writer->close(aac_err)) {
            if (hud)
                hud.error(aac_err);
            io.error("audioRecord", std::string("audioRecord: ") + aac_err, 1);
            return BlockResult::Error;
        }
    }
#endif

    const uint32_t pcm_frames = write_wav_file
        ? pcm_buf.frame_count()
#if defined(_WIN32)
        : (aac_writer ? aac_writer->frame_count() : 0);
#else
        : 0;
#endif
    const double duration_s = (pcm_rate > 0 && pcm_channels > 0 && pcm_frames > 0)
        ? static_cast<double>(pcm_frames) / static_cast<double>(pcm_rate)
        : 0.0;
    const auto recorded_ms = static_cast<long long>(duration_s * 1000);
    const pm::audio::PcmLevelStats levels =
        write_wav_file ? pcm_buf.analyze_levels() : capture_levels;

    uint32_t out_rate = pcm_rate;
    uint32_t out_channels = pcm_channels;
    uint64_t out_frames = pcm_frames;
    std::string out_format = write_aac ? std::string("aac/m4a")
        : pm::audio::capture_pcm_format_string(pcm_rate, pcm_channels);
    std::string filter_kind;
    std::string filter_backend;

    if (want_filter) {
        if (hud)
            hud.busy("filter");
        const auto fopt = audio_record_filter_options(io);
        const auto fres = pm::audio::filter::apply_file_inplace(output_path, fopt);
        if (!fres.ok) {
            if (hud)
                hud.error(fres.error);
            io.error("audioRecord", "audioRecord: filter failed: " + fres.error, 1);
            return BlockResult::Error;
        }
        out_rate = fres.sample_rate;
        out_channels = fres.channels;
        out_frames = fres.frames;
        out_format = pm::audio::capture_pcm_format_string(out_rate, out_channels);
        filter_kind = fres.kind;
        filter_backend = fres.backend;
    }

    if (hud)
        hud.completed(std::to_string(recorded_ms) + " ms");
    auto ev = io.ok("audioRecord",
                    std::string(want_filter ? "recorded+filtered " : "recorded ")
                        + std::to_string(recorded_ms) + " ms to: " + output_path);
    ev.set("path",       output_path)
      .set("durationMs", recorded_ms)
      .set("samples",    static_cast<long long>(out_frames) * out_channels)
      .set("sampleRate", static_cast<long long>(out_rate))
      .set("channels",   static_cast<long long>(out_channels))
      .set("format",     out_format)
      .set("peakDbfs",   levels.peak_dbfs)
      .set("rmsDbfs",    levels.rms_dbfs)
      .set("clippedSamples", static_cast<long long>(levels.clipped_samples))
      .set("inputSource", pm::audio::capture_source_name(cfg.source))
      .set("device",     input.opened_mic_device().empty() ? input.opened_desktop_device() : input.opened_mic_device());
    if (want_filter) {
        ev.set("filter", filter_kind)
          .set("backend", filter_backend);
    }
    ev.result(output_path, ParamKind::audio_path);
    return BlockResult::Ok;
}

BlockResult audio_record_start_block(BlockIO& io)
{
    std::string session_id = io.str("session");
    if (session_id.empty())
        session_id = make_record_session_id();

    {
        std::lock_guard<std::mutex> lk(g_record_sessions_mu);
        if (g_record_sessions.find(session_id) != g_record_sessions.end()) {
            io.error("audioRecordStart", "audioRecordStart: session already exists: " + session_id, 1);
            return BlockResult::Error;
        }
    }

    auto session = std::make_shared<AudioRecordSession>();
    session->cfg = pm::audio::make_recording_capture_config(
        io.str("inputSource"),
        resolve_audio_input(io.str("device")),
        io.str("desktopDevice"),
        static_cast<float>(io.f64("micGain", 1.0)),
        static_cast<float>(io.f64("desktopGain", 1.0)),
        pm::audio::CaptureSource::mic);
    const int req_rate = static_cast<int>(io.i64("sampleRate", 0));
    const int req_ch   = static_cast<int>(io.i64("channels", 0));
    if (req_rate >= 8000) session->cfg.sample_rate = static_cast<uint32_t>(req_rate);
    if (req_ch == 1 || req_ch == 2) session->cfg.channels = static_cast<uint32_t>(req_ch);
    uint32_t pcm_rate = pm::audio::constants::k_recording_sample_rate;
    uint32_t pcm_channels = pm::audio::constants::k_recording_channels;
    pm::audio::resolve_capture_pcm_format(session->cfg, pcm_rate, pcm_channels);
    session->pcm_rate     = pcm_rate;
    session->pcm_channels = pcm_channels;
    session->pcm_buf.set_format(pcm_rate, static_cast<uint16_t>(pcm_channels));
    session->pcm_buf.set_spill_threshold_seconds(
        pm::audio::constants::k_record_spill_threshold_seconds);

    std::string output_path = io.str("outputPath");
    if (output_path.empty())
        output_path = temp_wav_path("xblox_record_session_");
    session->output_path = output_path;

    const std::string ext = [&]() {
        std::string e = media::path::path_to_utf8(media::path::path_from_utf8(output_path).extension());
        e = media::strutil::lower_ascii(std::move(e));
        return e;
    }();
    session->write_aac =
        pm::audio::record_container_from_extension(ext) == pm::audio::RecordContainer::Aac;
#if !defined(_WIN32)
    if (session->write_aac) {
        io.error("audioRecordStart", "audioRecordStart: AAC/M4A output requires Windows", 1);
        return BlockResult::Error;
    }
#endif
#if defined(_WIN32)
    if (session->write_aac) {
        session->aac_writer = std::make_unique<pm::audio::MfAacRecordWriter>();
        std::string aac_err;
        if (!session->aac_writer->open(output_path, pcm_rate,
                                       static_cast<uint16_t>(pcm_channels), aac_err)) {
            io.error("audioRecordStart", std::string("audioRecordStart: ") + aac_err, 1);
            return BlockResult::Error;
        }
    }
#endif

    std::string cap_err;
    if (!session->input.start(
            session->cfg,
            [session](const int16_t* pcm, size_t frame_count) {
                if (session->done.load(std::memory_order_relaxed)) return;
                if (!session->write_aac)
                    session->pcm_buf.push(pcm, frame_count);
#if defined(_WIN32)
                if (session->aac_writer && session->aac_writer->is_open())
                    session->aac_writer->write_pcm(pcm, frame_count);
#endif
            },
            cap_err)) {
        io.error("audioRecordStart", std::string("audioRecordStart: open device failed: ") + cap_err, 1);
        return BlockResult::Error;
    }
    session->opened_mic = session->input.opened_mic_device();
    session->opened_desktop = session->input.opened_desktop_device();

    {
        std::lock_guard<std::mutex> lk(g_record_sessions_mu);
        g_record_sessions[session_id] = session;
    }

    const std::string opened = session->opened_mic.empty() ? session->opened_desktop : session->opened_mic;
    logger::trace("[audioRecordStart] session=" + session_id +
                  " source=" + pm::audio::capture_source_name(session->cfg.source) +
                  " device=" + (opened.empty() ? std::string{"<default>"} : opened));
    io.ok("audioRecordStart", "recording started: " + session_id)
      .set("session", session_id)
      .set("inputSource", pm::audio::capture_source_name(session->cfg.source))
      .set("device", opened)
      .result(session_id, ParamKind::string_);
    return BlockResult::Ok;
}

BlockResult audio_record_stop_block(BlockIO& io)
{
    const std::string session_id = io.str("session");
    if (session_id.empty()) {
        io.error("audioRecordStop", "audioRecordStop: missing session", 1);
        return BlockResult::Error;
    }

    std::shared_ptr<AudioRecordSession> session;
    {
        std::lock_guard<std::mutex> lk(g_record_sessions_mu);
        auto it = g_record_sessions.find(session_id);
        if (it == g_record_sessions.end()) {
            io.error("audioRecordStop", "audioRecordStop: unknown session: " + session_id, 1);
            return BlockResult::Error;
        }
        session = it->second;
        g_record_sessions.erase(it);
    }

    session->done.store(true, std::memory_order_relaxed);
    session->input.stop();

    std::string output_path = io.str("outputPath");
    if (output_path.empty())
        output_path = media::path::path_to_utf8(session->output_path);
    if (output_path.empty())
        output_path = temp_wav_path("xblox_record_session_");

    std::error_code ec;
    if (auto parent = media::path::path_from_utf8(output_path).parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec);

    const bool write_aac = session->write_aac;
    if (!write_aac) {
        if (!session->pcm_buf.write_wav(output_path)) {
            io.error("audioRecordStop", "audioRecordStop: cannot write: " + output_path, 1);
            return BlockResult::Error;
        }
    }
#if defined(_WIN32)
    else if (session->aac_writer) {
        std::string aac_err;
        if (!session->aac_writer->close(aac_err)) {
            io.error("audioRecordStop", std::string("audioRecordStop: ") + aac_err, 1);
            return BlockResult::Error;
        }
    }
#endif

    const uint32_t pcm_frames = write_aac
#if defined(_WIN32)
        ? (session->aac_writer ? session->aac_writer->frame_count() : 0)
#else
        ? 0
#endif
        : session->pcm_buf.frame_count();
    const auto recorded_ms = (session->pcm_rate > 0 && session->pcm_channels > 0 && pcm_frames > 0)
        ? static_cast<long long>(
              (static_cast<double>(pcm_frames) / static_cast<double>(session->pcm_rate)) * 1000.0)
        : 0LL;
    logger::trace("[audioRecordStop] session=" + session_id +
                  " frames=" + std::to_string(pcm_frames) +
                  " path=" + output_path);
    const std::string opened_device =
        session->opened_mic.empty() ? session->opened_desktop : session->opened_mic;
    const pm::audio::PcmLevelStats levels = session->pcm_buf.analyze_levels();
    io.ok("audioRecordStop", "recording stopped: " + output_path)
      .set("session", session_id)
      .set("path", output_path)
      .set("durationMs", recorded_ms)
      .set("samples", static_cast<long long>(pcm_frames) * session->pcm_channels)
      .set("sampleRate", static_cast<long long>(session->pcm_rate))
      .set("channels",   static_cast<long long>(session->pcm_channels))
      .set("format",     write_aac ? std::string("aac/m4a")
                                    : pm::audio::capture_pcm_format_string(session->pcm_rate, session->pcm_channels))
      .set("peakDbfs", levels.peak_dbfs)
      .set("rmsDbfs", levels.rms_dbfs)
      .set("clippedSamples", static_cast<long long>(levels.clipped_samples))
      .set("inputSource", pm::audio::capture_source_name(session->cfg.source))
      .set("device", opened_device)
      .result(output_path, ParamKind::audio_path);
    return BlockResult::Ok;
}

#endif // PM_HAS_AUDIO_IO

#if defined(FEATURE_STT) && FEATURE_STT

static nlohmann::json whisper_provider_options_dynamic_schema()
{
    return {
        {"kind", "provider_options"},
        {"mode", "stt"},
        {"provider_field", "provider"},
        {"model_field", "model"},
        {"schema_method", "xbloxProviderOptionsSchemaGet"},
        {"value_field", "providerOptions"},
    };
}

#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
static pm::stt::WhisperLocalOptions whisper_options_from_json(const nlohmann::json& raw)
{
    pm::stt::WhisperLocalOptions opts;
    if (!raw.is_object())
        return opts;

    const std::string backend = raw.value("backend", std::string("gpu"));
    opts.use_gpu = (backend == "gpu");
    opts.gpu_device = raw.value("gpuDevice", raw.value("gpu_device", 0));
    opts.n_threads = raw.value("threads", 0);
    opts.language = raw.value("language", std::string("auto"));

    const std::string flash_attn = raw.value("flashAttn", raw.value("flash_attn", std::string("auto")));
    if (flash_attn == "on")
        opts.flash_attn = pm::stt::WhisperFlashAttention::On;
    else if (flash_attn == "off")
        opts.flash_attn = pm::stt::WhisperFlashAttention::Off;
    else
        opts.flash_attn = pm::stt::WhisperFlashAttention::Auto;
    return opts;
}
#endif // FEATURE_WHISPER

static pm::video::SttRoute transcribe_stt_route(BlockIO& io)
{
    auto r = pm::video::stt_route_from_json(io.json("stt"));
    if (r.provider.empty())
        r.provider = io.str("provider");
    if (r.model.empty())
        r.model = io.str("model");
    if (!r.options.is_object() || r.options.empty()) {
        const auto& po = io.json("providerOptions");
        if (po.is_object())
            r.options = po;
    }
    return r;
}

static nlohmann::json cues_to_segments_json(const std::vector<pm::subs::Cue>& cues)
{
    nlohmann::json a = nlohmann::json::array();
    for (const auto& c : cues) {
        a.push_back({
            {"text", c.text},
            {"t0Ms", c.t0_ms},
            {"t1Ms", c.t1_ms},
            {"t0_cs", c.t0_ms / 10},
            {"t1_cs", c.t1_ms / 10},
        });
    }
    return a;
}

static pm::subs::GrowOptions transcribe_grow(BlockIO& io)
{
    pm::subs::GrowOptions g;
    g.max_chars = std::clamp(static_cast<int>(io.i64("subtitleMaxChars", 42)),
                             pm::subs::k_min_cue_chars, pm::subs::k_max_cue_chars);
    g.max_lines = std::clamp(static_cast<int>(io.i64("subtitleMaxLines", 2)), 1, 4);
    return g;
}

static std::vector<pm::subs::Cue> cues_from_el_timed(
    const pm::stt::ElevenLabsSTT::TimedCommit& tc, const pm::subs::GrowOptions& opt)
{
    std::vector<pm::subs::Cue> words;
    words.reserve(tc.words.size());
    for (const auto& w : tc.words)
        words.push_back({w.text, w.t0_ms, w.t1_ms});
    return pm::subs::pack_word_cues(words, opt, tc.text);
}

static std::vector<pm::subs::Cue> cues_from_whisper(
    const pm::stt::WhisperTranscript& detail, const pm::subs::GrowOptions& opt)
{
    std::vector<pm::subs::Cue> words;
    for (size_t si = 0; si < detail.segments.size(); ++si) {
        const auto& s = detail.segments[si];
        if (si > 0 && pm::stt::whisper_segment_repeats(detail.segments[si - 1], s))
            continue;
        if (!s.words.empty()) {
            for (const auto& w : s.words) {
                pm::subs::Cue c;
                c.text = w.text;
                c.t0_ms = pm::subs::whisper_cs_to_ms(w.t0_cs);
                c.t1_ms = pm::subs::whisper_cs_to_ms(w.t1_cs);
                c.segment_id = static_cast<int>(si);
                words.push_back(std::move(c));
            }
            continue;
        }
        if (s.text.empty())
            continue;
        pm::subs::Cue c;
        c.text = s.text;
        c.t0_ms = pm::subs::whisper_cs_to_ms(s.t0_cs);
        c.t1_ms = pm::subs::whisper_cs_to_ms(s.t1_cs);
        c.segment_id = static_cast<int>(si);
        words.push_back(std::move(c));
    }
    return pm::subs::pack_word_cues(words, opt, detail.text);
}

static std::vector<pm::subs::Cue> cues_from_proxy(
    const pm::stt::ProxyTranscript& detail)
{
    std::vector<pm::subs::Cue> cues;
    cues.reserve(detail.segments.size());
    for (const auto& s : detail.segments) {
        if (s.text.empty())
            continue;
        pm::subs::Cue c;
        c.text = s.text;
        c.t0_ms = s.t0_ms;
        c.t1_ms = s.t1_ms > s.t0_ms ? s.t1_ms : s.t0_ms + 1;
        cues.push_back(std::move(c));
    }
    return cues;
}

static bool write_transcribe_subtitles(
    BlockIO& io,
    const std::string& transcript,
    std::vector<pm::subs::Cue> cues,
    const std::filesystem::path& media_stem,
    nlohmann::json& extra,
    std::string& err,
    std::int64_t untimed_end_ms = 1000)
{
    pm::subs::WriterOptions wrap;
    wrap.max_chars = static_cast<int>(io.i64("subtitleMaxChars", 42));
    wrap.max_lines = static_cast<int>(io.i64("subtitleMaxLines", 2));
    wrap.untimed_end_ms = untimed_end_ms > 0 ? untimed_end_ms : 1000;
    std::filesystem::path stem = io.str("subtitlePath");
    if (stem.empty())
        stem = media_stem;
    std::vector<std::filesystem::path> written;
    if (!pm::subs::write_sidecars(io.str("subtitleFormat"), stem, transcript,
                                  std::move(cues), wrap, written, err))
        return false;
    if (written.empty())
        return true;
    nlohmann::json paths = nlohmann::json::array();
    for (const auto& p : written)
        paths.push_back(media::path::path_to_utf8(p));
    extra["subtitlePaths"] = paths;
    extra["subtitlePath"] = written.size() == 1 ? nlohmann::json(media::path::path_to_utf8(written[0])) : std::move(paths);
    return true;
}

static std::string overlay_clip(const std::string& s, size_t n = 42)
{
    if (s.size() <= n)
        return s;
    return s.substr(0, n > 3 ? n - 3 : n) + "...";
}

// ── audioTranscribe ───────────────────────────────────────────────────────────
// When `input` is an existing regular file: transcribe that file (no capture).
// Otherwise (empty, PREVIOUS text, missing path): record mic / desktop / mix
// for up to maxDurationMs ms (optional VAD silence-gate at silenceMs), then
// transcribe. Provider / model / apiKey override the saved settings when present.
// PREVIOUS / storeAs ← transcript text string, or STT JSON when json=true.

BlockResult audio_transcribe_block(BlockIO& io)
{
    namespace chr = std::chrono;
    using clk     = chr::steady_clock;

    std::string input_file = io.str("input");
    {
        // `.prev()` fills input from PREVIOUS. A prior block's text is not a
        // path — only treat input as a file when it exists on disk.
        while (!input_file.empty() && std::isspace(static_cast<unsigned char>(input_file.front())))
            input_file.erase(input_file.begin());
        while (!input_file.empty() && std::isspace(static_cast<unsigned char>(input_file.back())))
            input_file.pop_back();
        if (!input_file.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(media::path::path_from_utf8(input_file), ec) || ec) {
                logger::trace("[audioTranscribe] input is not an existing file, using live capture input=\"" +
                              (input_file.size() > 80 ? input_file.substr(0, 77) + "..." : input_file) + "\"");
                input_file.clear();
            }
        }
    }
    const int         max_ms      = std::max(500, static_cast<int>(io.i64("maxDurationMs", 30000)));
    const int         sil_ms      = static_cast<int>(io.i64("silenceMs", 1500));
    std::string       source_token = io.str("inputSource");
    if (source_token.empty())
        source_token = io.str("audioSource");
    std::string       device_name = io.str("device");
    if (device_name.empty())
        device_name = io.str("audioDevice");
    device_name = resolve_audio_input(device_name);
    const std::string desktop_device = io.str("desktopDevice");
    const float       mic_gain       = static_cast<float>(io.f64("micGain", 1.0));
    const float       desktop_gain   = static_cast<float>(io.f64("desktopGain", 1.0));
    std::string       wav_path    = io.str("outputPath");
    const std::string hud_mode    = io.str("mode");
    const bool        want_hud    = io.flag("hud", false) || hud_mode == "transcribe";
    const auto        stt_route   = transcribe_stt_route(io);

    // ── provider credentials ──────────────────────────────────────────────────
    const auto stt = pm::video::resolve_stt_route(stt_route);
    logger::trace("[audioTranscribe] resolve providerArg=" + (stt_route.provider.empty() ? std::string("(empty)") : stt_route.provider) +
                  " modelArg=" + (stt_route.model.empty() ? std::string("(empty)") : stt_route.model) +
                  " provider=" + stt.provider +
                  " model=" + stt.model +
                  (stt.error.empty() ? std::string{} : " error=" + stt.error));
    if (!stt.error.empty()) {
        io.error("audioTranscribe", "audioTranscribe: " + stt.error, 1);
        return BlockResult::Error;
    }

    std::unique_ptr<media::win::OverlayJob> overlay;
    if (want_hud) {
        overlay = std::make_unique<media::win::OverlayJob>("Transcribe");
        overlay->busy(stt.provider.empty() ? "working" : stt.provider);
    }

    // ── file transcription path ───────────────────────────────────────────────
    if (!input_file.empty()) {
        std::string transcript;
        std::vector<pm::subs::Cue> cues;
        std::int64_t cues_duration_ms = 0;
#if PM_HAS_AUDIO_IO
        cues_duration_ms = pm::audio::filter::duration_ms(input_file);
#endif
        try {
            if (stt.provider == "whisper") {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
                const auto whisper_probe = pm::stt::whisper_probe_model_path(stt.model);
                logger::trace("[audioTranscribe] file probe id=" + whisper_probe.model_id +
                              " filename=" + whisper_probe.filename +
                              " path=" + (whisper_probe.path.empty() ? std::string("(miss)") : whisper_probe.path));
                if (whisper_probe.path.empty()) {
                    if (overlay)
                        overlay->error("model missing");
                    io.error("audioTranscribe", "audioTranscribe: "
                             + pm::stt::whisper_model_not_found_message(whisper_probe), 1);
                    return BlockResult::Error;
                }
                const auto detail = pm::stt::whisper_local_transcribe_detailed(
                    input_file, whisper_probe.path, whisper_options_from_json(
                        stt_route.options.is_object() ? stt_route.options : io.json("providerOptions")));
                transcript = detail.text;
                cues = cues_from_whisper(detail, transcribe_grow(io));
#else
                throw std::runtime_error("whisper provider not available (build without FEATURE_WHISPER)");
#endif
            } else if (pm::audio::constants::is_windows_ai_stt_provider(stt.provider)) {
                transcript = pm::stt::windows_ai_stt_transcribe_file(input_file);
            } else if (pm::audio::constants::is_speech_proxy_provider(stt.provider)) {
                const auto detail = pm::stt::proxy_stt_transcribe_detailed(
                    input_file, stt.base_url, stt.api_key, stt.model);
                transcript = detail.text;
                cues = cues_from_proxy(detail);
                if (detail.duration_ms > 0)
                    cues_duration_ms = detail.duration_ms;
            } else {
                throw std::runtime_error("provider '" + stt.provider +
                    "' does not support file transcription; use 'whisper', 'tanit', 'openai', or 'openrouter'");
            }
        } catch (const std::exception& ex) {
            if (overlay)
                overlay->error(ex.what());
            io.error("audioTranscribe", std::string("audioTranscribe: ") + ex.what(), 1);
            return BlockResult::Error;
        }

        if (overlay)
            overlay->completed(overlay_clip(transcript));

        nlohmann::json transcript_json = {
            {"provider",   stt.provider},
            {"model",      stt.model},
            {"transcript", transcript},
            {"file",       input_file},
            {"segments",   cues_to_segments_json(cues)},
        };
        std::string sub_err;
        if (!write_transcribe_subtitles(io, transcript, cues, input_file, transcript_json, sub_err,
                                       cues_duration_ms)) {
            io.error("audioTranscribe", "audioTranscribe: " + sub_err, 1);
            return BlockResult::Error;
        }

        const std::string preview = transcript.size() > 80 ? transcript.substr(0, 77) + "..." : transcript;
        auto ev = io.ok("audioTranscribe",
            "transcript (" + std::to_string(transcript.size()) + " chars, file): " + preview);
        ev.set("transcript", transcript)
          .set("provider",   stt.provider)
          .set("model",      stt.model)
          .set("file",       input_file);
        if (transcript_json.contains("subtitlePath") && transcript_json["subtitlePath"].is_string())
            ev.set("subtitlePath", transcript_json["subtitlePath"].get<std::string>());
        if (io.flag("json", false))
            ev.result(Value::from_json(std::move(transcript_json)), ParamKind::json_value);
        else
            ev.result(transcript, ParamKind::string_);
        return BlockResult::Ok;
    }

    // ── record mic / desktop / mix ────────────────────────────────────────────
    constexpr double k_vad_rms = 200.0;
    constexpr double k_hud_rms = 2500.0;
    std::mutex              pcm_mu;
    std::vector<int16_t>    pcm;
    std::atomic<bool>       had_speech{false};
    std::atomic<bool>       vad_stop{false};
    std::atomic<clk::time_point> last_speech_tp{clk::now()};

    pm::audio::MixedCaptureConfig cap_cfg = pm::audio::make_mixed_capture_config(
        source_token, device_name, desktop_device, mic_gain, desktop_gain,
        pm::audio::CaptureSource::mic);
    cap_cfg.sample_rate = C::k_capture_sample_rate;
    cap_cfg.channels    = C::k_capture_channels;

#if defined(_WIN32)
    media::win::TranscriptionHud hud;
    if (want_hud) {
        if (!hud.show_at_cursor())
            logger::warn("[audioTranscribe] transcription HUD failed to show");
    }
#endif

    std::unique_ptr<pm::stt::ElevenLabsSTT> live_stt;
    std::mutex live_mu;
    std::string live_committed;
    std::string live_partial;
    std::string live_err;
    std::vector<pm::subs::Cue> live_cues;
    const auto cue_grow = transcribe_grow(io);
    const bool live_eleven = want_hud && stt.provider == "elevenlabs";
    if (live_eleven) {
        try {
            live_stt = std::make_unique<pm::stt::ElevenLabsSTT>();
            live_stt->on_partial = [&](const std::string& t) {
                std::lock_guard<std::mutex> lk(live_mu);
                live_partial = t;
            };
            live_stt->on_committed = [&](const std::string& t) {
                std::lock_guard<std::mutex> lk(live_mu);
                if (!t.empty())
                    live_committed = t;
                live_partial.clear();
            };
            live_stt->on_committed_timed = [&](const pm::stt::ElevenLabsSTT::TimedCommit& tc) {
                auto more = cues_from_el_timed(tc, cue_grow);
                std::lock_guard<std::mutex> lk(live_mu);
                if (!live_cues.empty() && !more.empty() && more.front().t0_ms <= 80)
                    live_cues = std::move(more);
                else
                    live_cues.insert(live_cues.end(), more.begin(), more.end());
            };
            live_stt->on_error = [&](const std::string& e) {
                std::lock_guard<std::mutex> lk(live_mu);
                if (live_err.empty())
                    live_err = e;
            };
            pm::stt::ElevenLabsSTT::Config scfg;
            scfg.api_key = stt.api_key;
            scfg.include_timestamps = true;
            if (!stt.model.empty() && stt.model != "tanit-speech-to-text")
                scfg.model_id = stt.model;
            live_stt->connect(scfg);
            logger::trace("[audioTranscribe] elevenlabs live STT connected");
        } catch (const std::exception& ex) {
            logger::warn(std::string("[audioTranscribe] elevenlabs live STT unavailable: ") + ex.what());
            live_stt.reset();
        }
    }

    pm::audio::MixedAudioCapture capture;
    std::string capture_err;
    logger::trace("[audioTranscribe] capture start source=" +
                  std::string(pm::audio::capture_source_name(cap_cfg.source)) +
                  " device=" + (device_name.empty() ? std::string{"<default>"} : device_name) +
                  " desktopDevice=" + (desktop_device.empty() ? std::string{"<default>"} : desktop_device) +
                  " micGain=" + std::to_string(mic_gain) +
                  " desktopGain=" + std::to_string(desktop_gain) +
                  " maxMs=" + std::to_string(max_ms) +
                  " silenceMs=" + std::to_string(sil_ms) +
                  " hud=" + (want_hud ? "1" : "0") +
                  " mode=" + (hud_mode.empty() ? std::string("(default)") : hud_mode));
    if (!capture.start(cap_cfg, [&](const int16_t* data, size_t frames) {
            if (!frames) return;
            const size_t samples = frames * cap_cfg.channels;
            { std::lock_guard<std::mutex> lk(pcm_mu); pcm.insert(pcm.end(), data, data + samples); }
            double sum = 0.0;
            for (size_t i = 0; i < samples; ++i) sum += static_cast<double>(data[i]) * data[i];
            const double rms = std::sqrt(sum / static_cast<double>(samples));
#if defined(_WIN32)
            if (hud.valid())
                hud.set_level(static_cast<float>(std::min(1.0, rms / k_hud_rms)));
#endif
            if (live_stt)
                live_stt->send_pcm(data, frames);
            if (sil_ms <= 0) return;
            const auto now = clk::now();
            if (rms >= k_vad_rms) {
                last_speech_tp.store(now);
                had_speech.store(true);
            } else if (had_speech.load()) {
                const auto sil = chr::duration_cast<chr::milliseconds>(now - last_speech_tp.load()).count();
                if (sil >= sil_ms) vad_stop.store(true);
            }
        }, capture_err)) {
#if defined(_WIN32)
        hud.destroy();
#endif
        if (live_stt)
            live_stt->close();
        if (overlay)
            overlay->error(capture_err.empty() ? "capture failed" : capture_err);
        io.error("audioTranscribe",
                 "audioTranscribe: capture.start: " +
                     (capture_err.empty() ? std::string("unknown error") : capture_err),
                 1);
        return BlockResult::Error;
    }

    const auto t0       = clk::now();
    const auto deadline = t0 + chr::milliseconds(max_ms);
    const char* stopped_by = "max_duration";
    const std::string source_name = pm::audio::capture_source_name(cap_cfg.source);
    if (overlay)
        overlay->busy(source_name);
    while (clk::now() < deadline) {
        if (vad_stop.load()) { stopped_by = "vad"; break; }
#if defined(_WIN32)
        if (hud.valid()) {
            hud.pump();
            if (hud.stop_requested()) { stopped_by = "hud"; break; }
        }
#endif
        if (overlay && overlay->stop_requested()) { stopped_by = "hud"; break; }
        if (io.cancelled()) {
            stopped_by = "cancelled";
            logger::trace("[audioTranscribe] cancel observed while recording");
            break;
        }
        if (overlay) {
            std::string detail = source_name;
            {
                std::lock_guard<std::mutex> lk(live_mu);
                if (!live_partial.empty())
                    detail = overlay_clip(live_partial);
                else if (!live_committed.empty())
                    detail = overlay_clip(live_committed);
            }
            const auto elapsed = chr::duration_cast<chr::milliseconds>(clk::now() - t0).count();
            overlay->progress(static_cast<uint64_t>(std::max<long long>(0, elapsed)),
                              static_cast<uint64_t>(max_ms),
                              detail);
        }
        std::this_thread::sleep_for(chr::milliseconds(want_hud ? 16 : 50));
    }
    capture.stop();
#if defined(_WIN32)
    hud.hide();
    hud.destroy();
#endif
    if (live_stt) {
        live_stt->commit();
        const auto live_deadline = clk::now() + chr::milliseconds(2500);
        std::chrono::steady_clock::time_point text_at{};
        bool saw_text = false;
        while (clk::now() < live_deadline) {
            {
                std::lock_guard<std::mutex> lk(live_mu);
                if (!live_err.empty())
                    break;
                if (!live_committed.empty() && !saw_text) {
                    saw_text = true;
                    text_at = clk::now();
                }
                if (saw_text && (!live_cues.empty()
                                 || clk::now() >= text_at + chr::milliseconds(1200)))
                    break;
            }
            std::this_thread::sleep_for(chr::milliseconds(40));
        }
        live_stt->close();
    }
    const auto total_ms = chr::duration_cast<chr::milliseconds>(clk::now() - t0).count();
    logger::trace("[audioTranscribe] capture stopped reason=" + std::string(stopped_by) +
                  " source=" + std::string(pm::audio::capture_source_name(cap_cfg.source)) +
                  " durationMs=" + std::to_string(total_ms));

    std::vector<int16_t> captured;
    { std::lock_guard<std::mutex> lk(pcm_mu); captured.swap(pcm); }
    logger::trace("[audioTranscribe] captured samples=" + std::to_string(captured.size()));
    if (captured.empty()) {
        if (overlay)
            overlay->error("no audio");
        io.error("audioTranscribe", "audioTranscribe: no audio captured (0 samples)", 1);
        return BlockResult::Error;
    }

    // ── write WAV ─────────────────────────────────────────────────────────────
    bool wav_is_temp = wav_path.empty();
    if (wav_is_temp) {
        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        wav_path = (ec ? std::string(".") : media::path::path_to_utf8(tmp)) + "/xblox_transcribe_" +
                   std::to_string(clk::now().time_since_epoch().count()) + ".wav";
    }
    if (!pm::stt::write_pcm_wav(wav_path, captured.data(), captured.size(), 16000)) {
        if (overlay)
            overlay->error("cannot write");
        io.error("audioTranscribe", "audioTranscribe: failed to write WAV: " + wav_path, 1);
        return BlockResult::Error;
    }
    logger::trace("[audioTranscribe] wav written path=" + wav_path +
                  " temp=" + std::to_string(wav_is_temp));

    // ── transcribe ────────────────────────────────────────────────────────────
    std::string transcript;
    std::vector<pm::subs::Cue> cues;
    try {
        if (overlay)
            overlay->busy("transcribing");
        logger::trace("[audioTranscribe] transcribe begin provider=" + stt.provider +
                      " model=" + stt.model);
        if (stt.provider == "whisper") {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
            const auto whisper_probe = pm::stt::whisper_probe_model_path(stt.model);
            logger::trace("[audioTranscribe] mic probe id=" + whisper_probe.model_id +
                          " filename=" + whisper_probe.filename +
                          " path=" + (whisper_probe.path.empty() ? std::string("(miss)") : whisper_probe.path));
            if (whisper_probe.path.empty()) {
                if (overlay)
                    overlay->error("model missing");
                io.error("audioTranscribe", "audioTranscribe: "
                         + pm::stt::whisper_model_not_found_message(whisper_probe), 1);
                return BlockResult::Error;
            }
            const auto detail = pm::stt::whisper_local_transcribe_detailed(
                wav_path, whisper_probe.path, whisper_options_from_json(
                    stt_route.options.is_object() ? stt_route.options : io.json("providerOptions")));
            transcript = detail.text;
            cues = cues_from_whisper(detail, cue_grow);
#else
            throw std::runtime_error("whisper provider not available (build without FEATURE_WHISPER)");
#endif
        } else if (pm::audio::constants::is_windows_ai_stt_provider(stt.provider)) {
            transcript = pm::stt::windows_ai_stt_transcribe_file(wav_path);
        } else if (pm::audio::constants::is_speech_proxy_provider(stt.provider)) {
            const auto detail = pm::stt::proxy_stt_transcribe_detailed(
                wav_path, stt.base_url, stt.api_key, stt.model);
            transcript = detail.text;
            cues = cues_from_proxy(detail);
        } else {
            std::string live_text;
            {
                std::lock_guard<std::mutex> lk(live_mu);
                live_text = live_committed.empty() ? live_partial : live_committed;
                if (cues.empty())
                    cues = live_cues;
            }
            if (!live_text.empty()) {
                transcript = std::move(live_text);
                logger::trace("[audioTranscribe] using live elevenlabs transcript chars=" +
                              std::to_string(transcript.size()));
            } else {
            // ElevenLabs: stream raw PCM over WebSocket
            pm::stt::ElevenLabsSTT sttws;
            std::mutex              cb_mu;
            std::condition_variable cb_cv;
            std::string             committed, err_msg;
            std::vector<pm::subs::Cue> timed_cues;
            std::atomic<bool>       done{false};
            std::atomic<bool>       timed_done{false};
            sttws.on_committed = [&](const std::string& t) {
                std::lock_guard<std::mutex> lk(cb_mu);
                committed = t; done.store(true); cb_cv.notify_all();
            };
            sttws.on_committed_timed = [&](const pm::stt::ElevenLabsSTT::TimedCommit& tc) {
                auto more = cues_from_el_timed(tc, cue_grow);
                std::lock_guard<std::mutex> lk(cb_mu);
                timed_cues.insert(timed_cues.end(), more.begin(), more.end());
                timed_done.store(true);
                cb_cv.notify_all();
            };
            sttws.on_error = [&](const std::string& e) {
                std::lock_guard<std::mutex> lk(cb_mu);
                if (err_msg.empty()) err_msg = e; done.store(true); cb_cv.notify_all();
            };
            pm::stt::ElevenLabsSTT::Config scfg;
            scfg.api_key = stt.api_key;
            scfg.include_timestamps = true;
            if (stt.model != "tanit-speech-to-text") scfg.model_id = stt.model;
            sttws.connect(scfg);
            constexpr size_t kChunk = 4000;
            for (size_t i = 0; i < captured.size(); i += kChunk)
                sttws.send_pcm(captured.data() + i, std::min(kChunk, captured.size() - i));
            sttws.commit();
            std::unique_lock<std::mutex> lk(cb_mu);
            cb_cv.wait_for(lk, std::chrono::seconds(15), [&]{ return done.load(); });
            if (done.load() && err_msg.empty())
                cb_cv.wait_for(lk, std::chrono::milliseconds(1500),
                               [&]{ return timed_done.load(); });
            sttws.close();
            if (!err_msg.empty() && committed.empty()) throw std::runtime_error(err_msg);
            transcript = committed;
            if (cues.empty())
                cues = std::move(timed_cues);
            }
        }
        logger::trace("[audioTranscribe] transcribe end chars=" + std::to_string(transcript.size()) +
                      (transcript.empty() ? " empty=1" : ""));
    } catch (const std::exception& ex) {
        logger::trace(std::string("[audioTranscribe] transcribe failed: ") + ex.what());
        if (wav_is_temp) { std::error_code ec; std::filesystem::remove(wav_path, ec); }
        if (overlay)
            overlay->error(ex.what());
        io.error("audioTranscribe", std::string("audioTranscribe: ") + ex.what(), 1);
        return BlockResult::Error;
    }

    nlohmann::json transcript_json = {
        {"provider", stt.provider},
        {"model", stt.model},
        {"transcript", transcript},
        {"samples", static_cast<long long>(captured.size())},
        {"duration_seconds", static_cast<double>(total_ms) / 1000.0},
        {"durationMs", static_cast<long long>(total_ms)},
        {"stoppedBy", std::string(stopped_by)},
        {"inputSource", pm::audio::capture_source_name(cap_cfg.source)},
        {"segments", cues_to_segments_json(cues)},
    };
    if (!wav_is_temp)
        transcript_json["wav"] = wav_path;
    std::string sub_err;
    const std::filesystem::path sub_stem =
        !io.str("subtitlePath").empty() ? io.str("subtitlePath") : wav_path;
    if (!write_transcribe_subtitles(io, transcript, cues, sub_stem, transcript_json, sub_err,
                                   static_cast<std::int64_t>(total_ms))) {
        if (wav_is_temp) { std::error_code ec; std::filesystem::remove(wav_path, ec); }
        io.error("audioTranscribe", "audioTranscribe: " + sub_err, 1);
        return BlockResult::Error;
    }

    if (wav_is_temp) { std::error_code ec; std::filesystem::remove(wav_path, ec); }
    if (overlay)
        overlay->completed(overlay_clip(transcript));

    const std::string preview = transcript.size() > 80 ? transcript.substr(0, 77) + "..." : transcript;
    auto ev = io.ok("audioTranscribe",
        "transcript (" + std::to_string(transcript.size()) + " chars, " + stopped_by + "): " + preview);
    ev.set("transcript", transcript)
      .set("provider",   stt.provider)
      .set("model",      stt.model)
      .set("durationMs", static_cast<long long>(total_ms))
      .set("samples",    static_cast<long long>(captured.size()))
      .set("stoppedBy",  std::string(stopped_by))
      .set("inputSource", pm::audio::capture_source_name(cap_cfg.source));
    if (!wav_is_temp) ev.set("wavPath", wav_path);
    if (transcript_json.contains("subtitlePath") && transcript_json["subtitlePath"].is_string())
        ev.set("subtitlePath", transcript_json["subtitlePath"].get<std::string>());
    if (io.flag("json", false))
        ev.result(Value::from_json(std::move(transcript_json)), ParamKind::json_value);
    else
        ev.result(transcript, ParamKind::string_);
    return BlockResult::Ok;
}

static pm::tts::TtsSynthesisOptions synthesis_options_from_io(BlockIO& io)
{
    pm::tts::TtsSynthesisOptions opts;
    opts.send_voice_settings = io.flag("sendVoiceSettings", C::k_elevenlabs_send_voice_settings);
    opts.chunk_sentences     = io.flag("chunkSentences", C::k_elevenlabs_chunk_sentences);
    opts.chunk_target_chars  = static_cast<std::size_t>(
        io.i64("chunkTargetChars", static_cast<long long>(C::k_elevenlabs_chunk_target_chars)));
    opts.chunk_max_chars     = static_cast<std::size_t>(
        io.i64("chunkMaxChars", static_cast<long long>(C::k_elevenlabs_chunk_max_chars)));
    opts.stability           = io.f64("stability", C::k_elevenlabs_stability);
    opts.similarity_boost    = io.f64("similarityBoost", C::k_elevenlabs_similarity_boost);
    opts.style               = io.f64("style", C::k_elevenlabs_style);
    return opts;
}

// ── audioSpeak ────────────────────────────────────────────────────────────────

#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE

// VibevoiceSpeakInstance  -  scope-level token that calls VibevoiceEngine::global()
// on behalf of one xblox block instance.  Each scope cache entry holds one of
// these; they all share the same underlying engine (VibevoiceEngine::global() is
// a process singleton, so simultaneous load/tts calls are serialised by its
// internal mutex).  The destructor does NOT unload the engine  -  it stays warm
// for the next block in the flow.  The engine is unloaded when the process exits
// (VibevoiceEngine::~VibevoiceEngine) or when ensure() is called with different
// args.
struct VibevoiceSpeakInstance {
    std::string model_path;
    std::string tokenizer_path;
    std::string voice_path;
    bool        claimed = false;   // true when this instance last called ensure()

    // Ensure the global engine is loaded for (m, t, v). Returns 0 on success.
    int ensure(const std::string& m, const std::string& t, const std::string& v,
               int n_threads = 0)
    {
        const int rc = pm::tts::VibevoiceEngine::global().ensure(m, t, v, n_threads);
        if (rc == 0) { model_path = m; tokenizer_path = t; voice_path = v; claimed = true; }
        return rc;
    }
};

// FNV-1a hash for unnamed vibevoice instance keys.
static std::string vv_args_hash(const std::string& m, const std::string& t,
                                 const std::string& v)
{
    const std::string sig = m + "|" + t + "|" + v;
    std::uint64_t h = media::xblox::constants::kFnv1a64OffsetBasis;
    for (unsigned char c : sig) { h ^= c; h *= media::xblox::constants::kFnv1a64Prime; }
    return std::to_string(h);
}

#endif // FEATURE_VIBEVOICE

#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS

struct MossSpeakInstance {
    std::string model_path;
    std::string codec_path;
    std::string tokenizer_path;
    bool        claimed = false;

    int ensure(const std::string& m, const std::string& c, const std::string& t)
    {
        const int rc = pm::tts::MossNanoEngine::global().ensure(m, c, t);
        if (rc == 0) { model_path = m; codec_path = c; tokenizer_path = t; claimed = true; }
        return rc;
    }
};

static std::string moss_args_hash(const std::string& m, const std::string& c,
                                  const std::string& t)
{
    const std::string sig = m + "|" + c + "|" + t;
    std::uint64_t h = media::xblox::constants::kFnv1a64OffsetBasis;
    for (unsigned char ch : sig) { h ^= ch; h *= media::xblox::constants::kFnv1a64Prime; }
    return std::to_string(h);
}

#endif // FEATURE_MOSS_TTS

static std::string effective_tts_provider(const std::string& block_provider)
{
    if (!block_provider.empty()) return block_provider;
    media::runtime_settings::ChatProviderSettings chat;
    std::string err;
    if (media::runtime_settings::load_chat_provider(chat, err))
        return chat.tts_provider;
    return {};
}

static bool tts_model_looks_like_cloud_id(const std::string& model)
{
    return model.rfind("eleven_", 0) == 0 || model.rfind("tanit-", 0) == 0;
}

// Converts `text` (or PREVIOUS string) to speech via a pixlwiz / ElevenLabs /
// vibevoice / moss TTS provider. Plays through the selected/app/default output device.
// When `outputPath` is set the audio is also saved.
// PREVIOUS / storeAs ← output file path (when saved) or input text.

BlockResult audio_speak_block(BlockIO& io)
{
    const std::string text_arg = io.str("text");
    if (text_arg.empty()) {
        io.error("audioSpeak", "audioSpeak: text is empty (pass `text` or chain from a previous block via PREVIOUS)", 2);
        return BlockResult::Error;
    }

    pm::tts::TtsTextInputResult text_in;
    std::string text_err;
    if (!pm::tts::resolve_tts_text(text_arg, text_in, text_err)) {
        const std::string detail = text_err.empty()
            ? "text resolution failed (no details; input length="
              + std::to_string(text_arg.size()) + ")"
            : text_err;
        io.error("audioSpeak", std::string("audioSpeak: ") + detail, 1);
        return BlockResult::Error;
    }
    const std::string& text = text_in.text;

    std::unique_ptr<media::win::OverlayJob> overlay;
    if (io.flag("hud", false)) {
        overlay = std::make_unique<media::win::OverlayJob>("Speak");
        overlay->busy(overlay_clip(text));
    }
    auto hud_fail = [&](const std::string& d) {
        if (overlay)
            overlay->error(overlay_clip(d, 48));
    };
    auto hud_done = [&] {
        if (overlay)
            overlay->completed(overlay_clip(text));
    };
    auto speak_cancelled = [&] {
        return io.cancelled() || (overlay && overlay->stop_requested());
    };

    const std::string prov_arg  = io.str("provider");
    const std::string model_arg = io.str("model");
    const std::string voice_arg = io.str("voice");
    const std::string output_device = resolve_audio_output(io.str("outputDevice"));
    std::string       out_path  = io.str("outputPath");
    const std::string effective_prov = effective_tts_provider(prov_arg);
    logger::info("[audioSpeak] chars=" + std::to_string(text.size())
        + " provider=" + (effective_prov.empty() ? std::string("(unset)") : effective_prov)
        + " model=" + (model_arg.empty() ? std::string("(default)") : model_arg)
        + " device=" + (output_device.empty() ? std::string("(default)") : output_device)
        + " out=" + (out_path.empty() ? std::string("(none)") : out_path));

    // ── vibevoice local path ──────────────────────────────────────────────────
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
    if (effective_prov == "vibevoice") {
        const std::string tokenizer_arg = io.str("tokenizer");
        // refAudio: optional reference WAV for voice cloning (vibevoice 1.5B only).
        // When set, voice_arg must be empty and the 1.5B model must be loaded.
        const std::string ref_audio_arg = io.str("refAudio");
        if (model_arg.empty()) {
            hud_fail("model required");
            io.error("audioSpeak",
                "audioSpeak (vibevoice): 'model' (GGUF path) is required. "
                "Set it on the block or in the document context as ${vibevoice_model}. "
                "Alternatively configure vibevoice_model in Chat Settings (tts_provider=vibevoice).", 2);
            return BlockResult::Error;
        }
        if (tokenizer_arg.empty()) {
            hud_fail("tokenizer required");
            io.error("audioSpeak",
                "audioSpeak (vibevoice): 'tokenizer' (GGUF path) is required. "
                "Set it on the block or in the document context as ${vibevoice_tokenizer}.", 2);
            return BlockResult::Error;
        }

        // Resolve cached instance from run scope (keyed by explicit `instance`
        // name or an args hash  -  mirrors the videoDetect pattern).
        const std::string inst_name = io.str("instance");
        const std::string key = inst_name.empty()
            ? "audioSpeak:vv:#" + vv_args_hash(model_arg, tokenizer_arg, voice_arg)
            : "audioSpeak:vv:" + inst_name;

        VibevoiceSpeakInstance  local;
        VibevoiceSpeakInstance* inst = nullptr;
        if (RunScope* scope = io.scope()) {
            inst = &scope->acquire<VibevoiceSpeakInstance>(
                key, [] { return std::make_shared<VibevoiceSpeakInstance>(); });
        } else {
            inst = &local;
        }

        if (overlay)
            overlay->busy("loading");
        const int lrc = inst->ensure(model_arg, tokenizer_arg, voice_arg);
        if (lrc != 0) {
            hud_fail("engine load failed");
            io.error("audioSpeak", "audioSpeak (vibevoice): engine load failed (rc=" +
                     std::to_string(lrc) + ")", 1);
            return BlockResult::Error;
        }

        // Synthesize into a temp WAV via the shared engine.
        namespace fs = std::filesystem;
        const std::string tmp = media::path::path_to_utf8((fs::temp_directory_path() /
            ("vv_speak_" + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()) + ".wav")));

        namespace chr = std::chrono;
        if (overlay)
            overlay->busy("synthesizing");
        const auto t0 = chr::steady_clock::now();
        const int rc = pm::tts::VibevoiceEngine::global().tts(
            text, voice_arg, ref_audio_arg, tmp);
        const long long synth_ms = chr::duration_cast<chr::milliseconds>(
            chr::steady_clock::now() - t0).count();

        if (rc != 0) {
            std::error_code ec; fs::remove(media::path::path_from_utf8(tmp), ec);
            hud_fail("synthesis failed");
            io.error("audioSpeak", "audioSpeak (vibevoice): synthesis failed (rc=" +
                     std::to_string(rc) + ")", 1);
            return BlockResult::Error;
        }

        // Optional save: copy temp -> outputPath.
        if (!out_path.empty()) {
            std::error_code ec;
            if (auto par = media::path::path_from_utf8(out_path).parent_path(); !par.empty())
                fs::create_directories(par, ec);
            fs::copy_file(media::path::path_from_utf8(tmp), media::path::path_from_utf8(out_path),
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove(media::path::path_from_utf8(tmp), ec);
                hud_fail("cannot write");
                io.error("audioSpeak", "audioSpeak (vibevoice): save failed: " + out_path, 1);
                return BlockResult::Error;
            }
        }

        try {
            if (overlay)
                overlay->busy("playing");
            play_file_tts(tmp, output_device, speak_cancelled);
        } catch (const std::exception& ex) {
            std::error_code rec; fs::remove(media::path::path_from_utf8(tmp), rec);
            hud_fail(ex.what());
            io.error("audioSpeak",
                     std::string("audioSpeak (vibevoice): playback failed: ") + ex.what(), 1);
            return BlockResult::Error;
        }
        std::error_code ec; fs::remove(media::path::path_from_utf8(tmp), ec);
        hud_done();

        const std::string preview = text.size() > 80 ? text.substr(0, 77) + "..." : text;
        auto ev = io.ok("audioSpeak", "spoke (vibevoice, " + std::to_string(synth_ms) + " ms): " + preview);
        ev.set("text",     text)
          .set("provider", std::string("vibevoice"))
          .set("model",    model_arg)
          .set("outputDevice", output_device)
          .set("synthMs",  synth_ms);
        if (text_in.from_file)
            ev.set("textSource", text_in.source_path);
        if (!text_in.preparser_id.empty())
            ev.set("textPreparser", text_in.preparser_id);
        if (out_path.empty()) {
            ev.result(text, ParamKind::string_);
        } else {
            ev.set("path", out_path);
            ev.result(out_path, ParamKind::audio_path);
        }
        return BlockResult::Ok;
    }
#endif // FEATURE_VIBEVOICE

#if defined(FEATURE_MOSS_TTS) && FEATURE_MOSS_TTS
    if (effective_prov == "moss") {
        std::string model_hint = model_arg;
        if (tts_model_looks_like_cloud_id(model_hint))
            model_hint.clear();
        const auto models = media::local_models::probe_moss_tts_model_paths(
            model_hint, io.str("codec"), io.str("tokenizer"));
        if (!models.complete()) {
            hud_fail("model missing");
            io.error("audioSpeak",
                     "audioSpeak (moss): "
                         + media::local_models::moss_tts_models_not_found_message(models),
                     1);
            return BlockResult::Error;
        }

        std::string ref_audio_arg = io.str("refAudio");
        if (!ref_audio_arg.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(media::path::path_from_utf8(ref_audio_arg), ec) || ec) {
                io.error("audioSpeak",
                         "audioSpeak (moss): refAudio not found: " + ref_audio_arg, 1);
                return BlockResult::Error;
            }
        }

        const std::string inst_name = io.str("instance");
        const std::string key = inst_name.empty()
            ? "audioSpeak:moss:#" + moss_args_hash(
                  models.model_path, models.codec_path, models.tokenizer_path)
            : "audioSpeak:moss:" + inst_name;

        MossSpeakInstance  local;
        MossSpeakInstance* inst = nullptr;
        if (RunScope* scope = io.scope()) {
            inst = &scope->acquire<MossSpeakInstance>(
                key, [] { return std::make_shared<MossSpeakInstance>(); });
        } else {
            inst = &local;
        }

        if (overlay)
            overlay->busy("loading");
        const int lrc = inst->ensure(
            models.model_path, models.codec_path, models.tokenizer_path);
        if (lrc != 0) {
            hud_fail("engine load failed");
            io.error("audioSpeak",
                     "audioSpeak (moss): engine load failed (rc=" + std::to_string(lrc)
                         + "): " + pm::tts::MossNanoEngine::global().last_error(),
                     1);
            return BlockResult::Error;
        }

        namespace fs = std::filesystem;
        const std::string tmp = media::path::path_to_utf8((fs::temp_directory_path() /
            ("moss_speak_" + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()) + ".wav")));

        namespace chr = std::chrono;
        if (overlay)
            overlay->busy("synthesizing");
        const auto t0 = chr::steady_clock::now();
        const int rc = pm::tts::MossNanoEngine::global().tts(text, ref_audio_arg, tmp);
        const long long synth_ms = chr::duration_cast<chr::milliseconds>(
            chr::steady_clock::now() - t0).count();

        if (rc != 0) {
            std::error_code ec; fs::remove(media::path::path_from_utf8(tmp), ec);
            hud_fail("synthesis failed");
            io.error("audioSpeak",
                     "audioSpeak (moss): synthesis failed (rc=" + std::to_string(rc)
                         + "): " + pm::tts::MossNanoEngine::global().last_error(),
                     1);
            return BlockResult::Error;
        }

        if (!out_path.empty()) {
            std::error_code ec;
            if (auto par = media::path::path_from_utf8(out_path).parent_path(); !par.empty())
                fs::create_directories(par, ec);
            fs::copy_file(media::path::path_from_utf8(tmp), media::path::path_from_utf8(out_path), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove(media::path::path_from_utf8(tmp), ec);
                hud_fail("cannot write");
                io.error("audioSpeak", "audioSpeak (moss): save failed: " + out_path, 1);
                return BlockResult::Error;
            }
        }

        try {
            if (overlay)
                overlay->busy("playing");
            play_file_tts(tmp, output_device, speak_cancelled);
        } catch (const std::exception& ex) {
            std::error_code rec; fs::remove(media::path::path_from_utf8(tmp), rec);
            hud_fail(ex.what());
            io.error("audioSpeak",
                     std::string("audioSpeak (moss): playback failed: ") + ex.what(), 1);
            return BlockResult::Error;
        }
        std::error_code ec; fs::remove(media::path::path_from_utf8(tmp), ec);
        hud_done();

        const std::string preview = text.size() > 80 ? text.substr(0, 77) + "..." : text;
        auto ev = io.ok("audioSpeak",
            "spoke (moss, " + std::to_string(synth_ms) + " ms): " + preview);
        ev.set("text",     text)
          .set("provider", std::string("moss"))
          .set("model",    models.model_path)
          .set("codec",    models.codec_path)
          .set("tokenizer", models.tokenizer_path)
          .set("outputDevice", output_device)
          .set("synthMs",  synth_ms);
        if (text_in.from_file)
            ev.set("textSource", text_in.source_path);
        if (!text_in.preparser_id.empty())
            ev.set("textPreparser", text_in.preparser_id);
        if (out_path.empty()) {
            ev.result(text, ParamKind::string_);
        } else {
            ev.set("path", out_path);
            ev.result(out_path, ParamKind::audio_path);
        }
        return BlockResult::Ok;
    }
#else
    if (effective_prov == "moss") {
        hud_fail("moss unavailable");
        io.error("audioSpeak",
                 "audioSpeak (moss): unavailable in this build (FEATURE_MOSS_TTS=OFF)", 1);
        return BlockResult::Error;
    }
#endif // FEATURE_MOSS_TTS

    // ── ElevenLabs / pixlwiz path ─────────────────────────────────────────────
    const auto tts = bp::resolve_tts({ prov_arg, model_arg, voice_arg });
    if (!tts.error.empty()) {
        hud_fail(tts.error);
        io.error("audioSpeak", "audioSpeak: " + tts.error, 1);
        return BlockResult::Error;
    }

    std::vector<uint8_t> audio_bytes;
    try {
        if (overlay)
            overlay->busy(tts.provider.empty() ? "synthesizing" : tts.provider);
        if (tts.provider == "elevenlabs") {
            pm::tts::ElevenLabsTTSConfig cfg;
            cfg.api_key = tts.api_key;
            if (!tts.voice.empty()) cfg.voice_id = tts.voice;
            if (!tts.model.empty()) cfg.model_id = tts.model;
            pm::tts::apply_synthesis_options(cfg, synthesis_options_from_io(io));
            cfg.should_cancel = []() -> bool { return media::cli::cancel_requested(); };
            audio_bytes = pm::tts::elevenlabs_tts_synthesize(text, cfg);
        } else {
            audio_bytes = pm::tts::proxy_tts_synthesize(
                text, tts.base_url, tts.api_key, tts.model, tts.voice);
        }
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        const std::string detail = (what && what[0] != '\0')
            ? std::string(what)
            : "unknown exception (no message)";
        hud_fail(detail);
        io.error("audioSpeak",
                 "audioSpeak: synthesis failed (provider=" + tts.provider
                 + ", model=" + tts.model + ", voice=" + tts.voice
                 + ", textLen=" + std::to_string(text.size()) + "): " + detail,
                 1);
        return BlockResult::Error;
    }

    if (!out_path.empty()) {
        std::error_code ec;
        if (auto par = media::path::path_from_utf8(out_path).parent_path(); !par.empty())
            std::filesystem::create_directories(par, ec);
        std::ofstream ofs(media::path::path_from_utf8(out_path), std::ios::out | std::ios::binary);
        if (!ofs) {
            std::string why = "cannot write output file";
            if (const int err = errno; err == EACCES || err == EPERM)
                why = "access denied (output folder is not writable)";
            else if (err != 0)
                why += std::string(" (") + std::strerror(err) + ")";
            hud_fail(why);
            io.error("audioSpeak", "audioSpeak: " + why + ": " + out_path, 1);
            return BlockResult::Error;
        }
        ofs.write(reinterpret_cast<const char*>(audio_bytes.data()),
                  static_cast<std::streamsize>(audio_bytes.size()));
        ofs.close();
        if (!ofs) {
            hud_fail("cannot write");
            io.error("audioSpeak", "audioSpeak: write failed: " + out_path, 1);
            return BlockResult::Error;
        }
    }

    if (audio_bytes.empty()) {
        hud_fail("no audio");
        io.error("audioSpeak", "audioSpeak: provider returned no audio bytes", 1);
        return BlockResult::Error;
    }

    try {
        if (overlay)
            overlay->busy("playing");
        play_memory_tts(audio_bytes.data(), audio_bytes.size(), output_device, speak_cancelled);
    } catch (const std::exception& ex) {
        hud_fail(ex.what());
        io.error("audioSpeak", std::string("audioSpeak: playback failed: ") + ex.what(), 1);
        return BlockResult::Error;
    }
    hud_done();

    logger::info("[audioSpeak] spoke bytes=" + std::to_string(audio_bytes.size())
        + " provider=" + tts.provider
        + (out_path.empty() ? std::string{} : (" path=" + out_path)));
    const std::string preview = text.size() > 80 ? text.substr(0, 77) + "..." : text;
    auto ev = io.ok("audioSpeak", "spoke: " + preview);
    ev.set("text",     text)
      .set("provider", tts.provider)
      .set("model",    tts.model)
      .set("outputDevice", output_device)
      .set("bytes",    static_cast<long long>(audio_bytes.size()));
    if (text_in.from_file)
        ev.set("textSource", text_in.source_path);
    if (!text_in.preparser_id.empty())
        ev.set("textPreparser", text_in.preparser_id);
    if (out_path.empty()) {
        ev.result(text, ParamKind::string_);
    } else {
        ev.set("path", out_path);
        ev.result(out_path, ParamKind::audio_path);
    }
    return BlockResult::Ok;
}

#endif // FEATURE_STT

} // namespace

// ── registration ──────────────────────────────────────────────────────────────

void register_audio_blocks(BlockRegistry& registry)
{
#if PM_HAS_AUDIO_IO
    namespace C = pm::audio::constants;
    using J = nlohmann::json;

    const std::vector<ParamDef> audio_capture_source_params = {
        pd_enum("inputSource", {"mic", "desktop", "mix"}).grp("source").dflt("mic")
            .desc("Capture source. Default: mic (microphone). desktop = system loopback; mix = mic + desktop."),
        pd_audio_input_device("device").adv().grp("source").lbl("Microphone").dflt("")
            .desc("Mic device for mic/mix. Default: empty → App Settings audio_input_device, then system default."),
        pd_str("desktopDevice").adv().grp("source").lbl("Desktop audio").dflt("")
            .desc("Loopback device substring for desktop/mix. Default: empty → default render device."),
        pd_float("micGain").adv().grp("source").dflt(1.0).range(0.0, 4.0)
            .desc("Mic level multiplier (linear) for mix. Default: 1.0."),
        pd_float("desktopGain").adv().grp("source").dflt(1.0).range(0.0, 4.0)
            .desc("Desktop level multiplier for desktop/mix. Default: 1.0."),
        pd_sample_rate("sampleRate").adv().grp("source").dflt(0).range(0, 192000)
            .desc("Sample rate Hz (0 = default 48000). AAC (.m4a) on Windows is encoded at 48000 Hz (resampled if needed)."),
        pd_audio_channels("channels").adv().grp("source").dflt(0).range(0, 2)
            .desc("Channels: 0 = default stereo, 1 = mono, 2 = stereo. Applies to .wav and .m4a/.aac."),
    };

    registry["audioListDevices"] = bdb(
        "audioListDevices", audio_list_devices_block,
        "List Audio Devices", "Audio",
        "List microphone and desktop/loopback capture devices.",
        {{"kind","audioListDevices"}, {"storeAs","audioDevices"}})
        .params({
            pd_store_as().dflt("audioDevices")
                .desc("Variable for PREVIOUS / downstream blocks. Default: audioDevices."),
        })
        .primary_output(ParamKind::json_value, "Audio devices")
        .exec({false, false, false});

    registry["audioPlay"] = bdb(
        "audioPlay", audio_play_block,
        "Play Audio", "Audio",
        "Play an audio file (MP3, WAV, or FLAC). Uses PREVIOUS as the file path when path is empty.",
        {{"kind","audioPlay"}, {"path",""}, {"outputDevice",""}, {"storeAs",""}})
        .params({
            pd_audio("path").in().lbl("Audio file").prev().dflt("")
                .desc("Audio file to play. Default: empty → PREVIOUS chain value."),
            pd_audio_output_device("outputDevice").adv().grp("device").lbl("Speaker").dflt("")
                .desc("Playback device. Default: empty → App Settings audio_output_device, then system default."),
            pd_store_as().dflt("")
                .desc("Optional variable for PREVIOUS. Default: empty (PREVIOUS only)."),
        })
        .primary_output(ParamKind::audio_path, "Played audio")
        .exec({false, true, true});

    registry["audioRecord"] = bdb(
        "audioRecord", audio_record_block,
        "Record Audio", "Audio",
        "Record mic, desktop, or mixed audio to a file. outputPath extension selects format: "
        ".wav = PCM (48 kHz stereo default; long takes spill PCM to disk after ~30 s in RAM); "
        ".m4a or .aac = AAC-LC (Windows only). Optional filter runs after capture "
        "(DeepFilterNet → 48 kHz mono WAV; not on the callback). "
        "Stores the output path in PREVIOUS / storeAs. Optional hud / hudMode shows "
        "the Record overlay pill and/or the app taskbar button (Stop ends the take early).",
        {{"kind","audioRecord"}, {"inputSource","mic"}, {"durationMs",3000}, {"device",""},
         {"desktopDevice",""}, {"micGain",1.0}, {"desktopGain",1.0},
         {"sampleRate",0}, {"channels",0}, {"outputPath",""},
         {"filter","off"}, {"filterModel",""}, {"attenLim",100.0},
         {"postFilter",false}, {"compensateDelay",true},
         {"hud",false}, {"hudMode","overlay"}, {"storeAs","audioPath"}})
        .params([&] {
            auto p = audio_capture_source_params;
            p.insert(p.end(), {
                pd_ms("durationMs").in().lbl("Duration").req().dflt(3000).range(100, 3600000)
                    .desc("Recording duration in milliseconds. Default: 3000."),
                pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
                    .desc("Output path; extension selects format. "
                          ".wav = PCM WAV; .m4a or .aac = AAC-LC (Windows only). "
                          "Default: empty → temporary .wav in the system temp directory. "
                          "filter requires .wav."),
                pd_enum("filter", {"off", "deepfilter", "gtcrn"}).grp("filter").dflt("off")
                    .desc("Enhance after capture. off = dry file. "
                          "deepfilter = DeepFilterNet @ 48 kHz mono. gtcrn = reserved."),
                pd_str("filterModel").adv().grp("filter").dflt("")
                    .desc("Optional DeepFilterNet tar.gz. Default: empty → ${MODELS_DIR}/deepfilter."),
                pd_float("attenLim").adv().grp("filter").dflt(100.0).range(0.0, 100.0)
                    .desc("Attenuation limit in dB (in-process libDF only). Default: 100."),
                pd_bool("postFilter").adv().grp("filter").dflt(false)
                    .desc("Over-attenuate very noisy sections. Implies deepfilter when filter is off."),
                pd_bool("compensateDelay").adv().grp("filter").dflt(true)
                    .desc("Compensate STFT / model lookahead. Default: true."),
            });
            auto hud = pd_status_hud_params(false, "overlay");
            p.insert(p.end(),
                     std::make_move_iterator(hud.begin()),
                     std::make_move_iterator(hud.end()));
            p.push_back(pd_store_as().dflt("audioPath")
                    .desc("Variable for PREVIOUS / downstream blocks. Default: audioPath."));
            return p;
        }())
        .primary_output(ParamKind::audio_path, "Recorded audio")
        .exec({false, true, true});

    registry["audioRecordStart"] = bdb(
        "audioRecordStart", audio_record_start_block,
        "Start Recording", "Audio",
        "Start a mic/desktop/mix recording session and store its session id. "
        "Optional outputPath (.wav or .m4a/.aac) may be set here or on audioRecordStop.",
        {{"kind","audioRecordStart"}, {"inputSource","mic"}, {"session",""}, {"device",""},
         {"desktopDevice",""}, {"micGain",1.0}, {"desktopGain",1.0},
         {"outputPath",""}, {"storeAs","recordSession"}})
        .params([&] {
            auto p = audio_capture_source_params;
            p.insert(p.end(), {
                pd_str("session").in().dflt("")
                    .desc("Recording session id. Default: empty → auto-generated unique id."),
                pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
                    .desc("Output path; extension selects .wav vs .m4a/.aac (see audioRecord). "
                          "Default: empty → temporary .wav at start; audioRecordStop outputPath overrides."),
                pd_store_as().dflt("recordSession")
                    .desc("Variable for PREVIOUS / downstream blocks. Default: recordSession."),
            });
            return p;
        }())
        .primary_output(ParamKind::string_, "Recording session")
        .exec({false, true, false});

    registry["audioRecordStop"] = bdb(
        "audioRecordStop", audio_record_stop_block,
        "Stop Recording", "Audio",
        "Stop a recording session and finalize the output file (.wav or .m4a/.aac by extension).",
        {{"kind","audioRecordStop"}, {"session",""}, {"outputPath",""}, {"storeAs","audioPath"}})
        .params({
            pd_str("session").in().req().dflt("")
                .desc("Session id from audioRecordStart. Default: empty (required at runtime)."),
            pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
                .desc("Output path; extension selects .wav vs .m4a/.aac (see audioRecord). "
                      "Default: empty → path from audioRecordStart, or temporary .wav."),
            pd_store_as().dflt("audioPath")
                .desc("Variable for PREVIOUS / downstream blocks. Default: audioPath."),
        })
        .primary_output(ParamKind::audio_path, "Recorded audio")
        .exec({false, true, true});

    registry["audioFilter"] = bdb(
        "audioFilter", audio_filter_block,
        "Filter Audio", "Audio",
        "Enhance a recorded audio file. Default filter is DeepFilterNet (48 kHz). "
        "GTCRN is reserved. Stores the output WAV path in PREVIOUS / storeAs.",
        {{"kind","audioFilter"}, {"input",""}, {"outputPath",""},
         {"filter","deepfilter"}, {"model",""}, {"attenLim",100.0},
         {"postFilter",false}, {"compensateDelay",true}, {"storeAs","audioPath"}})
        .params({
            pd_audio("input").in().lbl("Audio file").prev().dflt("")
                .desc("Source audio (wav, mp3, …). Default: empty → PREVIOUS chain value."),
            pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
                .desc("Destination WAV. Default: empty → <stem>-filtered.wav next to the source."),
            pd_enum("filter", {"deepfilter", "gtcrn"}).grp("filter").dflt("deepfilter")
                .desc("Enhancer. deepfilter = DeepFilterNet @ 48 kHz. gtcrn = reserved (16 kHz)."),
            pd_str("model").adv().grp("filter").dflt("")
                .desc("Optional DeepFilterNet tar.gz. Default: empty → ${MODELS_DIR}/deepfilter then exe/../models."),
            pd_float("attenLim").adv().grp("filter").dflt(100.0).range(0.0, 100.0)
                .desc("Attenuation limit in dB (in-process libDF only). Default: 100."),
            pd_bool("postFilter").adv().grp("filter").dflt(false)
                .desc("Over-attenuate very noisy sections. Default: false."),
            pd_bool("compensateDelay").adv().grp("filter").dflt(true)
                .desc("Compensate STFT / model lookahead. Default: true."),
            pd_store_as().dflt("audioPath")
                .desc("Variable for PREVIOUS / downstream blocks. Default: audioPath."),
        })
        .primary_output(ParamKind::audio_path, "Filtered audio")
        .exec({false, true, true});
#endif // PM_HAS_AUDIO_IO

#if defined(FEATURE_STT) && FEATURE_STT
    namespace C = pm::audio::constants;
    using J = nlohmann::json;

    registry["audioTranscribe"] = bdb(
        "audioTranscribe", audio_transcribe_block,
        "Speech to Text", "Audio",
        "Transcribe an audio file, or record mic / desktop / mix and transcribe speech to text. "
        "When 'input' is an existing audio file, it is transcribed directly (no capture, no duration limit). "
        "When empty or not a real file (including PREVIOUS text from a prior block), records from "
        "inputSource (mic, desktop loopback, or mix) for up to maxDurationMs ms with optional VAD silence-gate. "
        "Prefers local Whisper when selected/available, with Tanit as a provider-backed fallback. "
        "mode=transcribe (or hud=true) shows the recording HUD (levels + stop) and the overlay pill; "
        "ElevenLabs streams live, Whisper/Tanit transcribe after the take. "
        "Stores the transcript string in PREVIOUS / storeAs, or a structured STT payload when json is on.",
        {{"kind","audioTranscribe"}, {"input",""}, {"inputSource","mic"}, {"maxDurationMs",30000}, {"silenceMs",1500},
         {"device",""}, {"desktopDevice",""}, {"micGain",1.0}, {"desktopGain",1.0},
         {"provider",""}, {"model",""}, {"providerOptions", J::object()},
         {"stt", J::object()},
         {"mode","off"}, {"hud", false},
         {"json", false}, {"outputPath",""}, {"subtitleFormat","none"}, {"subtitlePath",""},
         {"subtitleMaxChars",42}, {"subtitleMaxLines",2},
         {"storeAs","transcript"}})
        .params({
            pd_audio("input").in().prev().dflt("")
                .desc("Input audio file (wav, mp3, …). Default: empty → PREVIOUS when it is an existing file, otherwise live capture."),
            pd_enum("mode", {"off", "transcribe"}).grp("hud").dflt("off")
                .desc("HUD preset. transcribe = level pill + overlay. off = no HUD unless hud is on."),
            pd_bool("hud").grp("hud").dflt(false)
                .desc("Show the recording HUD (levels + stop) and the overlay pill. Implied when mode=transcribe. Default off."),
            pd_enum("inputSource", {"mic", "desktop", "mix"}).grp("source").dflt("mic")
                .desc("Live capture source when input is not a file. mic = microphone; desktop = system loopback; mix = mic + desktop. Same as audioRecord / video audioSource."),
            pd_audio_input_device("device").adv().grp("source").lbl("Microphone").dflt("")
                .desc("Mic device for mic/mix. Default: empty → App Settings audio_input_device, then system default. Ignored when input is an existing file."),
            pd_str("desktopDevice").adv().grp("source").lbl("Desktop audio").dflt("")
                .desc("Loopback device substring for desktop/mix. Default: empty → default render device. Ignored when input is an existing file."),
            pd_float("micGain").adv().grp("source").dflt(1.0).range(0.0, 4.0)
                .desc("Mic level multiplier (linear) for mic/mix. Default: 1.0."),
            pd_float("desktopGain").adv().grp("source").dflt(1.0).range(0.0, 4.0)
                .desc("Desktop/loopback level multiplier for desktop/mix. Default: 1.0."),
            pd_ms("maxDurationMs").grp("recording").lbl("Max duration").dflt(30000).range(500, 120000)
                .desc("Max live capture duration (ms). Default: 30000. Ignored when input is an existing file."),
            pd_ms("silenceMs").grp("recording").lbl("Stop after silence").dflt(1500).range(0, 60000)
                .desc("VAD silence stop (ms) after speech. Default: 1500. 0 = disabled. Ignored when input is an existing file."),
            pd_stt_route("stt").grp("speech_model").lbl("STT").dflt(J::object())
                .desc("Dictation STT route {preset,provider,model,options}. Empty {} inherits App Settings. Flat provider/model remain aliases."),
            pd_provider().grp("speech_model").dflt("")
                .ui({{"options_path", "providers.stt"}})
                .desc("STT provider override (alias of stt.provider). Default: empty → App Settings stt_provider (required)."),
            pd_model().grp("speech_model").dflt("")
                .dyn({{"kind", "options_list"},
                      {"options_path_template", "providers.stt.models.{provider}"}})
                .desc("STT model override (alias of stt.model). Default: empty → App Settings stt_model when provider matches; "
                      "whisper → base.en (fallback; prefer large-v3-turbo when installed); "
                      "pixlwiz → pixlwiz-speech-to-text."),
            pd_api_key().dflt("")
                .desc("API key override. Default: empty → App Settings providers map (not used for whisper)."),
            pd_json("providerOptions").adv().grp("speech_model").lbl("Provider options").dflt(J::object())
                .dyn(whisper_provider_options_dynamic_schema())
                .desc("Whisper-only options object (alias of stt.options). Defaults: backend=gpu (falls back to cpu), language=auto, threads=0 (auto), "
                      "flashAttn=auto. Whisper models resolve under ${MODELS_DIR} then exe/../models."),
            pd_bool("json").adv().grp("output").dflt(false)
                .desc("Return structured STT JSON vs transcript string. Includes segments when available. Default: false."),
            pd_enum("subtitleFormat", {"none", "srt", "vtt", "sbv", "all"}).adv().grp("output").dflt("none")
                .desc("Write YouTube-accepted sidecar(s) next to the audio (or subtitlePath)."),
            pd_outpath("subtitlePath").adv().grp("output").lbl("Subtitle path").dflt("")
                .desc("Sidecar stem. Default: empty → next to the source/WAV."),
            pd_int("subtitleMaxChars").adv().grp("output").dflt(42).range(8, 40000)
                .desc("Cue wrap width."),
            pd_int("subtitleMaxLines").adv().grp("output").dflt(2).range(1, 4)
                .desc("Cue wrap line count."),
            pd_json("result").out()
                .desc("Structured STT payload when json=true: provider, model, transcript, segments, duration, samples, optional wav/subtitlePath."),
            pd_outpath("outputPath").adv().grp("debug_output").lbl("Keep WAV").dflt("")
                .desc("Save captured WAV. Default: empty → temp file deleted after transcription."),
            pd_store_as().dflt("transcript")
                .desc("Variable for PREVIOUS / downstream blocks. Default: transcript."),
        })
        .dynamic_primary_output(
            ParamKind::string_,
            {ParamKind::string_, ParamKind::json_value},
            "Transcript")
        .exec({false, true, true});

    registry["audioSpeak"] = bdb(
        "audioSpeak", audio_speak_block,
        "Text to Speech", "Audio",
        "Convert text to speech via the configured TTS provider and play it through the speakers. "
        "Supports Tanit, ElevenLabs, local VibeVoice, and MOSS-TTS-Nano builds. Uses PREVIOUS string as text when text is empty. "
        "Markdown/plain-text pre-parsers run on file paths and on inline/PREVIOUS content (e.g. fsRead → audioSpeak). "
        "hud=true shows the overlay pill while synthesizing and playing. "
        "Stores output file path in PREVIOUS / storeAs when outputPath is set, otherwise stores the input text.",
        {{"kind","audioSpeak"}, {"text",""}, {"provider",""}, {"model",""}, {"voice",""},
         {"tokenizer",""}, {"codec",""}, {"refAudio",""}, {"instance",""}, {"outputDevice",""}, {"outputPath",""},
         {"hud", false},
         {"sendVoiceSettings", true}, {"chunkSentences", true},
         {"chunkTargetChars", static_cast<int>(C::k_elevenlabs_chunk_target_chars)},
         {"chunkMaxChars", static_cast<int>(C::k_elevenlabs_chunk_max_chars)},
         {"stability", C::k_elevenlabs_stability},
         {"similarityBoost", C::k_elevenlabs_similarity_boost},
         {"style", C::k_elevenlabs_style},
         {"storeAs",""}})
        .params({
            pd_prompt("text").in().lbl("Text").prev().dflt("")
                .desc("Text to speak, or path to a text file (.txt, .md, …). "
                      "Files are loaded (binary rejected); inline/PREVIOUS markdown is preparsed when detected. "
                      "Chain fsRead → audioSpeak with empty text to speak file contents. "
                      "Default: empty → PREVIOUS string (required at runtime)."),
            pd_provider().grp("voice_model").dflt("")
                .ui({{"options_path", "providers.tts"}})
                .desc("TTS provider override. Default: empty → App Settings tts_provider (required)."),
            pd_model().grp("voice_model").dflt("")
                .dyn({{"kind", "options_list"},
                      {"options_path_template", "providers.tts.models.{provider}"}})
                .desc("TTS model override. Default: empty → App Settings tts_model; "
                      "pixlwiz → pixlwiz-speech when unset. moss → moss-tts-nano GGUF bundle."),
            pd_str("voice").adv().grp("voice_model").dflt("")
                .dyn({{"kind", "options_list"},
                      {"options_path_template", "providers.tts.voices.{model}"}})
                .desc("Voice id. Default: empty → App Settings tts_voice_id. "
                      "OpenRouter voices come from the selected model."),
            pd_str("tokenizer").adv().grp("local_tts").lbl("Tokenizer").dflt("")
                .ui({{"visible_when", {{"provider", nlohmann::json{"vibevoice", "moss"}}}}})
                .desc("Tokenizer GGUF path. Required for VibeVoice; MOSS defaults to "
                      "models/moss-tts-nano/moss-nano-tokenizer.gguf."),
            pd_str("codec").adv().grp("local_tts").lbl("Codec").dflt("")
                .ui({{"visible_when", {{"provider", "moss"}}}})
                .desc("MOSS Audio Tokenizer Nano GGUF. Default: empty → "
                      "models/moss-tts-nano/moss-audio-tokenizer-nano.gguf."),
            pd_str("refAudio").adv().grp("local_tts").lbl("Reference audio").dflt("")
                .ui({{"visible_when", {{"provider", nlohmann::json{"vibevoice", "moss"}}}}})
                .desc("Reference WAV for VibeVoice 1.5B or MOSS Nano cloning. Default: empty."),
            pd_str("instance").adv().grp("local_tts").dflt("")
                .ui({{"visible_when", {{"provider", nlohmann::json{"vibevoice", "moss"}}}}})
                .desc("Named cached local TTS engine instance. Default: empty → auto-key by model args."),
            pd_audio_output_device("outputDevice").adv().grp("device").lbl("Speaker").dflt("")
                .desc("Playback device. Default: empty → App Settings audio_output_device, then system default."),
            pd_outpath("outputPath").adv().grp("output_file").lbl("Save audio").dflt("")
                .desc("Optional synthesized audio path (.wav). Default: empty → play only, no file saved."),
            pd_bool("hud").grp("hud").dflt(false)
                .desc("Show the overlay HUD while synthesizing and playing. Default off."),
            pd_bool("sendVoiceSettings").adv().grp("elevenlabs").lbl("Voice settings").dflt(true)
                .desc("ElevenLabs direct only: send voice_settings (stability/similarity/style). Default: true."),
            pd_bool("chunkSentences").adv().grp("elevenlabs").lbl("Chunk sentences").dflt(true)
                .desc("ElevenLabs direct only: split long text with request stitching. Default: true."),
            pd_int("chunkTargetChars").adv().grp("elevenlabs").lbl("Chunk target").dflt(static_cast<int>(C::k_elevenlabs_chunk_target_chars)).range(100, 800)
                .desc("ElevenLabs direct only: greedy-merge sentences up to this many chars. Default: 400."),
            pd_int("chunkMaxChars").adv().grp("elevenlabs").lbl("Chunk max").dflt(static_cast<int>(C::k_elevenlabs_chunk_max_chars)).range(200, 2000)
                .desc("ElevenLabs direct only: hard-split a single long sentence. Default: 800."),
            pd_float("stability").adv().grp("elevenlabs").dflt(C::k_elevenlabs_stability).range(0.0, 1.0)
                .desc("ElevenLabs direct only: stability (0–1). Default: 0.9."),
            pd_float("similarityBoost").adv().grp("elevenlabs").lbl("Similarity boost").dflt(C::k_elevenlabs_similarity_boost).range(0.0, 1.0)
                .desc("ElevenLabs direct only: similarity_boost (0–1). Default: 0.75."),
            pd_float("style").adv().grp("elevenlabs").dflt(C::k_elevenlabs_style).range(0.0, 1.0)
                .desc("ElevenLabs direct only: style (0–1). Default: 0 (non-zero can add drift/noise)."),
            pd_store_as().dflt("")
                .desc("Optional variable for PREVIOUS. Default: empty (PREVIOUS only)."),
        })
        .dynamic_primary_output(
            ParamKind::string_,
            {ParamKind::string_, ParamKind::audio_path},
            "Speech result")
        .exec({false, true, true});
#else
    (void)registry;
#endif
}

} // namespace media::xblox::blocks
