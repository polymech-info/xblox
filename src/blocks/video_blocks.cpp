// XBlox video block registration.

#include "blocks/video_blocks.hpp"

#include "constants.hpp"
#include "core/batch_queue.hpp"
#include "core/settings_runtime.hpp"
#include "core/transform.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace media::xblox::blocks {

namespace {

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

} // namespace

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
BlockResult video_list_devices_block(BlockIO& io);
BlockResult video_capture_block(BlockIO& io);
BlockResult video_source_block(BlockIO& io);
#if defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
BlockResult video_stream_block(BlockIO& io);
#endif
#endif

#if defined(FEATURE_VIDEO_DETECTION) && FEATURE_VIDEO_DETECTION
BlockResult video_list_screens_block(BlockIO& io);
BlockResult video_screen_capture_block(BlockIO& io);
BlockResult video_detect_block(BlockIO& io);
#endif

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO \
 && defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM \
 && defined(FEATURE_COMMAND_CREATE) && FEATURE_COMMAND_CREATE
BlockResult video_create_block(BlockIO& io)
{
    media::TransformOptions opts;
    opts.provider.clear();
    opts.model.clear();
    opts.provider = io.str("provider");
    opts.model = io.str("model");
    media::llm::apply_chat_video_defaults_for_tool_options(opts.provider, opts.model, nlohmann::json::object());
    opts.api_key = io.str("apiKey");
    opts.base_url = io.str("baseUrl");
    opts.prompt = io.str("prompt");
    opts.aspect_ratio = io.str("aspectRatio");
    opts.video_resolution = io.str("resolution");
    opts.video_duration = static_cast<int>(io.i64("duration", 0));
    opts.video_generate_audio = io.flag("generateAudio", false);
    opts.video_first_frame = io.str("startFrame");
    if (opts.video_first_frame.empty())
        opts.video_first_frame = io.str("image");
    opts.video_last_frame = io.str("lastFrame");
    {
        const std::vector<std::string> inputs = strings_from_param(io, "input");
        if (inputs.size() > 2) {
            io.error("videoCreate",
                     "videoCreate: at most two input stills (first frame, last frame); extra style images use references",
                     1);
            return BlockResult::Error;
        }
        if (opts.video_first_frame.empty() && !inputs.empty())
            opts.video_first_frame = inputs[0];
        if (opts.video_last_frame.empty() && inputs.size() > 1)
            opts.video_last_frame = inputs[1];
    }
    opts.reference_images = strings_from_param(io, "references");
    {
        const nlohmann::json& po = io.json("providerOptions");
        if (po.is_object() && !po.empty())
            opts.provider_options = po;
    }
    if (!opts.provider.empty())
        media::runtime_settings::merge_provider_credentials(opts.provider, false, opts.api_key, opts.base_url);

    if (opts.provider.empty() || opts.model.empty()) {
        io.error("videoCreate", "videoCreate: provider/model required (set block fields or App Settings)", 1)
          .set("provider", opts.provider)
          .set("model", opts.model);
        return BlockResult::Error;
    }
    if (opts.prompt.empty()) {
        io.error("videoCreate", "videoCreate: prompt is required", 1);
        return BlockResult::Error;
    }
    if (io.cancelled())
        return BlockResult::Fatal;

    media::BatchControl batch;
    auto progress = [&](const std::string& msg) { io.info("videoCreate: " + msg); };
    const media::TransformResult result =
        media::create_video(io.str("outputPath"), opts, progress, &batch);

    if (!result.ok) {
        if (io.cancelled() || result.error.find("cancelled") != std::string::npos) {
            batch.request_cancel();
            return BlockResult::Fatal;
        }
        io.error("videoCreate", "videoCreate: " + result.error, 1);
        return BlockResult::Error;
    }

    nlohmann::json row{
        {"kind", "video"},
        {"path", result.output_path},
    };
    if (!result.ai_text.empty())
        row["jobId"] = result.ai_text;
    if (!result.replicate_prediction_web_url.empty())
        row["predictionUrl"] = result.replicate_prediction_web_url;

    io.ok("videoCreate", "created video: " + result.output_path)
      .set("path", result.output_path)
      .set("provider", opts.provider)
      .set("model", opts.model)
      .payload(row)
      .result(result.output_path, ParamKind::video_path);
    return BlockResult::Ok;
}
#endif

// ── registration ──────────────────────────────────────────────────────────────

void register_video_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    using J = nlohmann::json;

    registry["videoListDevices"] = bdb(
        "videoListDevices", video_list_devices_block,
        "Video Devices", "Video",
        "List all available video capture (camera) devices.",
        {{"kind","videoListDevices"}, {"storeAs","videoDevices"}})
        .params({ pd_store_as() })
        .primary_output(ParamKind::json_value, "Video devices")
        .exec({false, false, false});

