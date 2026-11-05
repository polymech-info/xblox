// XBlox video detection/screen blocks - videoListScreens, videoScreenCapture, videoDetect.
//
// Gated on FEATURE_VIDEO_DETECTION (Win32 screen capture + ONNX).

#include "blocks/video_blocks.hpp"

#include "logger/logger.h"
#include "xblox/xblox_constants.hpp"
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "core/settings_runtime.hpp"
#include "core/gpu_memory_stats.hpp"
#endif

#if defined(FEATURE_VIDEO_DETECTION) && FEATURE_VIDEO_DETECTION

#include "core/video.hpp"

#if defined(_WIN32)
#include "core/screen_input.hpp"
#include "win/detect_overlay.hpp"
#include "win/box_smoother.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

inline void ensure_com_initialized() noexcept {
    static thread_local bool s_done = false;
    if (!s_done) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        s_done = true;
    }
}
#endif

#include "lib/onnx/OnnxSession.hpp"
#include "lib/onnx/OnnxImagePrep.hpp"
#include "lib/onnx/OnnxDetect.hpp"
#include "lib/onnx/OnnxCocoLabels.hpp"
#include "lib/onnx/OnnxYamlUtil.hpp"

#include "video/detect/types.hpp"
#include "video/detect/commons.hpp"
#include "video/detect/boxes.hpp"
#include "video/detect/cls.hpp"
#include "video/detect/obb.hpp"
#include "video/detect/pose.hpp"
#include "video/detect/seg.hpp"
#include "video/detect/serialize.hpp"
#include "video/detect/render.hpp"

#include "core/video_encoder.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "string/string_utils.hpp"

#endif // FEATURE_VIDEO_DETECTION

