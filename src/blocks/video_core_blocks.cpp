// XBlox video blocks - shared helpers, videoListDevices, videoCapture, videoSource.
//
// Gated on FEATURE_VIDEO. Builds without it compile an empty TU.

#include "blocks/video_blocks.hpp"

#include "constants.hpp"
#include "logger/logger.h"
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "core/video.hpp"
#include "core/settings_runtime.hpp"
#include "core/frame_source.hpp"
#include "core/frame_pump.hpp"
#include "core/video_encoder.hpp"
#include "blocks/frame_store.hpp"
#if defined(_WIN32)
#include "core/screen_input.hpp"
#include "video/key_overlay.hpp"
#include "video/creator_fx.hpp"
#include "win/display_affinity.hpp"
#include <objbase.h>
#endif
#if PM_HAS_AUDIO_IO
#include "core/audio.hpp"
#include "core/mixed_audio_capture.hpp"
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
#  include "audio/filter/audio_filter.hpp"
#endif
#endif
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
#include "video/filters/beauty_processor.hpp"
#endif
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
#include "video/vcam/vcam_sink.hpp"
#endif
#if defined(_WIN32)
#include "video/frame_preview.hpp"
#endif
#include "win/overlay_hud.hpp"
#include "win/taskbar_status.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "string/string_utils.hpp"

#endif // FEATURE_VIDEO

namespace media::xblox::blocks {

// ── Shared helpers (available for all video feature gates) ────────────────────

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

// ── Device resolution ─────────────────────────────────────────────────────────
// Returns the block `device` param when non-empty; otherwise the preferred
// camera from settings if it is still plugged in; otherwise the OS default.
std::string resolve_video_capture(const std::string& block_device)
{
    if (!block_device.empty()) return block_device;
    media::runtime_settings::AudioVideoDeviceSettings avs;
    std::string err;
    media::runtime_settings::load_audio_video_device_settings(avs, err);
    if (avs.video_capture_device.empty()) return {};
    std::string want = avs.video_capture_device;
    want = media::strutil::lower_ascii(std::move(want));
    for (const auto& d : pm::video::enumerate_capture_devices()) {
        std::string have = d.name;
        have = media::strutil::lower_ascii(std::move(have));
        if (have.find(want) != std::string::npos)
            return avs.video_capture_device;
    }
    logger::warn("[xblox videoCapture] preferred camera '" +
                 avs.video_capture_device +
                 "' is not connected; using the default device");
    return {};
}

#if PM_HAS_AUDIO_IO
static std::string resolve_audio_input(const std::string& block_device)
{
    if (!block_device.empty()) return block_device;
    media::runtime_settings::AudioVideoDeviceSettings avs;
    std::string err;
    media::runtime_settings::load_audio_video_device_settings(avs, err);
    return avs.audio_input_device;
}

static pm::audio::MixedCaptureConfig video_audio_config_from_io(BlockIO& io)
{
    // Match block docs / AAC mux: 48 kHz stereo for mic, desktop, mix, camera.
    auto cfg = pm::audio::make_recording_capture_config(
        io.str("audioSource"),
        resolve_audio_input(io.str("audioDevice")),
        io.str("desktopDevice"),
        static_cast<float>(io.f64("micGain", 1.0)),
        static_cast<float>(io.f64("desktopGain", 1.0)),
        pm::audio::CaptureSource::none);
    if (io.flag("cameraAudio", false)) {
        pm::audio::apply_camera_audio(
            cfg,
            true,
            io.str("cameraInput", ""),
            static_cast<float>(std::clamp(io.f64("cameraAudioGain", 1.0), 0.0, 4.0)));
    }
    return cfg;
}

static void fill_video_recorder_audio(const pm::audio::MixedCaptureConfig& cfg,
                                      media::xblox::VideoRecorderRequest& req)
{
    req.audio_source    = pm::audio::capture_source_name(cfg.source);
    req.audio_device    = cfg.mic_device;
    req.desktop_device  = cfg.desktop_device;
    req.mic_gain        = cfg.mic_gain;
    req.desktop_gain    = cfg.desktop_gain;
}
#endif

using media::strutil::lower_ascii;

static bool has_ext_ci(const std::string& path, const char* ext)
{
    if (path.empty() || !ext) return false;
    return lower_ascii(media::path::path_to_utf8(media::path::path_from_utf8(path).extension())) == ext;
}

static std::string loop_suffix(BlockIO& io)
{
    if (const nlohmann::json* li = io.context("loop.index")) {
        if (li->is_number()) return "_" + std::to_string(li->get<int>());
    }
    return {};
}

struct CaptureHud {
    std::unique_ptr<media::win::TaskbarJob> taskbar;
    std::unique_ptr<media::win::OverlayJob> overlay;
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);

    explicit operator bool() const noexcept { return taskbar || overlay; }

    bool stop_requested() const noexcept
    {
        return (stop && stop->load(std::memory_order_relaxed)) ||
               (overlay && overlay->stop_requested());
    }

    void busy(const std::string& detail)
    {
        if (taskbar) taskbar->busy(detail);
        if (overlay) overlay->busy(detail);
    }

    void progress(uint64_t completed, uint64_t total, const std::string& detail)
    {
        if (taskbar) taskbar->progress(completed, total, detail);
        if (overlay) overlay->progress(completed, total, detail);
    }

    void hide()
    {
        if (overlay) overlay->hide();
        if (taskbar) taskbar->dismiss();
    }

    void set_on_stop(std::function<void()> fn, bool cancel_sink)
    {
        auto flag = stop;
        auto wrapped = [flag, user = std::move(fn)] {
            if (flag)
                flag->store(true, std::memory_order_relaxed);
            if (user)
                user();
        };
        if (overlay) overlay->set_on_stop(wrapped, cancel_sink);
        if (taskbar) taskbar->set_on_stop(std::move(wrapped));
    }
};

static CaptureHud begin_capture_hud(BlockIO& io)
{
    CaptureHud hud;
    const std::string mode = lower_ascii(io.str("hudMode"));
    const bool want = io.flag("hud", false) && mode != "off";
    if (!want)
        return hud;
    const bool use_taskbar = mode == "taskbar" || mode == "both";
    const bool use_overlay = mode.empty() || mode == "overlay" || mode == "both";
    if (use_taskbar)
        hud.taskbar = std::make_unique<media::win::TaskbarJob>("Capture");
    if (use_overlay)
        hud.overlay = std::make_unique<media::win::OverlayJob>("Capture");
    return hud;
}

static std::string temp_capture_path(BlockIO& io, const char* stem, const char* ext)
{
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec);
    const std::string tmp_dir = ec ? std::string(".") : media::path::path_to_utf8(tmp);
    return tmp_dir + "/" + stem + loop_suffix(io) + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ext;
}

struct VideoCaptureRecordSummary {
    std::string path;
    std::string input;
    std::string source;
    long long   frames = 0;
    int         width = 0;
    int         height = 0;
    int         fps = 30;
    bool        fast = false;
};

// Built-in recorder knobs (same CPU path as `video record --mirror --blur --virtual-out`).
struct VideoCaptureFx {
    bool write_file = true;
    bool virtual_out = false;
    bool preview = false;
    bool camera_overlay = false;
    std::string virtual_out_name = "Tanit Cam";
#if PM_HAS_AUDIO_IO
    pm::video::AudioPolishConfig audio_polish{};
#endif
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    bool use_beauty = false;
    pm::video::BeautyConfig beauty;
#endif

    bool needs_cpu_pump() const noexcept {
        if (virtual_out || preview) return true;
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        // PiP owns the face knobs; screen stays on fast WGC.
        if (use_beauty && !camera_overlay) return true;
#endif
        return false;
    }

    std::string describe() const {
        std::string s;
        if (preview) s += " preview";
        if (virtual_out) s += " virtualOut";
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        if (beauty.mirror) s += " mirror";
        if (beauty.skin) s += " skin";
        if (beauty.lips) s += " lips";
        if (beauty.teeth) s += " teeth";
        if (beauty.blur) s += " blur";
        if (beauty.background) s += " background";
        if (beauty.show_landmarks) s += " landmarks";
        if (beauty.undereye) s += " undereye";
        if (beauty.redness) s += " redness";
        if (beauty.eye_sharpen) s += " eyeSharpen";
        if (beauty.face_gain > 0.f) s += " faceGain";
        if (beauty.warmth > 0.f) s += " warmth";
#endif
        return s.empty() ? std::string(" (passthrough)") : s;
    }
};

#if !defined(FEATURE_VIDEO_BEAUTY) || !FEATURE_VIDEO_BEAUTY
static bool beauty_flags_on(BlockIO& io)
{
    return io.flag("skin", false) || io.flag("lips", false) ||
           io.flag("teeth", false) || io.flag("landmarks", false) ||
           io.flag("mirror", false) || io.flag("blur", false) ||
           io.flag("background", false) || io.flag("undereye", false) ||
           io.flag("redness", false) || io.flag("eyeSharpen", false) ||
           io.f64("faceGain", 0.0) > 0.0 || io.f64("warmth", 0.0) > 0.0;
}
#endif

#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
static pm::video::BeautyConfig beauty_config_from_io(BlockIO& io)
{
    pm::video::BeautyConfig beauty;
    beauty.skin = io.flag("skin", false);
    beauty.skin_strength = static_cast<float>(
        std::clamp(io.f64("skinBlend", 0.5), 0.0, 1.0));
    beauty.lips = io.flag("lips", false);
    beauty.lips_color = io.str("lipsColor", "#C45C6A");
    beauty.lips_alpha = static_cast<float>(
        std::clamp(io.f64("lipsBlend", 0.35), 0.0, 1.0));
    beauty.teeth = io.flag("teeth", false);
    beauty.teeth_strength = static_cast<float>(
        std::clamp(io.f64("teethBlend", 0.4), 0.0, 1.0));
    beauty.show_landmarks = io.flag("landmarks", false);
    beauty.mirror = io.flag("mirror", false);
    beauty.blur = io.flag("blur", false);
    beauty.background = io.flag("background", false);
    beauty.blur_strength = static_cast<float>(
        std::clamp(io.f64("blurBlend", 0.65), 0.0, 1.0));
    beauty.feather_px = std::clamp(static_cast<int>(io.i64("featherPx", 8)), 0, 64);
    beauty.bg_color = io.str("bgColor", "#1A1A1A");
    beauty.bg_image = io.str("bgImage", "");
    beauty.face_gain = static_cast<float>(
        std::clamp(io.f64("faceGain", 0.0), 0.0, 1.0));
    beauty.warmth = static_cast<float>(
        std::clamp(io.f64("warmth", 0.0), 0.0, 1.0));
    beauty.undereye = io.flag("undereye", false);
    beauty.undereye_blend = static_cast<float>(
        std::clamp(io.f64("undereyeBlend", 0.45), 0.0, 1.0));
    beauty.redness = io.flag("redness", false);
    beauty.redness_blend = static_cast<float>(
        std::clamp(io.f64("rednessBlend", 0.4), 0.0, 1.0));
    beauty.eye_sharpen = io.flag("eyeSharpen", false);
    beauty.eye_sharpen_blend = static_cast<float>(
        std::clamp(io.f64("eyeSharpenBlend", 0.35), 0.0, 1.0));
    beauty.face = beauty.skin || beauty.lips || beauty.teeth ||
                  beauty.show_landmarks || beauty.undereye || beauty.redness ||
                  beauty.eye_sharpen || beauty.face_gain > 0.f ||
                  beauty.warmth > 0.f;
    beauty.running_mode = "video";
    return beauty;
}
#endif