#if defined(FEATURE_COMMAND_LLM) && FEATURE_COMMAND_LLM \
 && defined(FEATURE_COMMAND_CREATE) && FEATURE_COMMAND_CREATE
    registry["videoCreate"] = bdb(
        "videoCreate", video_create_block,
        "Create Video", "Video",
        "Generate a video from a prompt using the same core create_video API as `video create`. "
        "input stills are keyframes (first / last frame); references are style/brand images. "
        "Tanit aliases (video-fast / video-deep) use LiteLLM POST /v1/videos.",
        {{"kind","videoCreate"}, {"prompt",""}, {"outputPath",""}, {"provider",""}, {"model",""},
         {"duration",0}, {"resolution","720p"}, {"aspectRatio","16:9"}, {"generateAudio",false},
         {"input",""}, {"startFrame",""}, {"lastFrame",""}, {"references", J::array()},
         {"apiKey",""}, {"baseUrl",""},
         {"providerOptions", J::object()}, {"storeAs","videoPath"}})
        .params({
            pd_prompt("prompt").in().req()
                .desc("Motion / camera / lighting prompt."),
            pd_image("input").in().prev().globs().grp("source").dflt("")
                .desc("Keyframe stills. First path = start frame, second = end frame. "
                      "Uses PREVIOUS when unset. Extra style images go in references."),
            pd_image("startFrame").grp("source").lbl("Start frame").dflt("")
                .desc("Optional start-frame still (alias of a single input)."),
            pd_image("lastFrame").grp("source").lbl("Last frame").dflt("")
                .desc("Optional last-frame still."),
            pd_args("references").grp("references").dflt(nlohmann::json::array())
                .ui({{"picks_path", true}})
                .desc("Style / brand reference images (not keyframes). Sent as OpenRouter input_references."),
            pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
                .desc("Output path (.mp4). Empty = core default naming."),
            pd_provider().grp("model").dflt("")
                .ui({{"options_path", "providers.image"}})
                .desc("Video generation provider. Empty = App Settings video provider."),
            pd_model().grp("model").dflt("")
                .desc("Video model (video-fast, video-deep, or a Replicate slug). Empty = App Settings video model."),
            pd_json("providerOptions").adv().grp("model").lbl("Model options")
                .dflt(J::object())
                .desc("Extra provider fields (personGeneration, negativePrompt, …)."),
            pd_int("duration").grp("request").dflt(0).range(0, 8)
                .desc("Seconds. Veo accepts 4 / 6 / 8. 0 = model default (4 fast / 8 deep)."),
            pd_enum("resolution", {"720p", "1080p", "4K"}).grp("request").dflt("720p")
                .desc("Output resolution. 4K is video-deep only."),
            pd_enum("aspectRatio", {"16:9", "9:16"}).grp("request").dflt("16:9")
                .desc("Output aspect ratio."),
            pd_bool("generateAudio").grp("request").dflt(false)
                .desc("Request native audio (higher cost on OpenRouter Veo)."),
            pd_api_key().grp("auth").dflt(""),
            pd_str("baseUrl").adv().grp("auth").dflt("")
                .desc("Provider base URL override. Empty = App Settings."),
            pd_store_as().dflt("videoPath"),
        })
        .primary_output(ParamKind::video_path, "Created video")
        .exec({true, true, true});