namespace media::xblox::blocks {

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
std::string resolve_video_capture(const std::string& block_device);
#endif

// ── Screen / detection blocks (FEATURE_VIDEO_DETECTION + Win32) ───────────────

#if defined(FEATURE_VIDEO_DETECTION) && FEATURE_VIDEO_DETECTION

// ── videoListScreens ──────────────────────────────────────────────────────────
// Lists all physical monitors and visible top-level windows.
// PREVIOUS / storeAs ← { monitors:[...], windows:[...] }

BlockResult video_list_screens_block(BlockIO& io)
{
#if defined(_WIN32)
    nlohmann::json monitors_arr = nlohmann::json::array();
    for (const auto& m : pm::video::ScreenSource::enumerate_monitors()) {
        monitors_arr.push_back({
            {"index",      m.index},
            {"name",       m.name},
            {"x", m.x}, {"y", m.y}, {"w", m.w}, {"h", m.h},
            {"is_primary", m.is_primary},
        });
    }

    nlohmann::json windows_arr = nlohmann::json::array();
    for (const auto& w : pm::video::ScreenSource::enumerate_windows()) {
        windows_arr.push_back({
            {"title", w.title},
            {"pid",   w.pid},
            {"x", w.x}, {"y", w.y}, {"w", w.w}, {"h", w.h},
        });
    }

    const auto n_mon = monitors_arr.size();
    const auto n_win = windows_arr.size();
    nlohmann::json result = {
        {"monitors", std::move(monitors_arr)},
        {"windows",  std::move(windows_arr)},
    };
    io.ok("videoListScreens",
          "found " + std::to_string(n_mon) + " monitor(s), " + std::to_string(n_win) + " window(s)")
      .set("monitors", static_cast<long long>(n_mon))
      .set("windows",  static_cast<long long>(n_win))
      .result(Value::from_json(std::move(result)), ParamKind::json_value);
    return BlockResult::Ok;
#else
    io.error("videoListScreens", "videoListScreens requires Win32", 1);
    return BlockResult::Error;
#endif
}

// ── videoScreenCapture ────────────────────────────────────────────────────────
// Single-shot GDI BitBlt capture of a screen or window.
// Block params: input (spec), outputPath, storeAs.
// PREVIOUS / storeAs ← output file path string.

#if defined(_WIN32)
BlockResult video_screen_capture_block(BlockIO& io)
{
    ensure_com_initialized();  // needed by WIC (save_frame -> CoCreateInstance)

    const std::string input_spec  = io.str("input");
    std::string       output_path = io.str("outputPath");

    if (input_spec.empty()) {
        io.error("videoScreenCapture", "videoScreenCapture: 'input' is required (e.g. \"screen:0\")", 1);
        return BlockResult::Error;
    }

    if (output_path.empty()) {
        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        output_path = (ec ? std::string(".") : media::path::path_to_utf8(tmp))
            + "/xblox_screen_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + ".jpg";
    }

    std::string cap_err;
    const pm::video::VideoFrame frame = pm::video::capture_still_screen(input_spec, cap_err);
    if (frame.width <= 0) {
        io.error("videoScreenCapture", "videoScreenCapture: capture failed: " + cap_err, 1);
        return BlockResult::Error;
    }

    std::string save_err;
    if (!pm::video::save_frame(frame, output_path, save_err)) {
        io.error("videoScreenCapture", "videoScreenCapture: save_frame failed: " + save_err, 1);
        return BlockResult::Error;
    }

    io.ok("videoScreenCapture", "captured screen to: " + output_path)
      .set("path", output_path)
      .set("width",  static_cast<long long>(frame.width))
      .set("height", static_cast<long long>(frame.height))
      .set("input",  input_spec)
      .result(output_path, ParamKind::image_path);
    return BlockResult::Ok;
}
#else
BlockResult video_screen_capture_block(BlockIO& io)
{
    io.error("videoScreenCapture", "videoScreenCapture requires Win32", 1);
    return BlockResult::Error;
}
#endif

// ── videoDetect ───────────────────────────────────────────────────────────────
// Captures one frame (webcam or screen) and runs YOLO detection.
// Block params: model, input, labels, classes, conf, nms, inputSize, threads,
//               provider, timeoutMs, outputPath, storeAs.
//   input = "" | device name = webcam  (via capture_still)
//   input = "screen:N[:wintitle*]"     = screen capture (Win32 only)
// PREVIOUS / storeAs ← { ok, image_w, image_h, infer_ms, boxes:[{x,y,w,h,conf,cls,label}] }

#if defined(_WIN32)
// ── Decoupled burn-in recorder (screen capture) ──────────────────────────────
// The detect block runs capture->inference->encode synchronously, so a naive
// burn-in records only one frame per loop tick (≈ inference rate) and the video
// looks like it is dropping frames. This recorder owns its OWN capture+encode
// thread that grabs the screen at recordFps and composites the most recent
// detections the block publishes each tick. Result: smooth, real-time video;
// the overlay simply refreshes whenever inference produces new boxes.
//
// Screen-only by design: two GDI captures of the same window are cheap, whereas
// a webcam device cannot be opened twice  -  that path keeps the synchronous sink.
class BurnInRecorder {
public:
    ~BurnInRecorder() { stop(); }

    bool running() const noexcept { return running_.load(); }

    void start(const std::string& input_spec, const std::string& path, int fps) {
        if (running_.load()) return;
        input_spec_ = input_spec;
        path_       = path;
        fps_        = std::clamp(fps, 1, 240);
        stop_.store(false);
        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        const bool was = running_.exchange(false);
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        (void)was;
    }