static VideoCaptureFx capture_fx_from_io(BlockIO& io)
{
    VideoCaptureFx fx;
    fx.virtual_out = io.flag("virtualOut", false);
    fx.preview = io.flag("preview", false);
    fx.camera_overlay = io.flag("cameraOverlay", false);
    fx.virtual_out_name = io.str("virtualOutName", "Tanit Cam");
    if (fx.virtual_out_name.empty())
        fx.virtual_out_name = "Tanit Cam";
#if PM_HAS_AUDIO_IO
    fx.audio_polish.filter = io.str("filter");
    fx.audio_polish.filter_model = io.str("filterModel");
    fx.audio_polish.atten_lim_db = static_cast<float>(io.f64("attenLim", 100.0));
    fx.audio_polish.post_filter = io.flag("postFilter", false);
    fx.audio_polish.compensate_delay = io.flag("compensateDelay", true);
#endif
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    fx.beauty = beauty_config_from_io(io);
    const bool pip_owns = fx.camera_overlay &&
                          pm::video::is_screen_input_spec(io.str("input"));
    fx.use_beauty = fx.beauty.wants_beauty() && !pip_owns;
#endif
    return fx;
}

static bool prepare_cpu_capture(BlockIO& io,
                               const std::string& kind_id,
                               const VideoCaptureFx& fx,
                               std::string& capture_engine,
                               const std::string& encoder)
{
    if (!fx.needs_cpu_pump())
        return true;
    if (capture_engine == "fast" || encoder == "nvenc") {
        const std::string warning =
            "videoCapture: requested fast/NVENC cannot apply skin / lips / "
            "teeth / landmarks / faceGain / warmth / undereye / redness / "
            "eyeSharpen / mirror / blur / background / preview / virtualOut; "
            "falling back to the compatibility CPU recorder";
        logger::warn("[xblox videoCapture] " + warning);
        io.warn(kind_id, warning);
    }
    capture_engine = "compat";
    return true;
}

static bool encode_mp4_from_frame_pump(const std::string& input,
                                       const std::string& output_path,
                                       int width,
                                       int height,
                                       int fps,
                                       bool include_cursor,
                                       long long duration_ms,
                                       int timeout_ms,
                                       const std::function<bool()>& cancel,
                                       const VideoCaptureFx& fx,
                                       VideoCaptureRecordSummary& summary,
                                       std::string& err
#if PM_HAS_AUDIO_IO
                                       , const pm::audio::MixedCaptureConfig* audio = nullptr
#endif
                                       )
{
#if defined(_WIN32)
    pm::video::ensure_com_initialized();
#endif
    const int rec_fps = std::clamp(fps > 0 ? fps : 30, 1, 240);

    pm::video::FramePumpOptions opt;
    opt.width           = width;
    opt.height          = height;
    opt.fps             = rec_fps;
    opt.include_cursor  = include_cursor;
    opt.duration_ms     = duration_ms;
    opt.open_timeout_ms = std::max(1000, timeout_ms);
    opt.cancel          = cancel;

    std::string open_err;
    auto pump = pm::video::make_frame_pump(input, opt, open_err);
    if (!pump) {
        err = open_err.empty() ? "could not open video input" : open_err;
        return false;
    }

#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    pm::video::BeautyProcessor beauty;
    std::int64_t beauty_prev_ts = -1;
    if (fx.use_beauty) {
        if (!pm::video::probe_beauty_runtime(fx.beauty, err)) {
            if (err.empty())
                err = "filter runtime not available";
            return false;
        }
        if (!beauty.create(fx.beauty, err)) {
            err = "beauty create failed: " + err;
            return false;
        }
    }
#endif
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
    pm::vcam::VcamSink vcam;
    if (fx.virtual_out) {
        pm::vcam::VcamOptions vopt;
        vopt.name = fx.virtual_out_name;
        if (!vcam.start(vopt, err))
            return false;
    }
#endif
#if defined(_WIN32)
    pm::video::FramePreview preview;
    bool preview_open = false;
#endif

    pm::video::VideoEncoder enc;
    bool opened = false;
    long long frames = 0;
    int actual_w = 0;
    int actual_h = 0;
    const bool write_file = fx.write_file && !output_path.empty();
    const auto t_start = std::chrono::steady_clock::now();
    auto t_mark = t_start;
    long long frames_at_mark = 0;
    long long beauty_ms_last = 0;
    long long beauty_ms_sum = 0;
    bool logged_slow = false;

    logger::warn("[xblox videoCapture] cpu-record start input='" +
                 (input.empty() ? std::string("default") : input) +
                 "' dst='" + output_path + "' " +
                 std::to_string(width) + "x" + std::to_string(height) + "@" +
                 std::to_string(rec_fps) + "fps" + fx.describe());

    pm::video::VideoFrame frame;
    std::string perr;
    while (pump->next(frame, perr)) {
        bool need_bgr = (frame.format != pm::video::PixelFormat::BGR24 &&
                         frame.format != pm::video::PixelFormat::RGB24);
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        if (fx.use_beauty)
            need_bgr = !pm::video::is_packed_bgr24(frame);
#endif
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
        if (vcam.is_open())
            need_bgr = !pm::video::is_packed_bgr24(frame);
#endif
#if defined(_WIN32)
        if (fx.preview)
            need_bgr = !pm::video::is_packed_bgr24(frame);
#endif
        if (need_bgr) {
            pm::video::VideoFrame converted;
            if (!pm::video::to_bgr24(frame, converted)) {
                err = "frame format is not encodable";
                break;
            }
            frame = std::move(converted);
        }
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        if (fx.use_beauty) {
            std::int64_t ts = frame.timestamp_ms;
            if (ts <= beauty_prev_ts)
                ts = beauty_prev_ts + 1;
            beauty_prev_ts = ts;
            std::string berr;
            const auto t_beauty = std::chrono::steady_clock::now();
            if (!beauty.process(frame, ts, berr)) {
                err = "beauty failed: " + berr;
                break;
            }
            beauty_ms_last = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_beauty).count();
            beauty_ms_sum += beauty_ms_last;
        }
#endif

        if (write_file && !opened) {
            std::string enc_err;
#if defined(_WIN32) && PM_HAS_AUDIO_IO
            pm::video::VideoAudioOptions audio_opt;
            const pm::video::VideoAudioOptions* audio_ptr = nullptr;
            if (audio && pm::audio::capture_source_active(*audio)) {
                audio_opt.capture = *audio;
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
                if (fx.audio_polish.filter_on()) {
                    audio_opt.filter_enabled = true;
                    audio_opt.filter = pm::audio::filter::make_options(
                        fx.audio_polish.filter,
                        fx.audio_polish.filter_model,
                        fx.audio_polish.atten_lim_db,
                        fx.audio_polish.post_filter,
                        fx.audio_polish.compensate_delay);
                }
#endif
                audio_ptr = &audio_opt;
            }
            if (!enc.open(output_path, frame.width, frame.height, rec_fps, enc_err, audio_ptr)) {
#else
            if (!enc.open(output_path, frame.width, frame.height, rec_fps, enc_err)) {
#endif
                err = "encoder open failed: " + enc_err;
                break;
            }
            actual_w = frame.width;
            actual_h = frame.height;
            opened = true;
        } else if (!write_file && actual_w == 0) {
            actual_w = frame.width;
            actual_h = frame.height;
        }

        if (write_file) {
            std::string enc_err;
            if (!enc.write(frame, enc_err)) {
                err = "encoder write failed: " + enc_err;
                break;
            }
        }
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
        if (vcam.is_open()) {
            std::string verr;
            if (!vcam.push(frame, verr)) {
                err = "virtual cam push failed: " + verr;
                break;
            }
        }
#endif
#if defined(_WIN32)
        if (fx.preview) {
            if (!preview_open) {
                if (!preview.show(frame.width, frame.height, "videoCapture")) {
                    err = "could not open preview window";
                    break;
                }
                preview_open = true;
            }
            const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
            preview.update(frame.data.data(), frame.width, frame.height, stride);
            if (!preview.is_open())
                break;
        }
#endif
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - t_mark >= std::chrono::seconds(1)) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - t_start).count();
            const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - t_mark).count();
            const double inst_fps = dt_ms > 0
                ? (static_cast<double>(frames - frames_at_mark) * 1000.0 /
                   static_cast<double>(dt_ms))
                : 0.0;
            const long long beauty_avg = frames > 0 ? beauty_ms_sum / frames : 0;
            logger::warn("[xblox videoCapture] " +
                         std::to_string(elapsed_ms / 1000.0) + "s frames=" +
                         std::to_string(frames) + " inst_fps=" +
                         std::to_string(inst_fps) + " target=" +
                         std::to_string(rec_fps) + " beauty_ms=" +
                         std::to_string(beauty_ms_last) + " avg=" +
                         std::to_string(beauty_avg) +
                         (fx.preview ? " preview=1" : " preview=0"));
            if (!logged_slow && inst_fps < static_cast<double>(rec_fps) * 0.5 &&
                frames >= rec_fps) {
                logged_slow = true;
                logger::warn("[xblox videoCapture] capture is filter-bound"
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
                             " (skin/blur/segmenter), not the preview window"
#endif
                );
            }
            t_mark = now;
            frames_at_mark = frames;
        }
        if (cancel && cancel()) {
            logger::warn("[xblox videoCapture] cancel after frames=" +
                         std::to_string(frames));
            break;
        }
    }

    if (!perr.empty() && err.empty()) err = perr;

    logger::warn("[xblox videoCapture] closing encoder frames=" +
                 std::to_string(frames) + " opened=" + (opened ? "1" : "0"));
    std::string close_err;
    if (opened && !enc.close(close_err) && err.empty())
        err = "encoder close failed: " + close_err;

    if (!err.empty()) return false;
    if (frames == 0 || (write_file && !opened)) {
        err = "no frames captured";
        return false;
    }

    summary.path   = output_path;
    summary.input  = input.empty() ? std::string("default") : input;
    summary.source = pump->describe();
    summary.frames = frames;
    summary.width  = actual_w;
    summary.height = actual_h;
    summary.fps    = rec_fps;
    return true;
}