#endif

    std::vector<ParamDef> capture_params = {
        pd_enum("action", {"Still", "Record", "Start", "Update", "Status", "Stop"}).grp("mode").dflt("Still")
            .desc("Still = one frame; Record = capture MP4 until duration/cancel; Start keeps recording under instance; Update changes its viewport; Status reports it; Stop finalizes it."),
        pd_video_input("input").in().lbl("Source").dflt("")
            .desc("Camera device name, screen/window spec (screen:current / screen for the monitor of the launching window, screen:N, screen:N:wintitle=GLOB, screen:N:hwnd=ID), image (.png/.jpg), or video file (.mp4). Empty = preferred camera."),
        pd_dev("device").adv().dflt("").hide()
            .desc("Legacy camera device alias. Prefer input."),
        pd_str("instance").grp("recording").dflt("")
            .desc("Recording instance name for Start/Stop. Use the same name to stop and finalize the recording."),
        pd_outpath("outputPath").grp("output_file").lbl("Output file").dflt("")
            .desc("Output path (.jpg/.png for Still, .mp4 for Record/Start). Empty = auto temp file."
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
                  " With virtualOut, empty means virtual cam only (no MP4)."
#endif
            ),
        pd_video_fps("fps").grp("recording").dflt(30).range(1, 240)
            .desc("Target recording frame rate."),
        pd_video_bitrate_kbps("bitrateKbps").adv().grp("recording").dflt(0).range(0, 100000)
            .desc("H.264 bitrate in Kbps for fast (WGC) recording. 0 = automatic from resolution and fps. Ignored on compat fallback."),
        pd_enum("captureEngine", {"auto", "fast", "compat"}).adv().grp("recording").dflt("auto")
            .desc("auto = prefer fast Windows WGC; fast = require it (zoom/follow/bitrate); compat = portable frame-pump fallback (no zoom/follow/bitrate)."),
        pd_enum("encoder", {"auto", "nvenc", "mediaFoundation"}).adv().grp("recording").dflt("auto")
            .desc("Fast Windows capture encoder. auto and nvenc try the optional NVIDIA plugin, then Media Foundation when pm-video-nvenc.dll is missing or will not load. mediaFoundation uses the Windows encoder. A missing NVENC plugin does not fail the block."),
        pd_ms("durationMs").grp("recording").dflt(0).range(0, 86400000)
            .desc("Record action only: stop after this many ms. 0 = record until cancellation or stopKey. Start ignores this and runs until Stop or stopKey."),
        pd_bool("includeCursor").grp("capture_options").dflt(true)
            .desc("Window fast recording: include the mouse cursor in the captured MP4 when supported."),
        pd_shortcut("pauseKey").grp("recording").dflt("")
            .desc("Optional global shortcut that toggles recording pause/resume, e.g. F9 or Ctrl+Shift+P. Empty disables it."),
        pd_shortcut("stopKey").grp("recording").dflt("")
            .desc("Optional global shortcut that stops and finalizes recording, e.g. F10 or Ctrl+Shift+S. Empty disables it."),
        pd_enum("aspectRatio", {
                "source", "16:9", "9:16", "4:3", "3:4", "1:1", "21:9", "3:2", "2:3", "custom"
            }).grp("viewport").dflt("source")
            .desc("Output and zoom-viewport aspect ratio. Kept through interactive zoom and cursor follow. "
                  "source = capture region; custom uses aspectCustomW/H. Pair with sizeMode fill for a hard crop."),
        pd_int("aspectCustomW").grp("viewport").lbl("Custom W").dflt(16).range(1, 100)
            .ui({{"visible_when", {{"aspectRatio", "custom"}}}})
            .desc("Custom aspect ratio width part (e.g. 16 in 16:10). Only used when aspectRatio is custom."),
        pd_int("aspectCustomH").grp("viewport").lbl("Custom H").dflt(9).range(1, 100)
            .ui({{"visible_when", {{"aspectRatio", "custom"}}}})
            .desc("Custom aspect ratio height part (e.g. 10 in 16:10). Only used when aspectRatio is custom."),
        pd_enum("sizeMode", {"native", "pad", "fit", "fill"}).grp("viewport").dflt("native")
            .desc("Fixed-canvas layout: native/pad preserve 1:1 pixels when possible, fit shows the full region, fill crops to cover."),
        pd_float("zoom").grp("viewport").dflt(1.0).range(1.0, 32.0)
            .desc("Viewport zoom multiplier. Update changes zoom without restarting capture or encoding."),
        pd_enum("follow", {"none", "anchor", "cursor"}).grp("viewport").dflt("none")
            .desc("Keep the initial picked anchor fixed, or smoothly follow the Windows cursor."),
        pd_float("followSpeed").adv().grp("viewport").dflt(0.2).range(0.01, 1.0)
            .desc("Per-update cursor-follow interpolation factor."),
        pd_int("followDeadzone").adv().grp("viewport").dflt(3).range(0, 1000)
            .desc("Source pixels the cursor may move before the viewport follows."),
        pd_ms("transitionMs").grp("viewport").dflt(300).range(0, 10000)
            .desc("Smooth zoom transition duration."),
        pd_ms("updateIntervalMs").adv().grp("viewport").dflt(16).range(1, 1000)
            .desc("Interactive/follow tracking interval."),
        pd_float("focusX").adv().grp("viewport").dflt(-1.0).range(-1.0, 1.0)
            .desc("Optional normalized horizontal focus point; -1 keeps the picked anchor."),
        pd_float("focusY").adv().grp("viewport").dflt(-1.0).range(-1.0, 1.0)
            .desc("Optional normalized vertical focus point; -1 keeps the picked anchor."),
        pd_bool("interactiveZoom").grp("viewport").dflt(false)
            .desc("While recording, Shift+mouse-wheel changes zoom around the cursor."),
        pd_float("zoomStep").adv().grp("viewport").dflt(0.15).range(0.01, 2.0)
            .desc("Zoom multiplier increment per Shift+wheel notch."),
        pd_float("zoomMin").adv().grp("viewport").dflt(1.0).range(1.0, 32.0)
            .desc("Minimum interactive zoom."),
        pd_float("zoomMax").adv().grp("viewport").dflt(4.0).range(1.0, 32.0)
            .desc("Maximum interactive zoom."),
        pd_bool("consumeZoomInput").adv().grp("viewport").dflt(true)
            .desc("Prevent Shift+wheel from also reaching the application under the cursor."),
        pd_bool("showZoomStatus").grp("viewport").dflt(true)
            .desc("Show a click-through recording HUD and active viewport while zoom changes."),
        pd_enum("hudCapture", {"auto", "exclude", "visible"}).adv().grp("viewport").dflt("auto")
            .desc("Overlay capture policy. visible includes the recording zoom HUD/frame, "
                  "a live picker HUD/highlight, and block status HUDs in WGC/BitBlt captures; "
                  "auto and exclude keep them out. Status HUDs stay visible to Windows Snap "
                  "when we are not capturing."),
#if PM_HAS_AUDIO_IO
        pd_enum("audioSource", {"none", "mic", "desktop", "mix"}).grp("audio").dflt("none")
            .desc("Mux audio into MP4: none, mic, desktop (loopback), or mix. When enabled, audio is 48 kHz stereo AAC (no separate rate/channel params)."),
        pd_audio_input_device("audioDevice").adv().grp("audio").lbl("Microphone").dflt("")
            .desc("Mic device for audioSource mic/mix. Empty = app setting, then system default."),
        pd_str("desktopDevice").adv().grp("audio").lbl("Desktop audio").dflt("")
            .desc("Loopback device substring for audioSource desktop/mix. Empty = default render device."),
        pd_float("micGain").adv().grp("audio").dflt(1.0).range(0.0, 4.0)
            .desc("Mic level multiplier (linear) for mix mode."),
        pd_float("desktopGain").adv().grp("audio").dflt(1.0).range(0.0, 4.0)
            .desc("Desktop/loopback level multiplier for desktop/mix."),
#endif
        pd_int("width").adv().grp("capture_options").dflt(0).range(0, 7680)
            .desc("Preferred width in pixels. 0 = device default."),
        pd_int("height").adv().grp("capture_options").dflt(0).range(0, 4320)
            .desc("Preferred height in pixels. 0 = device default."),
        pd_ms("timeoutMs").adv().grp("capture_options").dflt(5000).range(100, 30000)
            .desc("Max ms to wait for first frame."),
        pd_enum("hudMode", {"off", "taskbar", "overlay", "both"}).grp("hud").dflt("overlay")
            .desc("Status HUD while Still / Record / Start is running. taskbar = app taskbar button. overlay = movable pill over the clock. both = combine. off = none. Separate from showZoomStatus / hudCapture."),
        pd_bool("hud").grp("hud").dflt(false)
            .desc("Master switch for hudMode. Stop on the overlay or taskbar ends Record early, or finalizes a Start session. Default off."),
        pd_store_as().dflt("capturePath"),
    };
    append_key_overlay_params(capture_params);
    append_cursor_fx_params(capture_params);
    append_camera_overlay_params(capture_params);