    // Publish the latest detections (already in captured-frame pixel space).
    void publish(std::vector<media::vdetect::Detection> dets) {
        std::lock_guard<std::mutex> lk(mu_);
        dets_ = std::move(dets);
    }

private:
    void run() {
        ensure_com_initialized();   // WIC / save paths used downstream
        pm::video::VideoEncoder enc;
        bool opened = false;
        const auto frame_dur = std::chrono::microseconds(1'000'000 / std::max(1, fps_));
        auto next = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            std::string cap_err;
            // include_layered=false: never capture the live DetectOverlay that
            // sits over this same region (it draws the smoothed skeleton). We
            // composite our own annotations below, so capturing it too would
            // double-paint. WDA_EXCLUDEFROMCAPTURE can't help on Win10 < 2004.
            pm::video::VideoFrame frame =
                pm::video::capture_still_screen(input_spec_, cap_err, /*include_layered=*/false);
            const bool drawable = frame.width > 0
                && (frame.format == pm::video::PixelFormat::BGR24
                 || frame.format == pm::video::PixelFormat::RGB24);
            if (drawable) {
                if (!opened) {
                    std::string e;
                    opened = enc.open(path_, frame.width, frame.height, fps_, e);
                    if (!opened) {
                        spdlog::warn("videoDetect: record open failed: {}", e);
                        break;
                    }
                }
                std::vector<media::vdetect::Detection> dets;
                { std::lock_guard<std::mutex> lk(mu_); dets = dets_; }
                media::vdetect::Canvas cv;
                cv.data     = frame.data.data();
                cv.width    = frame.width;
                cv.height   = frame.height;
                cv.stride   = frame.stride > 0 ? frame.stride : frame.width * 3;
                cv.channels = 3;
                cv.bgr      = (frame.format == pm::video::PixelFormat::BGR24);
                media::vdetect::RenderOptions ropt;
                ropt.line_width = std::max(2, frame.width / 480);
                ropt.font_scale = std::max(1, frame.width / 640);
                media::vdetect::draw_detections(cv, dets, 1.f, 1.f, ropt);
                std::string e;
                (void)enc.write(frame, e);   // transient write errors are non-fatal
            }
            // Pace to recordFps without accumulating drift when we fall behind.
            next += frame_dur;
            const auto now = std::chrono::steady_clock::now();
            if (next > now) std::this_thread::sleep_for(next - now);
            else            next = now;
        }
        std::string e;
        enc.close(e);
    }

    std::thread                            thread_;
    std::atomic<bool>                      running_{false};
    std::atomic<bool>                      stop_{false};
    std::mutex                             mu_;
    std::vector<media::vdetect::Detection> dets_;
    std::string                            input_spec_;
    std::string                            path_;
    int                                    fps_ = 30;
};
#endif // _WIN32

// A cached detect instance: a loaded ONNX session + resolved class names, reused
// across loop passes via the run scope (keyed by the `instance` name or an args
// hash). Future temporal smoother + live overlay window will live here too, so
// they persist between frames instead of being rebuilt every pass.
struct VideoDetectInstance {
    media::onnx::OnnxSession  session;
    std::vector<std::string>  cls_names;
    std::string               model;
    int                       threads  = 0;
    media::onnx::OnnxProvider provider = media::onnx::OnnxProvider::CPU;
    bool                      loaded   = false;
    // Resolved task for the cached cls_names (rebuilt when the task changes so the
    // default label set tracks detect->COCO / obb->DOTA / classify->ImageNet).
    media::vdetect::Task      task     = media::vdetect::Task::Auto;

    // Burn-in recorder: annotated frames -> .mp4. Opened lazily on the first frame
    // (needs its size) and finalized in the encoder's destructor when this
    // instance is released (loop end / `xblox session stop`).
    std::unique_ptr<pm::video::VideoEncoder> encoder;
    bool                                     rec_started = false;
#if defined(_WIN32)
    // Background screen recorder (smooth real-time burn-in). Used for "screen:*"
    // inputs; webcam inputs fall back to the synchronous `encoder` above.
    std::unique_ptr<BurnInRecorder>          recorder;
#endif
#if defined(_WIN32)
    // Visualization persists across passes: the overlay window stays open and the
    // smoother accumulates temporal state. Both are torn down when the run scope
    // releases this instance (loop end / session stop).
    std::unique_ptr<pm::win::DetectOverlay> overlay;
    BoxSmoother                             smoother;
    bool                                    viz_started = false;
    long long                               frame_idx   = 0;
#endif
};

#if defined(_WIN32)
// Compute the screen-space rect a "screen:..." spec maps to, so the overlay
// window lands exactly over the captured pixels. Delegates to the single shared
// resolver (screen_input) which understands rect=/hwnd=/wintitle=/rrect= the
// same way the capture loop does  -  so a "follow" selector
// (hwnd=<root>:rrect=dx,dy,w,h) anchors the overlay on the sub-element, not the
// monitor origin.
inline RECT overlay_rect_for_spec(const std::string& spec)
{
    // Per-monitor DPI so GetWindowRect agrees with the (physical-px) capture path.
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    pm::video::ScreenSpec ss;
    pm::video::parse_screen_spec(spec, ss);
    return pm::video::resolve_screen_rect(ss);
}
#endif