static std::string default_record_instance(const std::string& instance, const std::string& input)
{
    if (!instance.empty()) return instance;
    return input.empty() ? std::string("default") : input;
}

class VideoCaptureRecorder {
public:
    ~VideoCaptureRecorder() { stop(); }

    bool start_generic(std::string input,
                       std::string output_path,
                       int width,
                       int height,
                       int fps,
                       bool include_cursor,
                       long long duration_ms,
                       int timeout_ms,
                       VideoCaptureFx fx,
#if PM_HAS_AUDIO_IO
                       pm::audio::MixedCaptureConfig audio_cfg,
#endif
                       std::string& err)
    {
        if (running_.load()) {
            err = "recording is already running";
            return false;
        }
        input_ = std::move(input);
        output_path_ = std::move(output_path);
        include_cursor_ = include_cursor;
        fast_ = false;
        stop_.store(false);
        running_.store(true);
        done_.store(false);
        error_.clear();
        summary_ = {};
        thread_ = std::thread([this, width, height, fps, duration_ms, timeout_ms, fx
#if PM_HAS_AUDIO_IO
                               , audio_cfg
#endif
        ] {
            VideoCaptureRecordSummary summary;
            std::string run_err;
            const bool ok = encode_mp4_from_frame_pump(
                input_, output_path_, width, height, fps, include_cursor_, duration_ms, timeout_ms,
                [this] { return stop_.load(); }, fx, summary, run_err
#if PM_HAS_AUDIO_IO
                , pm::audio::capture_source_active(audio_cfg) ? &audio_cfg : nullptr
#endif
            );
            {
                std::lock_guard<std::mutex> lk(mu_);
                summary_ = std::move(summary);
                error_ = ok ? std::string{} : run_err;
            }
            running_.store(false);
            done_.store(true);
        });
        return true;
    }

    bool start_fast(std::string input, std::string output_path)
    {
        input_ = std::move(input);
        output_path_ = std::move(output_path);
        fast_ = true;
        running_.store(true);
        done_.store(false);
        stop_.store(false);
        return true;
    }

    void request_stop() { stop_.store(true); }

    void set_host_stop(media::xblox::VideoRecorderHandler handler,
                       std::string instance,
                       std::string input)
    {
        if (!handler)
            return;
        host_stop_ = [h = std::move(handler),
                      instance = std::move(instance),
                      input = std::move(input)] {
            media::xblox::VideoRecorderRequest req;
            req.action = "stop";
            req.instance = instance;
            req.input = input;
            media::xblox::VideoRecorderResponse response;
            (void)h(req, &response);
        };
    }

    void take_hud(CaptureHud hud)
    {
        hud_ = std::move(hud);
        hud_.set_on_stop([this] {
            request_stop();
            if (host_stop_ && !host_stop_fired_.exchange(true)) {
                std::thread(host_stop_).detach();
            }
            hud_.hide();
        }, false);
    }

#if defined(_WIN32)
    void include_picker_hud(bool on)
    {
        picker_hud_ = std::make_unique<media::win::PickerHudCaptureScope>(on);
    }
#endif

    void hud_busy(const std::string& detail)
    {
        if (hud_)
            hud_.busy(detail);
    }

    void stop()
    {
        request_stop();
        hud_.hide();
        if (thread_.joinable()) thread_.join();
        running_.store(false);
        done_.store(true);
    }

    bool fast() const noexcept { return fast_; }
    bool running() const noexcept { return running_.load(); }
    std::string output_path() const { return output_path_; }
    VideoCaptureRecordSummary summary() const { std::lock_guard<std::mutex> lk(mu_); return summary_; }
    std::string error() const { std::lock_guard<std::mutex> lk(mu_); return error_; }

private:
    std::thread               thread_;
    std::atomic<bool>         stop_{false};
    std::atomic<bool>         running_{false};
    std::atomic<bool>         done_{false};
    std::atomic<bool>         host_stop_fired_{false};
    bool                      fast_ = false;
    mutable std::mutex        mu_;
    VideoCaptureRecordSummary summary_;
    std::string               error_;
    std::string               input_;
    std::string               output_path_;
    bool                      include_cursor_ = true;
    CaptureHud                hud_;
    std::function<void()>     host_stop_;
#if defined(_WIN32)
    std::unique_ptr<media::win::PickerHudCaptureScope> picker_hud_;
#endif
};

static std::mutex& capture_recorders_mutex()
{
    static std::mutex mu;
    return mu;
}

static std::unordered_map<std::string, std::shared_ptr<VideoCaptureRecorder>>& capture_recorders()
{
    static std::unordered_map<std::string, std::shared_ptr<VideoCaptureRecorder>> recorders;
    return recorders;
}

static int even_pixels(int value) noexcept
{
    return std::max(2, value & ~1);
}

/// Parse aspectRatio + optional custom W/H into width÷height. Returns 0 for
/// source / unset / invalid.
static double resolve_aspect_ratio(BlockIO& io)
{
    const std::string name = io.str("aspectRatio", "source");
    if (name.empty() || name == "source")
        return 0.0;
    if (name == "custom") {
        const int cw = std::max(1, static_cast<int>(io.i64("aspectCustomW", 16)));
        const int ch = std::max(1, static_cast<int>(io.i64("aspectCustomH", 9)));
        return static_cast<double>(cw) / static_cast<double>(ch);
    }
    const auto colon = name.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= name.size())
        return 0.0;
    try {
        const int aw = std::stoi(name.substr(0, colon));
        const int ah = std::stoi(name.substr(colon + 1));
        if (aw < 1 || ah < 1) return 0.0;
        return static_cast<double>(aw) / static_cast<double>(ah);
    } catch (...) {
        return 0.0;
    }
}

/// Derive a fixed encode canvas for a target aspect. Width is the primary
/// size when set (height is computed from it); height is only used when width
/// is 0. When both are 0, uses a 1920 long-edge default. Returns false when
/// aspect is unset (caller keeps w/h).
static bool apply_aspect_to_canvas(double aspect, int& width, int& height)
{
    if (aspect <= 0.0) return false;
    constexpr int kDefaultLongEdge = 1920;
    if (width > 0) {
        height = even_pixels(static_cast<int>(std::lround(
            static_cast<double>(width) / aspect)));
    } else if (height > 0) {
        width = even_pixels(static_cast<int>(std::lround(
            static_cast<double>(height) * aspect)));
    } else if (aspect >= 1.0) {
        width = even_pixels(kDefaultLongEdge);
        height = even_pixels(static_cast<int>(std::lround(
            static_cast<double>(width) / aspect)));
    } else {
        height = even_pixels(kDefaultLongEdge);
        width = even_pixels(static_cast<int>(std::lround(
            static_cast<double>(height) * aspect)));
    }
    width = even_pixels(width);
    height = even_pixels(height);
    return true;
}