#if PM_HAS_AUDIO_IO
    append_audio_polish_params(capture_params);
#endif
    append_idle_pause_params(capture_params);
#if defined(FEATURE_STT) && FEATURE_STT
    append_caption_params(capture_params);
#endif
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    append_beauty_params(capture_params);
#endif
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
    append_vcam_params(capture_params);
#endif
    J capture_defaults = {{"kind","videoCapture"}, {"action","Still"}, {"input",""}, {"device",""},
                                  {"instance",""}, {"outputPath",""}, {"fps",30}, {"bitrateKbps",0},
                                  {"captureEngine","auto"}, {"encoder","auto"}, {"durationMs",0},
                                  {"includeCursor",true}, {"pauseKey",""}, {"stopKey",""},
                                  {"aspectRatio","source"}, {"aspectCustomW",16}, {"aspectCustomH",9},
                                  {"sizeMode","native"}, {"zoom",1.0},
                                  {"follow","none"}, {"followSpeed",0.2}, {"followDeadzone",3},
                                  {"transitionMs",300}, {"updateIntervalMs",16},
                                  {"focusX",-1.0}, {"focusY",-1.0},
                                  {"interactiveZoom",false}, {"zoomStep",0.15},
                                  {"zoomMin",1.0}, {"zoomMax",4.0},
                                  {"consumeZoomInput",true}, {"showZoomStatus",true},
                                  {"hudCapture","auto"},
#if PM_HAS_AUDIO_IO
                                  {"audioSource","none"}, {"audioDevice",""}, {"desktopDevice",""},
                                  {"micGain",1.0}, {"desktopGain",1.0},
#endif
                                  {"width",0}, {"height",0}, {"timeoutMs",5000},
                                  {"hud",false}, {"hudMode","overlay"},
                                  {"storeAs","capturePath"}};
    merge_key_overlay_defaults(capture_defaults);
    merge_cursor_fx_defaults(capture_defaults);
    merge_camera_overlay_defaults(capture_defaults);