// FNV-1a over the identity args -> a stable per-config key for unnamed instances
// (changing model/provider/threads/input yields a fresh cache entry).
std::string detect_args_hash(const std::string& model, media::onnx::OnnxProvider provider,
                             int threads, const std::string& input,
                             const std::string& labels, const std::string& classes)
{
    std::string sig = model + "|" + std::to_string(static_cast<int>(provider)) + "|"
                    + std::to_string(threads) + "|" + input + "|" + labels + "|" + classes;
    std::uint64_t h = media::xblox::constants::kFnv1a64OffsetBasis;
    for (unsigned char c : sig) { h ^= c; h *= media::xblox::constants::kFnv1a64Prime; }
    return std::to_string(h);
}

nlohmann::json gpu_memory_json(const media::gpu::GpuMemorySampler& sampler)
{
    nlohmann::json j;
    j["available"] = sampler.peak.available;
    if (!sampler.peak.available) return j;

    j["baseline_used_mib"] = sampler.baseline.used_mib;
    j["peak_used_mib"]     = sampler.peak.used_mib;
    j["peak_delta_mib"]    = sampler.delta_mib();
    j["total_mib"]         = sampler.peak.total_mib;
    j["devices"]           = nlohmann::json::array();
    for (const auto& dev : sampler.peak.devices) {
        j["devices"].push_back({
            {"index", dev.index},
            {"name", dev.name},
            {"used_mib", dev.used_mib},
            {"total_mib", dev.total_mib},
        });
    }
    return j;
}

// Prep a captured frame, run inference, and decode it for the requested task.
// Mirrors the CLI `video detect` path: per-task preprocessing (letterbox for
// boxes, stretch for classify), input size auto-derived from the model's static
// dim (detect 640 / classify 224 / obb 1024).
struct FrameInfer {
    bool                        ok = false;
    std::string                 error;
    int                         image_w = 0, image_h = 0;
    double                      infer_ms = 0.0;
    media::vdetect::DecodeResult dr;
};

FrameInfer infer_frame(media::onnx::OnnxSession& session,
                       const pm::video::VideoFrame& frame,
                       media::vdetect::Task task, int input_size_arg,
                       float conf, float nms, int top_k,
                       const std::vector<std::string>& names)
{
    namespace vd = media::vdetect;
    FrameInfer out;

    int model_in = 0;
    {
        const auto ii = session.input_info();
        if (!ii.empty() && ii[0].shape.size() >= 4 && ii[0].shape[2] > 0)
            model_in = static_cast<int>(ii[0].shape[2]);
    }
    const bool is_classify = (task == vd::Task::Classify);
    media::onnx::ImagePrepOptions prep;
    int input_size;
    if (is_classify) {
        input_size = model_in > 0 ? model_in : 224;
        prep.target_w = prep.target_h = input_size;
        prep.force_size = true;  prep.letterbox = false; prep.normalize = false;
    } else {
        input_size = model_in > 0 ? model_in
                                  : (input_size_arg > 0 ? input_size_arg : 640);
        prep.target_w = prep.target_h = input_size;
        prep.force_size = false; prep.letterbox = true;
        prep.letterbox_fill = 114.f; prep.normalize = false;
    }

    const bool is_raw = (frame.format == pm::video::PixelFormat::BGR24);
    int orig_w = 0, orig_h = 0;
    media::onnx::LetterboxInfo lb;
    std::string perr;
    auto tensor = media::onnx::prepare_frame_tensor(
        frame.data.data(), frame.data.size(), frame.width, frame.height,
        is_raw, true, prep, perr, &orig_w, &orig_h, &lb);
    if (tensor.empty()) { out.error = "preprocessing failed: " + perr; return out; }
    out.image_w = orig_w; out.image_h = orig_h;

    const std::vector<int64_t> shape = {1, 3,
        static_cast<int64_t>(prep.target_h), static_cast<int64_t>(prep.target_w)};
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto run = session.run_with_shapes({tensor}, {shape});
    out.infer_ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();

    switch (task) {
        case vd::Task::Classify:
            out.dr = vd::decode_classify(run, top_k, names); break;
        case vd::Task::OBB:
            out.dr = vd::decode_obb(run, lb, orig_w, orig_h, input_size, conf, nms, names); break;
        case vd::Task::Pose:
            out.dr = vd::decode_pose(run, lb, orig_w, orig_h, input_size, conf, nms, names); break;
        case vd::Task::Segment:
            out.dr = vd::decode_segment(run, lb, orig_w, orig_h, input_size, conf, nms, names); break;
        default:
            out.dr = vd::decode_detect(run, lb, orig_w, orig_h, input_size, conf, nms, names); break;
    }
    out.ok = out.dr.ok;
    if (!out.ok) out.error = out.dr.error;
    return out;
}