static void fill_video_transform_request(
    BlockIO& io, media::xblox::VideoRecorderRequest& request)
{
    request.size_mode = lower_ascii(io.str("sizeMode", "native"));
    request.aspect_ratio = resolve_aspect_ratio(io);
    request.zoom = std::clamp(io.f64("zoom", 1.0), 1.0, 32.0);
    request.follow = lower_ascii(io.str("follow", "none"));
    request.follow_speed =
        std::clamp(io.f64("followSpeed", 0.2), 0.01, 1.0);
    request.follow_deadzone = std::clamp(
        static_cast<int>(io.i64("followDeadzone", 3)), 0, 1000);
    request.transition_ms = std::clamp(
        static_cast<int>(io.i64("transitionMs", 300)), 0, 10000);
    request.update_interval_ms = std::clamp(
        static_cast<int>(io.i64("updateIntervalMs", 16)), 1, 1000);
    request.focus_x =
        std::clamp(io.f64("focusX", -1.0), -1.0, 1.0);
    request.focus_y =
        std::clamp(io.f64("focusY", -1.0), -1.0, 1.0);
    request.interactive_zoom = io.flag("interactiveZoom", false);
    request.zoom_step =
        std::clamp(io.f64("zoomStep", 0.15), 0.01, 2.0);
    request.zoom_min =
        std::clamp(io.f64("zoomMin", 1.0), 1.0, 32.0);
    request.zoom_max = std::clamp(
        io.f64("zoomMax", 4.0), request.zoom_min, 32.0);
    request.consume_zoom_input = io.flag("consumeZoomInput", true);
    request.show_zoom_status = io.flag("showZoomStatus", true);
    request.hud_capture = io.str("hudCapture", "auto");
    {
        const std::string mode = lower_ascii(io.str("hudMode", "overlay"));
        request.status_hud = (io.flag("hud", false) && mode != "off") ? mode : "off";
    }
    request.pause_key = io.str("pauseKey", "");
    request.stop_key = io.str("stopKey", "");
    request.key_overlay = io.flag("keyOverlay", false);
    request.key_overlay_burn_in = io.flag("keyOverlayBurnIn", false);
    request.key_overlay_align = io.str("keyOverlayAlign", "bottom");
    request.key_overlay_margin_x = static_cast<int>(io.i64("keyOverlayMarginX", 0));
    request.key_overlay_margin_y = static_cast<int>(io.i64("keyOverlayMarginY", 0));
    request.key_overlay_font_size = static_cast<int>(io.i64("keyOverlayFontSize", 0));
    request.key_overlay_color = io.str("keyOverlayColor", "#F8F9FB");
    request.key_overlay_bg_color = io.str("keyOverlayBgColor", "#181B21");
    request.key_overlay_accent_color = io.str("keyOverlayAccentColor", "#EB445A");
    request.key_overlay_opacity =
        static_cast<float>(std::clamp(io.f64("keyOverlayOpacity", 0.9), 0.0, 1.0));
    request.key_overlay_hold_ms = static_cast<int>(io.i64("keyOverlayHoldMs", 1200));
    request.key_overlay_fade_ms = static_cast<int>(io.i64("keyOverlayFadeMs", 450));
    request.key_overlay_max_entries = static_cast<int>(io.i64("keyOverlayMaxEntries", 5));
    request.key_overlay_filter = io.str("keyOverlayFilter", "all");
    request.key_overlay_stack = io.str("keyOverlayStack", "auto");
    request.key_overlay_modifiers = io.flag("keyOverlayModifiers", true);
    request.key_overlay_capture = io.str("keyOverlayCapture", "visible");
    request.cursor_highlight = io.flag("cursorHighlight", false);
    request.cursor_highlight_color = io.str("cursorHighlightColor", "#F5C542");
    request.cursor_highlight_size = static_cast<int>(io.i64("cursorHighlightSize", 0));
    request.cursor_clicks = io.flag("cursorClicks", false);
    request.cursor_click_color = io.str("cursorClickColor", "#EB445A");
    request.cursor_trail = io.flag("cursorTrail", false);
    request.hide_cursor_when_typing = io.flag("hideCursorWhenTyping", false);
    request.camera_overlay = io.flag("cameraOverlay", false);
    request.camera_input = io.str("cameraInput", "");
    request.camera_position = io.str("cameraPosition", "bottom-right");
    request.camera_size =
        static_cast<float>(std::clamp(io.f64("cameraSize", 22.0), 4.0, 80.0));
    request.camera_shape = io.str("cameraShape", "circle");
    request.camera_border = io.flag("cameraBorder", true);
    request.camera_border_color = io.str("cameraBorderColor", "#FFFFFF");
    request.camera_shadow = io.flag("cameraShadow", true);
    request.camera_audio = io.flag("cameraAudio", false);
    request.camera_audio_gain =
        static_cast<float>(std::clamp(io.f64("cameraAudioGain", 1.0), 0.0, 4.0));
    request.camera_resolution = io.str("cameraResolution", "auto");
    if (request.camera_resolution.empty())
        request.camera_resolution = "auto";
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    request.camera_beauty = beauty_config_from_io(io);
#endif
#if PM_HAS_AUDIO_IO
    request.audio_normalize = io.flag("audioNormalize", false);
    request.audio_auto_gain = io.flag("audioAutoGain", false);
    request.audio_filter = io.str("filter");
    if (lower_ascii(request.audio_filter) == "gtcrn")
        request.audio_filter = "deepfilter";
    request.audio_filter_model = io.str("filterModel");
    request.audio_filter_atten = static_cast<float>(io.f64("attenLim", 100.0));
    request.audio_filter_post = io.flag("postFilter", false);
    request.audio_filter_compensate_delay = io.flag("compensateDelay", true);
#endif
    {
        nlohmann::json idle = {
            {"autoPause", io.str("autoPause", "off")},
            {"autoPauseIdleMs", io.i64("autoPauseIdleMs", 3000)},
            {"autoPauseResumeMs", io.i64("autoPauseResumeMs", 300)},
            {"autoPauseGridW", io.i64("autoPauseGridW", 32)},
            {"autoPauseGridH", io.i64("autoPauseGridH", 18)},
            {"autoPauseProbeW", io.i64("autoPauseProbeW", 64)},
            {"autoPauseProbeH", io.i64("autoPauseProbeH", 36)},
            {"autoPausePixelDelta", io.i64("autoPausePixelDelta", 12)},
            {"autoPauseTilePercent", io.i64("autoPauseTilePercent", 8)},
            {"autoPauseMinTiles", io.i64("autoPauseMinTiles", 4)},
            {"autoPauseMinCoveragePercent", io.f64("autoPauseMinCoveragePercent", 2.0)},
            {"autoPauseSampleFps", io.i64("autoPauseSampleFps", 8)},
            {"autoPauseMicWake", io.flag("autoPauseMicWake", true)},
            {"autoPauseMicActivityDb", io.f64("autoPauseMicActivityDb", -40.0)},
            {"autoPauseMicHangoverMs", io.i64("autoPauseMicHangoverMs", 500)},
            {"autoPauseKeyHoldMs", io.i64("autoPauseKeyHoldMs", 750)},
            {"autoPauseMouseHoldMs", io.i64("autoPauseMouseHoldMs", 350)},
            {"autoPauseWakeKey", io.flag("autoPauseWakeKey", true)},
            {"autoPauseWakeMouse", io.flag("autoPauseWakeMouse", true)},
            {"autoPauseMouseDeltaPx", io.i64("autoPauseMouseDeltaPx", 4)},
            {"autoPausePrerollMs", io.i64("autoPausePrerollMs", 500)},
        };
        request.idle_pause = pm::video::idle_pause_from_json(idle);
        nlohmann::json cap = {
            {"captions", io.str("captions", "off")},
            {"captionFormat", io.str("captionFormat", "srt")},
            {"captionStt", io.json("captionStt")},
            {"subtitleMaxChars", io.i64("subtitleMaxChars", 42)},
            {"subtitleMaxLines", io.i64("subtitleMaxLines", 2)},
        };
        request.captions = pm::video::caption_from_json(cap);
    }
}

static bool wants_fast_transform_features(BlockIO& io)
{
    return io.f64("zoom", 1.0) != 1.0 ||
           lower_ascii(io.str("follow", "none")) != "none" ||
           lower_ascii(io.str("sizeMode", "native")) != "native" ||
           resolve_aspect_ratio(io) > 0.0 ||
           io.flag("interactiveZoom", false);
}

static bool nvenc_absent(const std::string& message)
{
    const std::string m = lower_ascii(message);
    const bool mentions_nvenc = m.find("nvenc") != std::string::npos;
    const bool missing =
        m.find("not installed") != std::string::npos ||
        m.find("unavailable") != std::string::npos ||
        m.find("could not") != std::string::npos ||
        m.find("does not include") != std::string::npos ||
        m.find("incomplete api") != std::string::npos ||
        m.find("abi version") != std::string::npos ||
        m.find("failed to load") != std::string::npos;
    return mentions_nvenc && missing;
}

static void warn_compat_fallback(BlockIO& io,
                                 const std::string& kind_id,
                                 const std::string& fast_msg)
{
    std::string msg = "videoCapture: requested fast/NVENC recording is unavailable";
    if (!fast_msg.empty())
        msg += ": " + fast_msg;
    msg += "; continuing with the compatibility recorder";
    if (wants_fast_transform_features(io)) {
        msg += "; zoom/follow/aspectRatio/sizeMode/interactiveZoom require fast "
               "WGC capture and were ignored";
    }
    logger::warn("[xblox videoCapture] " + msg);
    io.warn(kind_id, msg);
}

static void warn_fast_soft_status(BlockIO& io,
                                  const std::string& kind_id,
                                  const pm::video::RecordingStatus& status)
{
    if (!status.warning.empty())
        io.warn(kind_id, "videoCapture: " + status.warning);
}

#if defined(_WIN32)
static void warn_screen_monitor_index(BlockIO& io,
                                      const std::string& kind_id,
                                      const std::string& input)
{
    pm::video::ScreenSpec spec;
    if (!pm::video::parse_screen_spec(input, spec)) return;
    if (spec.use_current || spec.hwnd != 0 || !spec.wintitle_glob.empty()) return;
    const auto monitors = pm::video::ScreenSource::enumerate_monitors();
    if (monitors.empty()) return;
    if (spec.monitor_idx >= 0 &&
        static_cast<std::size_t>(spec.monitor_idx) < monitors.size()) {
        return;
    }
    const int clamped = static_cast<int>(monitors.size()) - 1;
    io.warn(
        kind_id,
        "videoCapture: screen:" + std::to_string(spec.monitor_idx) +
            " is out of range (" + std::to_string(monitors.size()) +
            " monitor(s)); using screen:" + std::to_string(clamped));
}
#endif

static bool start_fast_host_recording(BlockIO& io,
                                      const std::string& instance,
                                      const std::string& input,
                                      const std::string& output_path,
                                      int width,
                                      int height,
                                      int fps,
                                      int bitrate_kbps,
                                      const std::string& capture_engine,
                                      const std::string& encoder,
                                      bool include_cursor,
#if PM_HAS_AUDIO_IO
                                      const pm::audio::MixedCaptureConfig& audio_cfg,
#endif
                                      std::string& message,
                                      pm::video::RecordingStatus* status = nullptr)
{
    if (!io.runtime().options.video_recorder_handler) {
        message = "fast recorder integration is unavailable in this Xblox host";
        logger::warn("[xblox videoCapture] " + message);
        return false;
    }
    logger::warn("[xblox videoCapture] fast start instance='" + instance +
                 "' input='" + input + "' output='" + output_path +
                 "' engine=" + capture_engine + " encoder=" + encoder +
                 " fps=" + std::to_string(fps) +
                 " bitrate_kbps=" + std::to_string(bitrate_kbps)
#if PM_HAS_AUDIO_IO
                 + " audio=" + pm::audio::capture_source_name(audio_cfg.source)
#endif
    );
    media::xblox::VideoRecorderRequest req;
    req.action = "start";
    req.instance = instance;
    req.input = input;
    req.output_path = output_path;
    req.width = width;
    req.height = height;
    req.fps = fps;
    req.bitrate_kbps = bitrate_kbps;
    req.capture_engine = capture_engine;
    req.encoder = encoder;
    req.include_cursor = include_cursor;
    fill_video_transform_request(io, req);
#if PM_HAS_AUDIO_IO
    fill_video_recorder_audio(audio_cfg, req);
#endif
    media::xblox::VideoRecorderResponse response;
    bool started = false;
    try {
        started = io.runtime().options.video_recorder_handler(req, &response);
        message = std::move(response.message);
        if (status) *status = std::move(response.status);
    } catch (const std::exception& ex) {
        message = ex.what() ? ex.what() : "fast recorder failed";
        started = false;
    } catch (...) {
        message = "fast recorder failed";
        started = false;
    }
    if (started) {
        logger::warn("[xblox videoCapture] fast start ok instance='" + instance +
                     "'" + (message.empty() ? std::string{} : " message='" + message + "'"));
    } else {
        logger::warn("[xblox videoCapture] fast start failed instance='" + instance +
                     "'" + (message.empty() ? std::string{} : " message='" + message + "'") +
                     "; compatibility recorder will be used");
    }
    return started;
}