#if PM_HAS_AUDIO_IO
    merge_audio_polish_defaults(capture_defaults);
#endif
    merge_idle_pause_defaults(capture_defaults);
#if defined(FEATURE_STT) && FEATURE_STT
    merge_caption_defaults(capture_defaults);
#endif
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    merge_beauty_defaults(capture_defaults);
#endif
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
    merge_vcam_defaults(capture_defaults);
#endif

    registry["videoCapture"] = bdb(
        "videoCapture", video_capture_block,
        "Capture Image / Video", "Video",
        "Capture a still image or record a camera, screen, or window source to H.264 MP4. "
        "HQ zoom/follow/interactive zoom require Windows fast (WGC) capture. "
        "Optional audio mux is 48 kHz stereo AAC. "
        "Optional skin / lips / teeth / faceGain / warmth / undereye / redness / "
        "eyeSharpen / mirror / blur / background apply to the full-frame camera, "
        "or to the PiP webcam when cameraOverlay is on (screen stays on fast WGC). "
        "preview"
#if defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM
        " and virtualOut"
#endif
        " use the CPU path as `video record`. "
        "Optional hud / hudMode shows the Capture overlay or taskbar while Still, Record, or Start is running. "
        "Stores the output path in PREVIOUS / storeAs.",
        capture_defaults)
        .params(capture_params)
        .dynamic_primary_output(
            ParamKind::image_path,
            {ParamKind::image_path, ParamKind::video_path},
            "Captured media",
            true)
        .exec({false, true, true});

    registry["videoStillCapture"] = bdb(
        "videoStillCapture", video_capture_block,
        "", "Video",
        "Alias for videoCapture (still frame capture).",
        J::object({{"kind","videoStillCapture"}, {"action","Still"}, {"input",""}, {"device",""}, {"outputPath",""},
                    {"width",0}, {"height",0}, {"timeoutMs",5000}, {"storeAs","imagePath"}}))
        .params(capture_params)
        .primary_output(ParamKind::image_path, "Captured image")
        .exec({false, true, true})
        .hidden();

    // videoSource  -  flat-pipe head: webcam / screen / image / video file -> frames.
    const std::vector<ParamDef> source_params = {
        pd_screen_input("input").in().lbl("Source").dflt("")
            .desc("Camera name, screen/window spec (screen:current / screen for the monitor of the launching window, screen:N, screen:N:wintitle=GLOB, screen:N:hwnd=ID), image (.png/.jpg), or video file (.mp4). Empty = preferred camera."),
        pd_enum("emit", {"both", "file", "handle"}).grp("one_shot_output").dflt("both")
            .desc("One-shot mode only (no child items): both = write file + cache frame; "
                  "file = write only; handle = in-memory only (mem:// handle, no file)."),
        pd_outpath("outputPath").adv().grp("one_shot_output").lbl("Output file").dflt("")
            .desc("One-shot output file path (.jpg/.png) for emit=file/both. Empty = auto temp file."),
        pd_int("frames").grp("pump_limits").dflt(0).range(0, 1000000)
            .desc("Max frames to pump (0 = unbounded -> video files run to EOF; "
                  "live sources run until durationMs/cancel). Use N for a fixed-length grab."),
        pd_ms("durationMs").adv().grp("pump_limits").dflt(0).range(0, 86400000)
            .desc("Live sources only: stop after this wall-clock budget (0 = no cap)."),
        pd_int("progressEvery").adv().grp("pump_limits").dflt(60).range(0, 100000)
            .desc("Pump mode: emit a progress event every N frames (child events are muted). "
                  "0 = only the final summary."),
        pd_int("fps").adv().grp("capture_options").dflt(0).range(0, 240)
            .desc("Preferred capture rate for live sources. 0 = source default."),
        pd_int("width").adv().grp("capture_options").dflt(0).range(0, 7680)
            .desc("Preferred width in pixels (live sources). 0 = source default."),
        pd_int("height").adv().grp("capture_options").dflt(0).range(0, 4320)
            .desc("Preferred height in pixels (live sources). 0 = source default."),
        pd_ms("timeoutMs").adv().grp("capture_options").dflt(5000).range(100, 30000)
            .desc("Max ms to wait for the first frame of a live source."),
        pd_store_as().dflt("imagePath"),
    };
    registry["videoSource"] = bdb(
        "videoSource", video_source_block,
        "Video Source", "Video",
        "Pump frames from a webcam, screen/window, image, or video file. With child "
        "items it runs them once per frame (PREVIOUS = frame handle)  -  a flat pipe that "
        "transcodes a clip with no for/while and no storeAs. With no items it is a "
        "one-shot grabber emitting a handle/path in PREVIOUS / storeAs.",
        {{"kind","videoSource"}, {"input",""}, {"emit","both"}, {"outputPath",""},
         {"frames",0}, {"durationMs",0}, {"fps",0}, {"progressEvery",60},
         {"width",0}, {"height",0}, {"timeoutMs",5000}, {"storeAs","imagePath"},
         {"items", nlohmann::json::array()}})
        .params(source_params)
        .dynamic_primary_output(
            ParamKind::image_path,
            {ParamKind::image_path, ParamKind::json_value},
            "Source result")
        .output(
            "videoSourceFps", ParamKind::json_value, "Source FPS",
            "videoSourceFps", true)
        // Container: the handler owns the per-frame loop and runs `items` itself.
        .children({{"items", "Pipeline", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});

#if defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
    const std::vector<ParamDef> stream_params = {
        pd_enum("action", {"Start", "Stop", "Status"}).grp("server_lifecycle").dflt("Start")
            .desc("Start a named remote video stream server, Stop it, or report Status."),
        pd_screen_input("input").in().lbl("Source").dflt("screen:0")
            .desc("Camera or screen/window source for Start. Examples: screen:current, screen:0, screen:0:hwnd=..., screen:0:wintitle=...:rrect=x,y,w,h."),
        pd_str("instance").grp("server_lifecycle").dflt("default")
            .desc("Named stream instance. Use the same name for Stop/Status."),
        pd_str("bind").adv().grp("network").dflt("127.0.0.1")
            .desc("Network interface to listen on. 127.0.0.1 is local only."),
        pd_int("port").adv().grp("network").dflt(8787).range(1, 65535)
            .desc("TCP port for the stream server. Use a fixed port so Status can report a stable URL."),
        pd_int("fps").adv().grp("quality").dflt(30).range(1, 240)
            .desc("Target capture frame rate."),
        pd_int("bitrateKbps").adv().grp("quality").dflt(8000).range(1, 100000)
            .desc("Future WebRTC/video encoder bitrate hint."),
        pd_int("mjpegQuality").adv().grp("quality").dflt(85).range(1, 100)
            .desc("JPEG quality for the MJPEG fallback preview."),
        pd_bool("includeCursor").grp("capture_options").dflt(true)
            .desc("Include the cursor in screen capture when supported."),
        pd_str("token").adv().grp("access").dflt("")
            .desc("Remote access token. Empty generates one for Start."),
        pd_str("encoder").adv().grp("quality").dflt("auto")
            .desc("Future encoder selector."),
        pd_bool("open").grp("server_lifecycle").dflt(false)
            .desc("Ask the stream server to open the local browser after starting."),
        pd_store_as().dflt("stream"),
    };
    registry["videoStream"] = bdb(
        "videoStream", video_stream_block,
        "Remote Stream", "Video",
        "Start, stop, or inspect a named remote video stream server for the mobile web remote. "
        "The current backend launches the same video stream server used by the CLI.",
        {{"kind","videoStream"}, {"action","Start"}, {"input","screen:0"}, {"instance","default"},
         {"bind","127.0.0.1"}, {"port",8787}, {"fps",30}, {"bitrateKbps",8000},
         {"mjpegQuality",85}, {"includeCursor",true}, {"token",""}, {"encoder","auto"},
         {"open",false}, {"storeAs","stream"}})
        .params(stream_params)
        .primary_output(ParamKind::json_value, "Stream status", true)
        .exec({false, true, true});
#endif
#else
    (void)registry;
#endif

#if defined(FEATURE_VIDEO_DETECTION) && FEATURE_VIDEO_DETECTION

    registry["videoListScreens"] = bdb(
        "videoListScreens", video_list_screens_block,
        "List Screens", "Video",
        "Enumerate physical monitors and visible windows. Stores {monitors:[...], windows:[...]} in PREVIOUS / storeAs.",
        {{"kind","videoListScreens"}, {"storeAs","screens"}})
        .params({
            pd_store_as().dflt("screens")
                .desc("Variable for PREVIOUS / downstream blocks. Default: screens."),
        })
        .primary_output(ParamKind::json_value, "Screens")
        .exec({false, false, false});

    registry["videoScreenCapture"] = bdb(
        "videoScreenCapture", video_screen_capture_block,
        "Screen Capture", "Video",
        "Capture a single frame from a screen or window via GDI BitBlt. "
        "input: \"screen:0\", \"screen:0:wintitle*\". Stores output path in PREVIOUS / storeAs.",
        {{"kind","videoScreenCapture"}, {"input","screen:0"}, {"outputPath",""}, {"storeAs","imagePath"}})
        .params({
            pd_screen_input("input").in().dflt("screen:0")
                .desc("Screen spec. Default: screen:0 (primary monitor). Examples: screen:current, screen:N, screen:N:wintitle=GLOB."),
            pd_outpath("outputPath").adv().dflt("")
                .desc("Output image path (.jpg/.png). Default: empty → temp file in system temp directory."),
            pd_store_as().dflt("imagePath")
                .desc("Variable for PREVIOUS / downstream blocks. Default: imagePath."),
        })
        .primary_output(ParamKind::image_path, "Captured screen")
        .exec({false, false, true});

    registry["videoDetect"] = bdb(
        "videoDetect", video_detect_block,
        "Detect Objects", "Video",
        "Capture one frame (webcam or screen) and run a YOLO model. task = "
        "detect/obb/pose -> {boxes:[{x,y,w,h,conf,cls,label,angle?,keypoints?}]}; "
        "classify -> {classes:[{cls,score,label}]}. Stores {ok, image_w, image_h, "
        "infer_ms, task, ...} in PREVIOUS / storeAs.",
        {{"kind","videoDetect"}, {"model",""}, {"input",""}, {"task","auto"}, {"labels",""}, {"classes",""},
         {"filterClasses",""}, {"conf",0.25}, {"nms",0.45}, {"topK",5}, {"inputSize",0}, {"threads",4},
        {"provider","gpu"}, {"gpuStats",false}, {"instance",""}, {"visualize",false}, {"visualizeMode","auto"},
         {"visualizeStats",true}, {"visualizeLabels",true}, {"smoothAlpha",0.35}, {"smoothAge",4},
         {"smoothMinHits",0}, {"trackIds",false}, {"trackTwoStage",false}, {"trackConfHigh",0.5},
         {"trackCenter",true}, {"trackCenterDist",1.2}, {"trackVelBlend",0.7}, {"kptMinScore",0.30},
         {"timeoutMs",5000}, {"outputPath",""}, {"recordPath",""}, {"recordFps",30},
         {"storeAs","detections"},
         {"items", nlohmann::json::array()}})
        .params({
            pd_file("model").in().grp("source_model").req()
                .desc("YOLO ONNX model path (required). Examples: ${MODELS_DIR}/yolov8n.onnx, "
                      "yolo11n-obb.onnx, yolo11n-cls.onnx, yolo11n-pose.onnx under model roots."),
            pd_screen_input("input").in().grp("source_model").dflt("")
                .desc("Webcam device name or screen spec (screen:0, screen:0:wintitle*). "
                      "Default: empty → App Settings video_capture_device, then default webcam."),
            pd_enum("task", {"auto", "detect", "classify", "obb", "pose", "segment"}).grp("source_model").dflt("auto")
                .desc("YOLO head to decode. Default: auto (infer detect/classify/segment from model). "
                      "obb and pose must be selected explicitly."),
            pd_file("labels").adv().grp("labels_filter").dflt("")
                .desc("Optional YOLO labels YAML. Default: empty → classes CSV, then task default "
                      "(obb: DOTA-15, classify: imagenet-classes.txt beside model, else COCO-80)."),
            pd_str("classes").adv().grp("labels_filter").dflt("")
                .desc("Comma-separated class names (index order). Default: empty. Ignored when labels is set."),
            pd_str("filterClasses").adv().grp("labels_filter").dflt("")
                .desc("Comma-separated class names to keep. Default: empty → keep all detections."),
            pd_float("conf").adv().grp("thresholds").dflt(0.25).range(0, 1)
                .desc("Detection confidence threshold (0–1). Default: 0.25."),
            pd_float("nms").adv().grp("thresholds").dflt(0.45).range(0, 1)
                .desc("NMS IoU threshold for detect/obb/pose. Default: 0.45."),
            pd_int("topK").adv().grp("thresholds").dflt(5).range(1, 100)
                .desc("classify task: top predictions to return. Default: 5."),
            pd_int("inputSize").adv().grp("runtime").dflt(0).range(0, 4096)
                .desc("Model input size (px). Default: 0 → from metadata (detect 640, classify 224, obb 1024)."),
            pd_int("threads").adv().grp("runtime").dflt(4).range(1, 64)
                .desc("ONNX Runtime intra-op CPU threads. Default: 4."),
            pd_enum("provider", {"gpu", "auto", "cpu", "cuda"}).adv().grp("runtime").dflt("gpu")
                .desc("ONNX execution provider. Default: gpu (CUDA required). auto = CUDA then CPU fallback."),
            pd_bool("gpuStats").adv().grp("runtime").dflt(false)
                .desc("Attach NVIDIA VRAM telemetry when activeProvider=cuda. Default: false."),
            pd_str("instance").adv().grp("runtime").dflt("")
                .desc("Named cached model/session instance across loop passes. Default: empty → hash by args."),
            pd_bool("visualize").adv().grp("visualization").dflt(false)
                .desc("Live detection window (Win32). Default: false. Persists across loop passes."),
            pd_enum("visualizeMode", {"auto", "overlay", "preview"}).adv().grp("visualization").dflt("auto")
                .desc("Visualization window mode. Default: auto (overlay for screen:*, preview otherwise)."),
            pd_bool("visualizeStats").adv().grp("visualization").dflt(true)
                .desc("Draw FPS / infer-time / class-count overlay text. Default: true."),
            pd_bool("visualizeLabels").adv().grp("visualization").dflt(true)
                .desc("Draw label + confidence on boxes. Default: true."),
            pd_float("smoothAlpha").adv().grp("smoothing").dflt(0.35).range(0, 1)
                .desc("Temporal EMA for overlay boxes (viz only). Default: 0.35."),
            pd_int("smoothAge").adv().grp("smoothing").dflt(4).range(0, 60)
                .desc("Overlay box persistence after disappearance (viz only). Default: 4 frames."),
            pd_int("smoothMinHits").adv().grp("smoothing").dflt(0).range(0, 60)
                .desc("Frames before a track is drawn (viz only). Default: 0."),
            pd_bool("trackIds").adv().grp("tracking").dflt(false)
                .desc("Stable track ids and per-track colors (viz only). Default: false."),
            pd_bool("trackTwoStage").adv().grp("tracking").dflt(false)
                .desc("ByteTrack-style high/low association (viz only). Default: false."),
            pd_float("trackConfHigh").adv().grp("tracking").dflt(0.5).range(0, 1)
                .desc("High-confidence split for trackTwoStage (viz only). Default: 0.5."),
            pd_bool("trackCenter").adv().grp("tracking").dflt(true)
                .desc("Center-distance fallback matching (viz only). Default: true."),
            pd_float("trackCenterDist").adv().grp("tracking").dflt(1.2).range(0, 10)
                .desc("Center-distance gate as multiple of mean box side (viz only). Default: 1.2."),
            pd_float("trackVelBlend").adv().grp("tracking").dflt(0.7).range(0, 1)
                .desc("Velocity EMA during dropouts (viz only). Default: 0.7."),
            pd_float("kptMinScore").adv().grp("pose").dflt(0.30).range(0, 1)
                .desc("Min pose keypoint score to update a joint (viz only). Default: 0.30."),
            pd_ms("timeoutMs").adv().grp("output_recording").dflt(5000).range(100, 30000)
                .desc("Webcam capture timeout (ms). Default: 5000. Ignored for screen input."),
            pd_outpath("outputPath").adv().grp("output_recording").dflt("")
                .desc("Optional annotated still image (.jpg/.png). Default: empty → do not save."),
            pd_outpath("recordPath").adv().grp("output_recording").dflt("")
                .desc("Optional annotated burn-in video (.mp4). Default: empty → do not record."),
            pd_int("recordFps").adv().grp("output_recording").dflt(30).range(1, 240)
                .desc("Burn-in video frame rate when recordPath is set. Default: 30."),
            pd_store_as().dflt("detections")
                .desc("Variable for PREVIOUS / downstream blocks. Default: detections."),
        })
        // Child pipeline: the handler runs these itself (is_container) and only
        // when ≥1 box survived the filter, so "found boxes => parse/log" composes
        // and is skipped on no-match passes. PREVIOUS = the detection result.
        .primary_output(ParamKind::json_value, "Detection result")
        .children({{"items", "Then", "body", {"*"}, 0, -1}})
        .exec({false, true, true, true});
#endif
}


} // namespace media::xblox::blocks