BlockResult video_detect_block(BlockIO& io)
{
#if defined(_WIN32)
    ensure_com_initialized();  // needed by WIC (save_frame -> CoCreateInstance)
#endif
    const std::string model_path  = io.str("model");
    const std::string input_spec  = resolve_video_capture(io.str("input"));
    const std::string labels_yaml = io.str("labels");
    const std::string classes_csv = io.str("classes");
    const std::string filter_csv  = io.str("filterClasses");
    std::string       output_path = io.str("outputPath");
    const std::string task_str    = io.str("task");
    const float       conf        = static_cast<float>(io.f64("conf", 0.25));
    const float       nms         = static_cast<float>(io.f64("nms",  0.45));
    const int         input_size  = static_cast<int>(io.i64("inputSize", 0));
    const int         top_k       = std::max(1, static_cast<int>(io.i64("topK", 5)));
    const int         threads     = std::max(1, static_cast<int>(io.i64("threads", 4)));
    const media::onnx::OnnxProvider provider = media::onnx::parse_provider(io.str("provider", "auto"));
    media::gpu::GpuMemorySampler gpu_sampler(
        io.flag("gpuStats", false) && provider != media::onnx::OnnxProvider::CPU);

    if (model_path.empty()) {
        io.error("videoDetect", "videoDetect: 'model' ONNX path is required", 1);
        return BlockResult::Error;
    }
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(media::path::path_from_utf8(model_path), ec) || ec) {
            io.error("videoDetect", "videoDetect: model file not found: " + model_path, 1);
            return BlockResult::Error;
        }
    }

    // ── Resolve a (cached) detect instance ─────────────────────────────────────
    // Loading the model and parsing labels are expensive, so cache them in the
    // run scope keyed by the explicit `instance` name (else an args hash). Across
    // a --loop run the same session is reused every pass and released when the
    // run ends / `session stop`. Without a run scope (detached) build a throwaway.
    const std::string instance_name = io.str("instance");

    // Class-name precedence mirrors the CLI: labels YAML > classes CSV > per-task
    // default (obb -> DOTA-15, classify -> ImageNet beside the model, else COCO-80).
    const auto build_names = [&](media::vdetect::Task t) -> std::vector<std::string> {
        std::vector<std::string> names;
        if (!labels_yaml.empty()) {
            names = media::onnx::parse_yolo_yaml_names(labels_yaml);
            if (names.empty())
                spdlog::warn("videoDetect: could not load labels from '{}'", labels_yaml);
        }
        if (names.empty() && !classes_csv.empty()) {
            std::stringstream ss(classes_csv);
            std::string item;
            while (std::getline(ss, item, ',')) {
                const auto b = item.find_first_not_of(" \t");
                const auto e = item.find_last_not_of(" \t");
                if (b != std::string::npos) names.push_back(item.substr(b, e - b + 1));
            }
        }
        if (names.empty()) {
            if (t == media::vdetect::Task::OBB) {
                names = media::vdetect::dota15_labels();
            } else if (t == media::vdetect::Task::Classify) {
                const auto dir  = media::path::path_from_utf8(model_path).parent_path();
                const auto cand = media::path::path_to_utf8((dir / "imagenet-classes.txt"));
                std::error_code ec;
                if (std::filesystem::exists(cand, ec))
                    names = media::vdetect::load_label_lines(cand);
            } else {
                names = media::onnx::coco80_labels();
            }
        }
        return names;
    };

    const auto load_session = [&](VideoDetectInstance& inst) {
        inst.model    = model_path;
        inst.threads  = threads;
        inst.provider = provider;
        inst.loaded   = inst.session.load(model_path, threads, provider);
        gpu_sampler.sample();
        inst.cls_names.clear();  // re-resolved once the task is known
    };

    VideoDetectInstance  local;             // fallback storage when no run scope
    VideoDetectInstance* inst = nullptr;
    if (RunScope* scope = io.scope()) {
        const std::string key = instance_name.empty()
            ? "videoDetect:#" + detect_args_hash(model_path, provider, threads, input_spec, labels_yaml, classes_csv)
            : "videoDetect:" + instance_name;
        inst = &scope->acquire<VideoDetectInstance>(
            key, [] { return std::make_shared<VideoDetectInstance>(); });
        // First use, or an explicit instance re-pointed at a different model ->
        // (re)build the session in place; otherwise reuse the cached one.
        if (!inst->loaded || inst->model != model_path
            || inst->threads != threads || inst->provider != provider) {
            inst->session = media::onnx::OnnxSession{};
            load_session(*inst);
        }
    } else {
        load_session(local);
        inst = &local;
    }

    if (!inst->loaded) {
        io.error("videoDetect", "videoDetect: model load failed: " + model_path
                 + (inst->session.error().empty() ? "" : " — " + inst->session.error()), 1);
        return BlockResult::Error;
    }
    media::onnx::OnnxSession& session = inst->session;
    const media::onnx::OnnxProvider active_provider = session.active_provider();

    // ── Resolve task + (cached, task-aware) class names ────────────────────────
    media::vdetect::Task task = media::vdetect::task_from_string(task_str);
    if (task == media::vdetect::Task::Auto)
        task = media::vdetect::autodetect_task(session);
    if (task == media::vdetect::Task::Semantic) {
        io.error("videoDetect", std::string("videoDetect: task '")
                 + media::vdetect::task_to_string(task) + "' is not implemented yet", 1);
        return BlockResult::Error;
    }
    if (inst->cls_names.empty() || inst->task != task) {
        inst->cls_names = build_names(task);
        inst->task = task;
    }
    const std::vector<std::string>& cls_names = inst->cls_names;
    const bool is_classify = (task == media::vdetect::Task::Classify);

    // ── Capture frame ─────────────────────────────────────────────────────────
    pm::video::VideoFrame frame;