static bool fast_host_recording_request(BlockIO& io,
                                        const std::string& action,
                                        const std::string& instance,
                                        const std::string& input,
                                        std::string& message,
                                        pm::video::RecordingStatus* status = nullptr)
{
    if (!io.runtime().options.video_recorder_handler) {
        message = "fast recorder handler is unavailable in this Xblox host";
        logger::warn("[xblox videoCapture] " + message);
        return false;
    }
    media::xblox::VideoRecorderRequest req;
    req.action = action;
    req.instance = instance;
    req.input = input;
    fill_video_transform_request(io, req);
    media::xblox::VideoRecorderResponse response;
    const bool ok = io.runtime().options.video_recorder_handler(req, &response);
    message = std::move(response.message);
    if (status) *status = std::move(response.status);
    // Status is polled frequently; only log failures (or non-status actions).
    if (action != "status" || !ok) {
        logger::trace("[xblox videoCapture] fast " + action + " instance='" + instance +
                     "' result=" + (ok ? std::string("ok") : std::string("failed")) +
                     (message.empty() ? std::string{} : " message='" + message + "'"));
    }
    return ok;
}

static bool stop_fast_host_recording(BlockIO& io, const std::string& instance,
                                     const std::string& input, std::string& message,
                                     pm::video::RecordingStatus* status = nullptr)
{
    return fast_host_recording_request(
        io, "stop", instance, input, message, status);
}

// ── videoListDevices ──────────────────────────────────────────────────────────

BlockResult video_list_devices_block(BlockIO& io)
{
    nlohmann::json devices = nlohmann::json::array();
    try {
        for (const auto& dev : pm::video::enumerate_capture_devices()) {
            devices.push_back({
                {"name",       dev.name},
                {"id",         dev.id},
                {"is_default", dev.is_default},
            });
        }
    } catch (const std::exception& ex) {
        io.error("videoListDevices", std::string("videoListDevices failed: ") + ex.what(), 1);
        return BlockResult::Error;
    }

    const auto count = static_cast<long long>(devices.size());
    io.ok("videoListDevices", "found " + std::to_string(count) + " video capture device(s)")
      .set("count", count)
      .result(Value::from_json(std::move(devices)), ParamKind::json_value);
    return BlockResult::Ok;
}

// ── videoCapture ──────────────────────────────────────────────────────────────
// Captures a single still frame from a camera and writes it to a JPEG/PNG file.
// When no outputPath is given a temp JPEG is created.
// PREVIOUS / storeAs ← output file path string.

BlockResult video_capture_block(BlockIO& io)
{
    const std::string raw_input   = io.str("input");
    const std::string device_name = raw_input.empty() ? resolve_video_capture(io.str("device")) : raw_input;
    std::string       output_path = io.str("outputPath");
    int               width       = static_cast<int>(io.i64("width",  0));
    int               height      = static_cast<int>(io.i64("height", 0));
    (void)apply_aspect_to_canvas(resolve_aspect_ratio(io), width, height);
    const int         timeout_ms  = std::max(1000, static_cast<int>(io.i64("timeoutMs", 5000)));
    const int         fps         = std::clamp(static_cast<int>(io.i64("fps", 30)), 1, 240);
    const int         bitrate_kbps = std::clamp(static_cast<int>(io.i64("bitrateKbps", 0)), 0, 100000);
    const long long   duration_ms = std::max<long long>(0, io.i64("durationMs", 0));
    const bool        include_cursor = io.flag("includeCursor", true);
    const std::string requested_capture_engine = io.str("captureEngine", "auto");
    std::string       capture_engine = lower_ascii(requested_capture_engine);
    if (capture_engine != "auto" && capture_engine != "fast" &&
        capture_engine != "compat") {
        logger::warn("[xblox videoCapture] unknown captureEngine='" +
                     requested_capture_engine + "'; using auto");
        capture_engine = "auto";
    }
    const std::string requested_encoder = io.str("encoder", "auto");
    std::string       encoder = lower_ascii(requested_encoder);
    if (encoder != "auto" && encoder != "nvenc" &&
        encoder != "mediafoundation") {
        logger::warn("[xblox videoCapture] unknown encoder='" +
                     requested_encoder + "'; using auto");
        encoder = "auto";
    }
    const std::string instance    = default_record_instance(io.str("instance"), device_name);
    std::string       kind_id     = io.str("kind");
    if (kind_id.empty()) kind_id = "videoCapture";
#if PM_HAS_AUDIO_IO
    pm::audio::MixedCaptureConfig audio_cfg = video_audio_config_from_io(io);
#endif

    std::string action = lower_ascii(io.str("action"));
    if (action.empty())
        action = has_ext_ci(output_path, ".mp4") ? "record" : "still";

#if PM_HAS_AUDIO_IO
    if (action == "record" || action == "start") {
        if (!audio_cfg.mic_device.empty() &&
            !pm::audio::capture_device_matches(audio_cfg.mic_device)) {
            io.warn(kind_id,
                    "videoCapture: audio device \"" + audio_cfg.mic_device +
                    "\" was not found; using the default capture device");
            logger::warn("[xblox videoCapture] audio device \"" +
                         audio_cfg.mic_device +
                         "\" was not found; using the default capture device");
            audio_cfg.mic_device.clear();
        }
        if (audio_cfg.camera_audio && !audio_cfg.camera_device.empty() &&
            !pm::audio::capture_device_matches(audio_cfg.camera_device)) {
            io.warn(kind_id,
                    "videoCapture: camera audio device \"" + audio_cfg.camera_device +
                    "\" was not found; using the default capture device");
            logger::warn("[xblox videoCapture] camera audio device \"" +
                         audio_cfg.camera_device +
                         "\" was not found; using the default capture device");
            audio_cfg.camera_device.clear();
        }
    }
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    // Only Record / Start mux audio; Still / Stop / Status / Update carry the
    // filter params along but never reach the mux worker.
    if (action == "record" || action == "start") {
        pm::video::AudioPolishConfig polish;
        polish.filter = io.str("filter");
        polish.filter_model = io.str("filterModel");
        polish.post_filter = io.flag("postFilter", false);
        if (polish.filter_on() && !pm::audio::capture_has_speech_mic(audio_cfg)) {
            // Desktop-only loopback would reach the mux worker and fail there,
            // so keep that a hard error; with no capture at all there is
            // nothing to filter, so warn and record dry.
            if (pm::audio::capture_source_active(audio_cfg)) {
                io.error(kind_id,
                         "videoCapture: filter requires audioSource mic or mix (or cameraAudio)",
                         1);
                return BlockResult::Error;
            }
            io.warn(kind_id,
                    "videoCapture: filter needs audioSource mic or mix (or cameraAudio); "
                    "no audio is being captured, so it is ignored");
        }
    }
#endif
#if defined(FEATURE_STT) && FEATURE_STT
    // Sidecars are transcribed from the 16 kHz mux dump; with no capture source
    // the recorder writes nothing and only says so in its own log.
    if ((action == "record" || action == "start") &&
        !pm::audio::capture_source_active(audio_cfg)) {
        const nlohmann::json cap = {{"captions", io.str("captions", "off")}};
        if (pm::video::caption_from_json(cap).any()) {
            io.warn(kind_id,
                    "videoCapture: captions need an audio source (audioSource mic, desktop, "
                    "or mix, or cameraAudio); no sidecar will be written");
        }
    }
#endif
#endif

#if !defined(FEATURE_VIDEO_BEAUTY) || !FEATURE_VIDEO_BEAUTY
    if (beauty_flags_on(io)) {
        const std::string warning =
            "videoCapture: requested beauty effects are not available in this "
            "build; continuing without skin / lips / teeth / landmarks / "
            "faceGain / warmth / undereye / redness / eyeSharpen / mirror / "
            "blur / background";
        logger::warn("[xblox videoCapture] " + warning);
        io.warn(kind_id, warning);
    }
#endif
    VideoCaptureFx fx = capture_fx_from_io(io);
#if PM_HAS_AUDIO_IO && defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    if (lower_ascii(fx.audio_polish.filter) == "gtcrn") {
        io.warn(kind_id,
                "videoCapture: gtcrn is reserved; using DeepFilterNet");
        logger::warn("[xblox videoCapture] gtcrn is reserved; using DeepFilterNet");
        fx.audio_polish.filter = "deepfilter";
    }
#endif
#if !defined(FEATURE_VIDEO_VCAM) || !FEATURE_VIDEO_VCAM
    if (fx.virtual_out) {
        io.warn(kind_id, "videoCapture: virtualOut is not available in this build; ignored");
        fx.virtual_out = false;
    }
#endif
#if !defined(_WIN32)
    if (fx.preview) {
        io.warn(kind_id, "videoCapture: preview is Windows-only");
        fx.preview = false;
    }