#if defined(_WIN32)
    if (!input_spec.empty() && input_spec.rfind("screen:", 0) == 0) {
        std::string cap_err;
        frame = pm::video::capture_still_screen(input_spec, cap_err);
        if (frame.width <= 0) {
            io.error("videoDetect", "videoDetect: screen capture failed: " + cap_err, 1);
            return BlockResult::Error;
        }
    } else
#endif
    {
        const int timeout_ms = std::max(1000, static_cast<int>(io.i64("timeoutMs", 5000)));
        try {
            frame = pm::video::capture_still(
                input_spec, 0, 0, timeout_ms, [&]{ return io.cancelled(); });
        } catch (const std::exception& ex) {
            io.error("videoDetect", std::string("videoDetect: capture failed: ") + ex.what(), 1);
            return BlockResult::Error;
        }
        if (frame.width <= 0) {
            io.error("videoDetect", "videoDetect: capture produced no frame", 1);
            return BlockResult::Error;
        }
    }

    // ── Inference + task decode (shared video/detect lib) ──────────────────────
    FrameInfer fi = infer_frame(session, frame, task, input_size, conf, nms, top_k, cls_names);
    if (active_provider == media::onnx::OnnxProvider::CUDA) gpu_sampler.sample();

    // ── Optional class filter (detection tasks; parity with CLI --filter-classes)
    if (fi.ok && !is_classify && !filter_csv.empty()) {
        std::vector<std::string> keep;
        std::stringstream fs(filter_csv);
        std::string item;
        while (std::getline(fs, item, ',')) {
            const auto b = item.find_first_not_of(" \t");
            const auto e = item.find_last_not_of(" \t");
            if (b == std::string::npos) continue;
            std::string lo = item.substr(b, e - b + 1);
            lo = media::strutil::lower_ascii(std::move(lo));
            keep.push_back(std::move(lo));
        }
        if (!keep.empty()) {
            const auto in_keep = [&](std::string name) {
                name = media::strutil::lower_ascii(std::move(name));
                return std::find(keep.begin(), keep.end(), name) != keep.end();
            };
            std::vector<media::vdetect::Detection> kept;
            for (auto& d : fi.dr.detections)
                if (in_keep(d.label)) kept.push_back(std::move(d));
            fi.dr.detections = std::move(kept);
        }
    }

    // ── Optional annotated still (detection tasks) ─────────────────────────────
    if (fi.ok && !is_classify && !output_path.empty()) {
        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        const std::string tmp_path = (ec ? std::string(".") : media::path::path_to_utf8(tmp))
            + "/xblox_detect_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jpg";
        std::string save_err;
        if (pm::video::save_frame(frame, tmp_path, save_err)) {
            // Full task-aware fidelity (boxes / rotated quads / pose skeletons /
            // segmentation contours) via the shared rasterizer  -  not boxes-only.
            std::string draw_err;
            media::onnx::draw_detections_to_file(tmp_path, output_path,
                                                 fi.dr.detections,
                                                 fi.image_w, fi.image_h, draw_err);
            std::filesystem::remove(media::path::path_from_utf8(tmp_path), ec);
        }
    }

    // ── Optional burn-in video (annotated frames -> .mp4) ───────────────────────
    // The shared rasterizer draws the per-task geometry straight onto a copy of
    // the captured frame, which is then fed to the frame-fed encoder. The encoder
    // lives in the cached instance so a --loop run accumulates one file, finalized
    // when the instance is released. Streaming (Sunshine) would tap the same
    // annotated frames here in the future.
    if (fi.ok && !is_classify && inst) {
        const std::string record_path = io.str("recordPath");
        if (!record_path.empty()) {
#if defined(_WIN32)
            const bool is_screen = (!input_spec.empty() && input_spec.rfind("screen:", 0) == 0);
            if (is_screen) {
                // Smooth real-time recording: a background thread captures the
                // screen at recordFps; here we just publish the latest boxes.
                const int rec_fps = std::clamp(static_cast<int>(io.i64("recordFps", 30)), 1, 240);
                if (!inst->recorder) inst->recorder = std::make_unique<BurnInRecorder>();
                if (!inst->recorder->running())
                    inst->recorder->start(input_spec, record_path, rec_fps);
                inst->recorder->publish(fi.dr.detections);
            } else
#endif
            {
            const bool drawable = (frame.format == pm::video::PixelFormat::BGR24
                                || frame.format == pm::video::PixelFormat::RGB24);
            if (!drawable) {
                spdlog::warn("videoDetect: recordPath set but frame is {} (need BGR/RGB)  -  skipping burn-in",
                             static_cast<int>(frame.format));
            } else {
                if (!inst->encoder) inst->encoder = std::make_unique<pm::video::VideoEncoder>();
                std::string rec_err;
                if (!inst->rec_started) {
                    const int rec_fps = std::clamp(static_cast<int>(io.i64("recordFps", 30)), 1, 240);
                    if (inst->encoder->open(record_path, frame.width, frame.height, rec_fps, rec_err))
                        inst->rec_started = true;
                    else
                        spdlog::warn("videoDetect: record open failed: {}", rec_err);
                }
                if (inst->rec_started && inst->encoder->is_open()) {
                    pm::video::VideoFrame annotated = frame;  // draw on a copy
                    media::vdetect::Canvas cv;
                    cv.data     = annotated.data.data();
                    cv.width    = annotated.width;
                    cv.height   = annotated.height;
                    cv.stride   = annotated.stride > 0 ? annotated.stride : annotated.width * 3;
                    cv.channels = 3;
                    cv.bgr      = (annotated.format == pm::video::PixelFormat::BGR24);
                    media::vdetect::RenderOptions ropt;
                    ropt.line_width = std::max(2, annotated.width / 480);
                    ropt.font_scale = std::max(1, annotated.width / 640);
                    media::vdetect::draw_detections(cv, fi.dr.detections, 1.f, 1.f, ropt);
                    if (!inst->encoder->write(annotated, rec_err))
                        spdlog::warn("videoDetect: record write failed: {}", rec_err);
                }
            }
            }
        }
    }

    // ── Live visualization (Win32): persistent overlay + temporal smoothing ────
    // Detection tasks only (boxes). The overlay window + smoother live in the
    // cached instance, so across a --loop run the window opens once and the
    // smoother carries state between frames. Visualization never alters results.
#if defined(_WIN32)
    if (io.flag("visualize") && fi.ok && !is_classify && inst) {
        inst->smoother.opts.alpha     = static_cast<float>(io.f64("smoothAlpha", 0.35));
        inst->smoother.opts.max_age   = static_cast<int>(io.i64("smoothAge", 4));
        inst->smoother.opts.min_hits  = static_cast<int>(io.i64("smoothMinHits", 0));
        inst->smoother.opts.two_stage = io.flag("trackTwoStage", false);
        inst->smoother.opts.conf_high = static_cast<float>(io.f64("trackConfHigh", 0.5));
        inst->smoother.opts.center_fallback = io.flag("trackCenter", true);
        inst->smoother.opts.center_dist     = static_cast<float>(io.f64("trackCenterDist", 1.2));
        inst->smoother.opts.vel_blend       = static_cast<float>(io.f64("trackVelBlend", 0.7));
        inst->smoother.opts.kpt_min_score   = static_cast<float>(io.f64("kptMinScore", 0.30));
        if (!inst->viz_started) {
            pm::win::DetectOverlayOptions vopts;
            vopts.stats  = io.flag("visualizeStats", true);
            vopts.labels = io.flag("visualizeLabels", true);
            vopts.track_ids = io.flag("trackIds", false);
            const bool is_screen = !input_spec.empty() && input_spec.rfind("screen:", 0) == 0;
            const std::string mode = io.str("visualizeMode", "auto");
            const bool use_overlay = (mode == "overlay") || (mode == "auto" && is_screen);
            vopts.mode = use_overlay ? "overlay" : "preview";
            inst->overlay = std::make_unique<pm::win::DetectOverlay>();
            if (use_overlay)
                inst->overlay->show(overlay_rect_for_spec(input_spec), vopts);
            else
                inst->overlay->show_preview(960, 540, vopts);
            inst->viz_started = true;
        }
        if (inst->overlay && inst->overlay->is_open()) {
            std::vector<pm::win::DetectBox> raw;
            raw.reserve(fi.dr.detections.size());
            for (const auto& d : fi.dr.detections) {
                pm::win::DetectBox wb{ d.x, d.y, d.w, d.h, d.conf, d.cls, d.label };
                wb.oriented = d.oriented;
                wb.angle    = d.angle;
                wb.keypoints.reserve(d.keypoints.size());
                for (const auto& k : d.keypoints)
                    wb.keypoints.push_back({ k.x, k.y, k.score });
                wb.contour = d.contour;
                raw.push_back(std::move(wb));
            }
            const auto disp = inst->smoother.smooth(raw);
            inst->overlay->update(disp, ++inst->frame_idx, fi.infer_ms);
        }
    }