#endif
    if (!prepare_cpu_capture(io, kind_id, fx, capture_engine, encoder))
        return BlockResult::Error;
    if (action == "still" && fx.virtual_out) {
        io.warn(kind_id, "videoCapture: virtualOut is ignored on Still; use Record or Start");
        fx.virtual_out = false;
    }

    // Status is polled frequently — skip the per-call banner.
    if (action != "status") {
        logger::trace("[xblox videoCapture] action=" + action +
                     " instance='" + instance + "' input='" + device_name +
                     "' output='" + output_path + "' engine=" + capture_engine +
                     " encoder=" + encoder + " fps=" + std::to_string(fps) +
                     " bitrate_kbps=" + std::to_string(bitrate_kbps) +
                     " duration_ms=" + std::to_string(duration_ms) +
                     (action == "record" && duration_ms == 0
                          ? " (runs until cancellation)" : ""));
    }

    if (action == "status" || action == "update") {
        std::shared_ptr<VideoCaptureRecorder> rec;
        {
            std::lock_guard<std::mutex> lock(capture_recorders_mutex());
            const auto it = capture_recorders().find(instance);
            if (it != capture_recorders().end()) rec = it->second;
        }
        if (!rec) {
            const std::string message =
                "videoCapture: no recording instance named '" +
                instance + "'";
            if (action == "update") {
                io.error(kind_id, message, 1).set("instance", instance);
                return BlockResult::Error;
            }
            io.warn(kind_id, message)
                .set("instance", instance)
                .set("running", false);
            return BlockResult::Ok;
        }
        if (action == "update" && !rec->fast()) {
            io.error(
                  kind_id,
                  "videoCapture: Update requires the fast WGC recorder", 1)
                .set("instance", instance);
            return BlockResult::Error;
        }
        std::string message;
        pm::video::RecordingStatus current;
        bool ok = true;
        if (rec->fast()) {
            ok = fast_host_recording_request(
                io, action, instance, device_name, message, &current);
        }
        if (!ok || current.failed()) {
            std::string detail = !message.empty() ? message : current.error;
            if (detail.empty()) {
                detail = "fast " + action + " failed (" +
                         pm::video::recording_state_name(current.state) + ")";
            }
            if (nvenc_absent(detail)) {
                io.warn(kind_id,
                        "videoCapture: NVENC is not installed; "
                        "recording continues with Media Foundation")
                    .set("instance", instance)
                    .set("running", rec->running())
                    .set("message", detail)
                    .result(rec->output_path(), ParamKind::video_path);
                return BlockResult::Ok;
            }
            io.error(kind_id, "videoCapture: " + detail, 1)
                .set("instance", instance);
            return BlockResult::Error;
        }
        io.ok(
              kind_id,
              action == "update"
                  ? "updated recording viewport for '" + instance + "'"
                  : "recording instance '" + instance + "' is running")
            .set("instance", instance)
            .set("running", rec->running())
            .set("fast", rec->fast())
            .set("width", static_cast<long long>(current.width))
            .set("height", static_cast<long long>(current.height))
            .set("frames", static_cast<long long>(current.frames_written))
            .set(
                "droppedFrames",
                static_cast<long long>(current.frames_dropped))
            .set("zoom", current.zoom)
            .set("follow", current.follow)
            .set(
                "viewportLeft",
                static_cast<long long>(current.viewport_left))
            .set(
                "viewportTop",
                static_cast<long long>(current.viewport_top))
            .set(
                "viewportWidth",
                static_cast<long long>(current.viewport_width))
            .set(
                "viewportHeight",
                static_cast<long long>(current.viewport_height))
            .set("message", message)
            .result(rec->output_path(), ParamKind::video_path);
        return BlockResult::Ok;
    }

    if (action == "stop") {
        std::shared_ptr<VideoCaptureRecorder> rec;
        {
            std::lock_guard<std::mutex> lk(capture_recorders_mutex());
            auto it = capture_recorders().find(instance);
            if (it != capture_recorders().end()) {
                rec = it->second;
                capture_recorders().erase(it);
            }
        }
        if (!rec) {
            io.warn(kind_id, "videoCapture: no recording instance named '" + instance + "'")
              .set("instance", instance);
            return BlockResult::Ok;
        }

        std::string stop_msg;
        pm::video::RecordingStatus fast_status;
        bool fast_stop_ok = true;
        if (rec->fast()) {
            fast_stop_ok = stop_fast_host_recording(
                io, instance, device_name, stop_msg, &fast_status);
        }
        rec->stop();
        const std::string path = rec->output_path();
        const std::string recorder_error = rec->error();
        if (path.empty()) {
            if (!recorder_error.empty()) {
                io.error(kind_id, "videoCapture: stop/finalize failed: " + recorder_error, 1)
                  .set("instance", instance)
                  .set("virtualOut", true);
                return BlockResult::Error;
            }
            io.ok(kind_id, "stopped virtual camera")
              .set("path", "")
              .set("instance", instance)
              .set("virtualOut", true)
              .set("encoder", "none")
              .result("", ParamKind::video_path);
            return BlockResult::Ok;
        }
        std::error_code output_ec;
        const auto output_size = std::filesystem::file_size(path, output_ec);
        if (!fast_stop_ok || fast_status.failed() || !recorder_error.empty() ||
            output_ec || output_size == 0) {
            std::string error = !fast_status.error.empty()
                ? fast_status.error : recorder_error;
            if (error.empty())
                error = output_ec ? "recording output was not created"
                                  : "recording output is empty";
            const bool usable = !output_ec && output_size > 0;
            if (!(nvenc_absent(error) && usable)) {
                io.error(kind_id, "videoCapture: stop/finalize failed: " + error, 1)
                  .set("path", path)
                  .set("instance", instance)
                  .set("message", stop_msg);
                return BlockResult::Error;
            }
            io.warn(kind_id,
                    "videoCapture: NVENC is not installed; kept the Media Foundation recording")
              .set("path", path)
              .set("instance", instance)
              .set("message", error);
        }
        io.ok(kind_id, "stopped recording: " + path)
          .set("path", path)
          .set("instance", instance)
          .set("message", stop_msg)
          .set("encoder", rec->fast()
              ? fast_status.effective_encoder : std::string("mediafoundation"))
          .set("effectiveBitrateKbps", static_cast<long long>(
              rec->fast() ? fast_status.bitrate_kbps : 0))
          .set("width", static_cast<long long>(fast_status.width))
          .set("height", static_cast<long long>(fast_status.height))
          .set("zoom", fast_status.zoom)
          .set("follow", fast_status.follow)
          .set("frames", static_cast<long long>(fast_status.frames_written))
          .set("droppedFrames", static_cast<long long>(fast_status.frames_dropped))
          .result(path, ParamKind::video_path);
        return BlockResult::Ok;
    }

    CaptureHud hud;
    if (action == "still" || action == "record" || action == "start") {
        hud = begin_capture_hud(io);
        if (hud) {
            hud.busy(device_name.empty() ? action : device_name);
            if (action != "start")
                hud.set_on_stop({}, true);
        }
    }

    const bool wants_recording = (action == "record" || action == "start");
    if (wants_recording && output_path.empty() && !fx.virtual_out)
        output_path = temp_capture_path(io, "xblox_capture", ".mp4");
    fx.write_file = !output_path.empty();
    if (wants_recording && !fx.write_file && !fx.virtual_out) {
        io.error(kind_id, "videoCapture: outputPath is required unless virtualOut is on", 1);
        return BlockResult::Error;
    }

    if (action == "start") {
        {
            std::lock_guard<std::mutex> lk(capture_recorders_mutex());
            if (capture_recorders().find(instance) != capture_recorders().end()) {
                io.error(kind_id, "videoCapture: recording instance already running: " + instance, 1)
                  .set("instance", instance);
                return BlockResult::Error;
            }
        }

        auto rec = std::make_shared<VideoCaptureRecorder>();
        std::string fast_msg;
        pm::video::RecordingStatus fast_status;
        bool fast_started = false;
#if defined(_WIN32)
        warn_screen_monitor_index(io, kind_id, device_name);
#endif
        if (capture_engine != "compat") {
#if PM_HAS_AUDIO_IO
            fast_started = start_fast_host_recording(
                io, instance, device_name, output_path, width, height, fps,
                bitrate_kbps, capture_engine, encoder, include_cursor, audio_cfg,
                fast_msg, &fast_status);
#else
            fast_started = start_fast_host_recording(
                io, instance, device_name, output_path, width, height, fps,
                bitrate_kbps, capture_engine, encoder, include_cursor,
                fast_msg, &fast_status);
#endif
        }
        if (fast_started) {
            rec->start_fast(device_name, output_path);
            warn_fast_soft_status(io, kind_id, fast_status);
        } else {
            if (fx.needs_cpu_pump()) {
                logger::warn("[xblox videoCapture] mirror / beauty / blur / "
                             "background / preview / virtualOut use the compatibility CPU recorder");
                io.warn(kind_id,
                        "videoCapture: mirror / beauty / blur / background / "
                        "preview / virtualOut use the compatibility CPU recorder");
            } else if (capture_engine != "compat") {
                warn_compat_fallback(io, kind_id, fast_msg);
            }
            if (bitrate_kbps > 0) {
                logger::warn("[xblox videoCapture] bitrateKbps applies only to "
                             "fast capture; compatibility encoder uses its default");
                io.warn(kind_id,
                        "videoCapture: bitrateKbps applies only to fast capture; "
                        "compatibility encoder uses its default");
            }
            fast_msg.clear();
            std::string err;
#if PM_HAS_AUDIO_IO
            if (!rec->start_generic(device_name, output_path, width, height, fps, include_cursor, 0, timeout_ms, fx, audio_cfg, err)) {
#else
            if (!rec->start_generic(device_name, output_path, width, height, fps, include_cursor, 0, timeout_ms, fx, err)) {
#endif
                io.error(kind_id, "videoCapture: " + err, 1).set("instance", instance);
                return BlockResult::Error;
            }
#if defined(_WIN32)
            rec->include_picker_hud(io.str("hudCapture", "auto") == "visible");
#endif
        }

        {
            std::lock_guard<std::mutex> lk(capture_recorders_mutex());
            capture_recorders()[instance] = rec;
        }
        if (hud) {
            if (fast_started)
                rec->set_host_stop(io.runtime().options.video_recorder_handler,
                                   instance, device_name);
            rec->take_hud(std::move(hud));
            rec->hud_busy(device_name.empty() ? instance : device_name);
        }
        const std::string start_msg = output_path.empty()
            ? "started recording instance '" + instance +
                  "' (virtual cam; run Stop with the same instance to finalize)"
            : "started recording instance '" + instance +
                  "' (run Stop with the same instance to finalize): " + output_path;
        io.ok(kind_id, start_msg)
          .set("path", output_path)
          .set("instance", instance)
          .set("running", true)
          .set("input", device_name.empty() ? std::string("default") : device_name)
          .set("fast", rec->fast())
          .set("virtualOut", fx.virtual_out)
          .set("captureEngine", capture_engine)
          .set("encoder", rec->fast()
              ? fast_status.effective_encoder
              : (output_path.empty() ? std::string("none")
                                     : std::string("mediafoundation")))
          .set("requestedEncoder", encoder)
          .set("fps", static_cast<long long>(fps))
          .set("bitrateKbps", static_cast<long long>(bitrate_kbps))
          .set("effectiveBitrateKbps", static_cast<long long>(
              rec->fast() ? fast_status.bitrate_kbps : 0))
          .set("width", static_cast<long long>(
              rec->fast() ? fast_status.width : width))
          .set("height", static_cast<long long>(
              rec->fast() ? fast_status.height : height))
          .set("zoom", fast_status.zoom)
          .set("follow", fast_status.follow)
          .set("includeCursor", include_cursor)
#if PM_HAS_AUDIO_IO
          .set("audioSource", pm::audio::capture_source_name(audio_cfg.source))
#endif
          .set("message", fast_msg)
          .result(output_path, ParamKind::video_path);
        return BlockResult::Ok;
    }

    if (action == "record") {
        std::string fast_msg;
        pm::video::RecordingStatus fast_status;
        bool fast_started = false;
        bool discard_failed_fast_output = false;
        bool safe_runtime_fallback = false;
#if defined(_WIN32)
        warn_screen_monitor_index(io, kind_id, device_name);
#endif
        if (capture_engine != "compat") {
#if PM_HAS_AUDIO_IO
            fast_started = start_fast_host_recording(
                io, instance, device_name, output_path, width, height, fps,
                bitrate_kbps, capture_engine, encoder, include_cursor, audio_cfg,
                fast_msg, &fast_status);
#else
            fast_started = start_fast_host_recording(
                io, instance, device_name, output_path, width, height, fps,
                bitrate_kbps, capture_engine, encoder, include_cursor,
                fast_msg, &fast_status);
#endif
        }
        if (fast_started) {
            warn_fast_soft_status(io, kind_id, fast_status);
            const auto started = std::chrono::steady_clock::now();
            int status_tick = 0;
            while (!io.cancelled() && !hud.stop_requested()) {
                if (duration_ms > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();
                    if (hud)
                        hud.progress(static_cast<uint64_t>(std::max<long long>(0, elapsed)),
                                     static_cast<uint64_t>(duration_ms),
                                     device_name.empty() ? "record" : device_name);
                    if (elapsed >= duration_ms) break;
                }
                if (++status_tick >= 5) {
                    status_tick = 0;
                    std::string status_msg;
                    if (!fast_host_recording_request(
                            io, "status", instance, device_name,
                            status_msg, &fast_status) ||
                        fast_status.failed()) {
                        std::string stop_msg;
                        (void)stop_fast_host_recording(
                            io, instance, device_name, stop_msg, &fast_status);
                        const std::string error = !fast_status.error.empty()
                            ? fast_status.error
                            : (status_msg.empty()
                                ? "fast recorder stopped unexpectedly"
                                : status_msg);
                        if (fast_status.frames_written == 0 || nvenc_absent(error)) {
                            fast_msg = error;
                            safe_runtime_fallback = true;
                            fast_started = false;
                            discard_failed_fast_output = true;
                            warn_compat_fallback(io, kind_id, error);
                            break;
                        }
                        io.error(kind_id, "videoCapture: " + error, 1)
                          .set("path", output_path)
                          .set("instance", instance);
                        return BlockResult::Error;
                    }
                    if (!fast_status.active())
                        break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (hud && hud.stop_requested())
                hud.busy("stopping");
            if (!safe_runtime_fallback) {
                std::string stop_msg;
                const bool stop_ok = stop_fast_host_recording(
                    io, instance, device_name, stop_msg, &fast_status);
                std::error_code output_ec;
                const auto output_size = std::filesystem::file_size(
                    output_path, output_ec);
                // Incomplete MF MP4 stubs are tiny (often ~80 bytes, no moov).
                constexpr std::uintmax_t kMinPlayableMp4Bytes = 4096;
                const bool empty_or_stub =
                    output_ec || output_size == 0 ||
                    output_size < kMinPlayableMp4Bytes ||
                    fast_status.frames_written == 0;
                if (!stop_ok || fast_status.failed() || empty_or_stub) {
                    std::string error = fast_status.error;
                    if (error.empty()) {
                        if (output_ec)
                            error = "recording output was not created";
                        else if (fast_status.frames_written == 0)
                            error = "recording stopped before any frame was written";
                        else if (output_size < kMinPlayableMp4Bytes)
                            error = "recording output is incomplete (" +
                                    std::to_string(static_cast<unsigned long long>(
                                        output_size)) +
                                    " bytes; MF Finalize likely interrupted)";
                        else
                            error = "recording output is empty";
                    }
                    if (!output_ec && output_size > 0 &&
                        output_size < kMinPlayableMp4Bytes) {
                        std::error_code rm_ec;
                        std::filesystem::remove(output_path, rm_ec);
                        logger::warn("[xblox videoCapture] removed incomplete "
                                     "output '" + output_path + "' (" +
                                     std::to_string(static_cast<unsigned long long>(
                                         output_size)) +
                                     " bytes)");
                    }
                    if (nvenc_absent(error)) {
                        safe_runtime_fallback = true;
                        fast_started = false;
                        discard_failed_fast_output = empty_or_stub;
                        warn_compat_fallback(io, kind_id, error);
                    } else {
                        logger::error("[xblox videoCapture] " + error +
                                      " frames=" +
                                      std::to_string(fast_status.frames_written) +
                                      " size=" +
                                      std::to_string(static_cast<unsigned long long>(
                                          output_ec ? 0 : output_size)));
                        io.error(kind_id, "videoCapture: " + error, 1)
                          .set("path", output_path)
                          .set("instance", instance)
                          .set("frames", static_cast<long long>(fast_status.frames_written))
                          .set("message", stop_msg);
                        return BlockResult::Error;
                    }
                } else {
                io.ok(kind_id, "recorded video to: " + output_path)
                  .set("path", output_path)
                  .set("instance", instance)
                  .set("input", device_name.empty() ? std::string("default") : device_name)
                  .set("fast", true)
                  .set("captureEngine", capture_engine)
                  .set("encoder", fast_status.effective_encoder)
                  .set("requestedEncoder", encoder)
                  .set("fps", static_cast<long long>(fps))
                  .set("bitrateKbps", static_cast<long long>(bitrate_kbps))
                  .set("effectiveBitrateKbps", static_cast<long long>(
                      fast_status.bitrate_kbps))
                  .set("width", static_cast<long long>(fast_status.width))
                  .set("height", static_cast<long long>(fast_status.height))
                  .set("zoom", fast_status.zoom)
                  .set("follow", fast_status.follow)
                  .set("viewportLeft", static_cast<long long>(
                      fast_status.viewport_left))
                  .set("viewportTop", static_cast<long long>(
                      fast_status.viewport_top))
                  .set("viewportWidth", static_cast<long long>(
                      fast_status.viewport_width))
                  .set("viewportHeight", static_cast<long long>(
                      fast_status.viewport_height))
                  .set("includeCursor", include_cursor)
                  .set("frames", static_cast<long long>(fast_status.frames_written))
                  .set("droppedFrames", static_cast<long long>(fast_status.frames_dropped))
#if PM_HAS_AUDIO_IO
                  .set("audioSource", pm::audio::capture_source_name(audio_cfg.source))
#endif
                  .set("message", stop_msg.empty() ? fast_msg : stop_msg)
                  .result(output_path, ParamKind::video_path);
                return BlockResult::Ok;
            }
        }
        }
        if (fx.needs_cpu_pump()) {
            logger::warn("[xblox videoCapture] mirror / beauty / blur / "
                         "background / preview / virtualOut use the compatibility CPU recorder");
            io.warn(kind_id,
                    "videoCapture: mirror / beauty / blur / background / "
                    "preview / virtualOut use the compatibility CPU recorder");
        } else if (!safe_runtime_fallback && capture_engine != "compat") {
            warn_compat_fallback(io, kind_id, fast_msg);
        }
        if (bitrate_kbps > 0) {
            logger::warn("[xblox videoCapture] bitrateKbps applies only to "
                         "fast capture; compatibility encoder uses its default");
            io.warn(kind_id,
                    "videoCapture: bitrateKbps applies only to fast capture; "
                    "compatibility encoder uses its default");
        }
        fast_msg.clear();
        if (discard_failed_fast_output) {
            std::error_code remove_error;
            (void)std::filesystem::remove(output_path, remove_error);
            if (remove_error) {
                io.error(
                    kind_id,
                    "videoCapture: could not replace failed fast-capture output: " +
                        remove_error.message(),
                    1)
                  .set("path", output_path);
                return BlockResult::Error;
            }
        }

        VideoCaptureRecordSummary summary;
        std::string err;
#if defined(_WIN32)
        media::win::PickerHudCaptureScope picker_hud(
            io.str("hudCapture", "auto") == "visible");
#endif
#if PM_HAS_AUDIO_IO
        const pm::audio::MixedCaptureConfig* audio_ptr =
            pm::audio::capture_source_active(audio_cfg) ? &audio_cfg : nullptr;
        if (!encode_mp4_from_frame_pump(device_name, output_path, width, height, fps, include_cursor, duration_ms, timeout_ms,
                                        [&] { return io.cancelled() || hud.stop_requested(); }, fx, summary, err, audio_ptr)) {
#else
        if (!encode_mp4_from_frame_pump(device_name, output_path, width, height, fps, include_cursor, duration_ms, timeout_ms,
                                        [&] { return io.cancelled() || hud.stop_requested(); }, fx, summary, err)) {
#endif
            io.error(kind_id, "videoCapture: " + err, 1)
              .set("input", device_name);
            return BlockResult::Error;
        }
        const std::string rec_msg = output_path.empty()
            ? "published to virtual cam" + (fx.virtual_out_name.empty()
                  ? std::string{} : (": " + fx.virtual_out_name))
            : "recorded video to: " + output_path;
        io.ok(kind_id, rec_msg)
          .set("path", output_path)
          .set("frames", summary.frames)
          .set("width", static_cast<long long>(summary.width))
          .set("height", static_cast<long long>(summary.height))
          .set("fps", static_cast<long long>(summary.fps))
          .set("input", summary.input)
          .set("source", summary.source)
          .set("fast", false)
          .set("captureEngine", capture_engine)
          .set("encoder", output_path.empty() ? "none" : "mediafoundation")
          .set("requestedEncoder", encoder)
          .set("includeCursor", include_cursor)
          .set("virtualOut", fx.virtual_out)
#if PM_HAS_AUDIO_IO
          .set("audioSource", pm::audio::capture_source_name(audio_cfg.source))
#endif
          .result(output_path, ParamKind::video_path);
        return BlockResult::Ok;
    }

    if (action != "still") {
        io.error(kind_id, "videoCapture: unknown action '" + action + "'", 1);
        return BlockResult::Error;
    }

    if (output_path.empty()) {
        output_path = temp_capture_path(io, "xblox_capture", ".jpg");
    }
    if (has_ext_ci(output_path, ".mp4")) {
        io.error(kind_id, "videoCapture: action=Still writes an image. Choose Record or Start for MP4 output.", 1)
          .set("path", output_path)
          .set("input", device_name.empty() ? std::string("default") : device_name);
        return BlockResult::Error;
    }

    try {
        pm::video::VideoFrame frame;
        std::string source = "camera";
#if defined(_WIN32)
        media::win::PickerHudCaptureScope picker_hud(
            io.str("hudCapture", "auto") == "visible");
#endif
        if (pm::video::is_screen_input_spec(device_name)) {
            pm::video::FramePumpOptions opt;
            opt.width = width;
            opt.height = height;
            opt.fps = fps;
            opt.include_cursor = include_cursor;
            opt.max_frames = 1;
            opt.open_timeout_ms = timeout_ms;
            opt.cancel = [&]() { return io.cancelled() || hud.stop_requested(); };

#if defined(_WIN32)
            ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC/MF need COM on this thread
#endif
            std::string open_err;
            auto pump = pm::video::make_frame_pump(device_name, opt, open_err);
            if (!pump) {
                io.error(kind_id, "videoCapture: " + open_err, 1)
                  .set("input", device_name);
                return BlockResult::Error;
            }
            std::string perr;
            if (!pump->next(frame, perr)) {
                io.error(kind_id, "videoCapture: no frame from source"
                                  + (perr.empty() ? std::string() : ": " + perr), 1)
                  .set("input", device_name);
                return BlockResult::Error;
            }
            source = pump->describe();
        } else {
            frame = pm::video::capture_still(
                device_name, width, height, timeout_ms,
                [&]() { return io.cancelled() || hud.stop_requested(); });
        }
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        if (fx.use_beauty) {
            pm::video::BeautyConfig still_cfg = fx.beauty;
            still_cfg.running_mode = "image";
            std::string probe_err;
            if (!pm::video::probe_beauty_runtime(still_cfg, probe_err)) {
                io.error(kind_id, "videoCapture: " +
                                      (probe_err.empty() ? std::string("filter runtime not available")
                                                         : probe_err),
                         1);
                return BlockResult::Error;
            }
            if (!pm::video::is_packed_bgr24(frame)) {
                pm::video::VideoFrame converted;
                if (!pm::video::to_bgr24(frame, converted)) {
                    io.error(kind_id, "videoCapture: frame format is not encodable", 1);
                    return BlockResult::Error;
                }
                frame = std::move(converted);
            }
            pm::video::BeautyProcessor beauty;
            std::string berr;
            if (!beauty.create(still_cfg, berr) ||
                !beauty.process(frame, frame.timestamp_ms > 0 ? frame.timestamp_ms : 1, berr)) {
                io.error(kind_id, "videoCapture: beauty failed: " + berr, 1);
                return BlockResult::Error;
            }
        }
#endif
        std::string err;
        if (!pm::video::save_frame(frame, output_path, err)) {
            io.error(kind_id, "videoCapture: save_frame failed: " + err, 1);
            return BlockResult::Error;
        }

#if defined(_WIN32)
        if (fx.preview) {
            if (!pm::video::is_packed_bgr24(frame)) {
                pm::video::VideoFrame converted;
                if (!pm::video::to_bgr24(frame, converted)) {
                    io.error(kind_id, "videoCapture: frame format is not encodable", 1);
                    return BlockResult::Error;
                }
                frame = std::move(converted);
            }
            pm::video::FramePreview preview;
            if (!preview.show(frame.width, frame.height, "videoCapture")) {
                io.error(kind_id, "videoCapture: could not open preview window", 1);
                return BlockResult::Error;
            }
            const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
            preview.update(frame.data.data(), frame.width, frame.height, stride);
            while (preview.is_open() && !io.cancelled() && !hud.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
#endif

        io.ok(kind_id, "captured frame to: " + output_path)
          .set("path", output_path)
          .set("width", static_cast<long long>(frame.width))
          .set("height", static_cast<long long>(frame.height))
          .set("input", device_name.empty() ? std::string("default") : device_name)
          .set("source", source)
          .result(output_path, ParamKind::image_path);
        return BlockResult::Ok;
    } catch (const std::exception& ex) {
        // Device not available (no webcam)  -  soft error, do not crash.
        io.error(kind_id, std::string("videoCapture: ") + ex.what(), 1)
          .set("input", device_name);
        return BlockResult::Error;
    }
}

// ── videoSource ───────────────────────────────────────────────────────────────
// videoSource  -  the flat-pipe head. It pumps frames from a webcam, screen/window,
// still image, OR a video file and, for every frame, runs its child `items`
// (filters / videoWriter / videoDetect / pictureOut) with PREVIOUS set to that
// frame's in-memory handle. So a clip transcodes through `videoSource -> items`
// with NO for/while loop and NO storeAs in the document  -  the block owns the
// per-frame loop, exactly like a stream that pushes chunks until end-of-stream.
//   input = ""  / "NexiGo"      -> webcam stream     (live: until frames/durationMs/cancel)
//   input = "screen:0[:...]"      -> screen / window   (live)
//   input = "clip.mp4"          -> decode to EOF      (finite, Win32 Media Foundation)
//   input = "frame.png"         -> one decoded frame  (finite)
//
// With NO items it is a one-shot grabber: pulls a single frame and emits a path
// (emit=file/both) or a mem:// handle (emit=handle) for a picture pipeline.
//   emit = both  -> write file AND cache the frame (next filter skips the decode)
//        = file  -> write only;  = handle -> in-memory only (no file)
//
// "stream.end()" is EOF for files/images, or a stop condition for live sources
// (frames / durationMs / cancellation). PREVIOUS / storeAs ← handle/path (or, in
// pump mode, the processed frame count).

BlockResult video_source_block(BlockIO& io)
{
    std::string kind_id = io.str("kind");
    if (kind_id.empty()) kind_id = "videoSource";

    const std::string input       = io.str("input");
    std::string       output_path = io.str("outputPath");
    std::string       emit        = io.str("emit", "both");  // both | file | handle
    if (emit != "file" && emit != "handle") emit = "both";

    pm::video::FramePumpOptions opt;
    opt.width           = static_cast<int>(io.i64("width", 0));
    opt.height          = static_cast<int>(io.i64("height", 0));
    opt.fps             = static_cast<int>(io.i64("fps", 0));
    opt.max_frames      = io.i64("frames", 0);       // 0 = unbounded (files -> EOF)
    opt.duration_ms     = io.i64("durationMs", 0);   // live wall-clock cap (0 = none)
    opt.open_timeout_ms = std::max(1000, static_cast<int>(io.i64("timeoutMs", 5000)));
    opt.cancel          = [&]() { return io.cancelled(); };

#if defined(_WIN32)
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC/MF need COM on this thread
#endif

    // Coarse label for the summary (the pump itself routes precisely).
    std::string src_kind = "camera";
    if (pm::video::is_video_file_spec(input))      src_kind = "video";
    else if (pm::video::is_screen_input_spec(input)) src_kind = "screen";
    else if (!input.empty()) {
        std::error_code fec;
        if (std::filesystem::is_regular_file(media::path::path_from_utf8(input), fec)) src_kind = "image";
    }

    std::string open_err;
    auto pump = pm::video::make_frame_pump(input, opt, open_err);
    if (!pump) {
        io.error(kind_id, "videoSource: " + open_err, 1)
          .set("input", input.empty() ? std::string("default") : input);
        return BlockResult::Error;
    }

    // ── Pump mode: drive the child pipeline once per decoded frame ─────────────
    if (io.has_children("items")) {
        FrameStore* fs = frame_store(io);
        if (!fs) {
            io.error(kind_id, "videoSource: streaming a pipeline requires a run scope", 1);
            return BlockResult::Error;
        }
        // Publish the source fps so a downstream videoWriter can transcode at the
        // right speed (it defaults fps to this when the author didn't pin one).
        const double src_fps = pump->source_fps();
        if (src_fps > 0.0)
            io.set_output("videoSourceFps", ParamKind::json_value, src_fps);

        // Per-frame child events are muted (a 2000-frame clip would emit ~3 events
        // ×frame); PREVIOUS/storeAs still chain through the pipeline. A progress
        // event surfaces every `progressEvery` frames (0 = only the final summary).
        const long long progress_every = std::max<long long>(0, io.i64("progressEvery", 60));
        const bool      prev_events     = io.events_enabled();

        long long             n = 0, last_w = 0, last_h = 0;
        pm::video::VideoFrame frame;
        std::string           perr;
        while (pump->next(frame, perr)) {
            last_w = frame.width;
            last_h = frame.height;
            // Cache the frame and hand its mem:// token to the pipeline as PREVIOUS.
            const std::string token = fs->put_mem(std::move(frame));
            io.prime_previous(token);   // no per-frame output record (long streams)
            io.set_events_enabled(false);
            io.run("items");            // filters -> writer / pictureOut / detect ...
            io.set_events_enabled(prev_events);
            ++n;
            if (progress_every > 0 && (n % progress_every) == 0)
                io.ok(kind_id, "... " + std::to_string(n) + " " + src_kind + " frames")
                  .set("frames", n);
            if (io.cancelled()) break;
        }
        io.set_events_enabled(prev_events);
        if (!perr.empty()) {
            io.error(kind_id, "videoSource: " + perr, 1)
              .set("input", input).set("frames", n);
            return BlockResult::Error;
        }
        io.ok(kind_id, "streamed " + std::to_string(n) + " " + src_kind + " frame(s)"
                       + (src_fps > 0.0 ? " @ " + std::to_string(static_cast<int>(src_fps + 0.5)) + "fps" : ""))
          .set("frames", n)
          .set("width",  last_w)
          .set("height", last_h)
          .set("fps",    src_fps)
          .set("source", src_kind)
          .set("input",  input.empty() ? std::string("default") : input)
          .result(Value(n), ParamKind::json_value);
        return BlockResult::Ok;
    }

    // ── One-shot mode: single frame -> handle / file (picture-pipeline head) ────
    pm::video::VideoFrame frame;
    std::string           perr;
    if (!pump->next(frame, perr)) {
        io.error(kind_id, "videoSource: no frame from source"
                          + (perr.empty() ? std::string() : ": " + perr), 1)
          .set("input", input.empty() ? std::string("default") : input);
        return BlockResult::Error;
    }

    const long long fw = frame.width, fh = frame.height;
    std::string token;
    if (emit == "handle") {
        FrameStore* fs = frame_store(io);
        if (!fs) {
            io.error(kind_id, "videoSource: emit=handle requires a run scope", 1);
            return BlockResult::Error;
        }
        token = fs->put_mem(std::move(frame));
    } else {
        if (output_path.empty()) {
            std::error_code ec;
            const auto tmp = std::filesystem::temp_directory_path(ec);
            const std::string tmp_dir = ec ? std::string(".") : media::path::path_to_utf8(tmp);
            std::string idx_suffix;
            if (const nlohmann::json* li = io.context("loop.index")) {
                if (li->is_number()) idx_suffix = "_" + std::to_string(li->get<int>());
            }
            output_path = tmp_dir + "/xblox_source" + idx_suffix + "_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jpg";
        }
        std::string save_err;
        if (!pm::video::save_frame(frame, output_path, save_err)) {
            io.error(kind_id, "videoSource: save_frame failed: " + save_err, 1);
            return BlockResult::Error;
        }
        if (emit == "both") {
            if (FrameStore* fs = frame_store(io)) fs->put(output_path, std::move(frame));
        }
        token = output_path;
    }

    io.ok(kind_id, "captured " + src_kind + " frame -> " + token)
      .set("path",   token)
      .set("width",  fw)
      .set("height", fh)
      .set("source", src_kind)
      .set("input",  input.empty() ? std::string("default") : input)
      .result(token, ParamKind::image_path);
    return BlockResult::Ok;
}

#endif // FEATURE_VIDEO


} // namespace media::xblox::blocks