#endif

    // ── Build result JSON (shared serializer + envelope) ───────────────────────
    nlohmann::json result = media::vdetect::result_to_json(fi.dr);
    result["ok"]       = fi.ok;
    result["image_w"]  = fi.image_w;
    result["image_h"]  = fi.image_h;
    result["infer_ms"] = fi.infer_ms;
    result["error"]    = fi.error;
    result["provider"] = media::onnx::provider_name(provider);
    result["activeProvider"] = media::onnx::provider_name(active_provider);
    if (io.flag("gpuStats", false) && active_provider == media::onnx::OnnxProvider::CUDA) {
        gpu_sampler.sample();
        result["gpuMemory"] = gpu_memory_json(gpu_sampler);
    }

    if (!fi.ok) {
        io.error("videoDetect", "videoDetect: " + fi.error, 1)
          .result(Value::from_json(std::move(result)), ParamKind::json_value);
        return BlockResult::Error;
    }

    const size_t n_found = is_classify ? fi.dr.classes.size() : fi.dr.detections.size();
    io.ok("videoDetect",
          std::string(media::vdetect::task_to_string(task)) + ": "
              + std::to_string(n_found) + (is_classify ? " class(es) in " : " box(es) in ")
              + std::to_string(static_cast<int>(fi.infer_ms)) + " ms")
      .set("count", static_cast<long long>(n_found))
      .set("provider", media::onnx::provider_name(provider))
      .set("activeProvider", media::onnx::provider_name(active_provider))
      .result(Value::from_json(std::move(result)), ParamKind::json_value);

    // Container semantics: run children only when there's a result this pass
    // ("found something => ..."). PREVIOUS is the decode result set by .result().
    if (n_found > 0 && io.has_children("items"))
        io.run("items");
    return BlockResult::Ok;
}

#endif // FEATURE_VIDEO_DETECTION


} // namespace media::xblox::blocks
