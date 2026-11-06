// XBlox video/image filter blocks  -  videoGrayscale, videoColor, videoCrop,
// videoResize, pictureOut.
//
// Gated on FEATURE_VIDEO. These are the first "buffer pipeline" blocks: each
// resolves a PREVIOUS frame handle (zero-copy) or an image path, works on a
// tightly-packed BGR24 buffer (our own pixel ops  -  no OpenCV), and emits a new
// handle/path via the run-scoped FrameStore (see frame_store.hpp).
//
//   videoSource -> videoGrayscale -> videoResize -> pictureOut
//                 └ in-memory mem:// handles ───┘  └ encodes to disk
//
// PREVIOUS / storeAs contract:
//   • videoGrayscale / videoColor / videoCrop / videoResize
//        -> frame handle ("mem://N") or output path (image_path)
//   • pictureOut -> output file path (image_path)

#include "blocks/video_filter_blocks.hpp"

#include "logging/logger.h"

#include <string>

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "blocks/frame_store.hpp"
#include "core/video.hpp"
#include "core/video_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#endif // FEATURE_VIDEO

namespace media::xblox::blocks {

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

namespace {

inline std::uint8_t clamp_u8(float v) {
    if (v <= 0.f)   return 0;
    if (v >= 255.f) return 255;
    return static_cast<std::uint8_t>(v + 0.5f);
}

// BGR24 luma (Rec.601): keep channel order B,G,R.
inline float luma_bgr(std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    return 0.114f * b + 0.587f * g + 0.299f * r;
}

// Shared filter spine: resolve -> normalize to BGR24 -> op(frame) -> emit.
// `op` may mutate the frame in place or replace it (crop/resize change dims).
BlockResult run_filter(BlockIO& io, const char* kind,
                       const std::function<bool(pm::video::VideoFrame&, std::string&)>& op)
{
    const std::string input = io.str("input");
    std::string       err;

    pm::video::VideoFrame raw;
    if (!resolve_frame_consume(io, input, raw, err)) {
        io.error(kind, std::string(kind) + ": " + err, 1).set("input", input);
        return BlockResult::Error;
    }

    // Fast path: a packed-BGR24 frame (the pump / previous filter output) is moved
    // in and mutated in place  -  no allocation, no copy. Otherwise normalize.
    pm::video::VideoFrame frame;
    if (pm::video::is_packed_bgr24(raw)) {
        frame = std::move(raw);
    } else if (!pm::video::to_bgr24(raw, frame)) {
        io.error(kind, std::string(kind) + ": unsupported pixel format (need RGB24/BGR24)", 1);
        return BlockResult::Error;
    }

    if (!op(frame, err)) {
        io.error(kind, std::string(kind) + ": " + err, 1);
        return BlockResult::Error;
    }

    const std::string out_path = io.str("outputPath");
    const long long    ow      = frame.width;
    const long long    oh      = frame.height;
    std::string        token;
    if (!emit_frame(io, std::move(frame), out_path, /*in_memory=*/out_path.empty(), token, err)) {
        io.error(kind, std::string(kind) + ": " + err, 1);
        return BlockResult::Error;
    }

    io.ok(kind, std::string(kind) + " -> " + token)
      .set("path",   token)
      .set("width",  ow)
      .set("height", oh)
      .result(token, ParamKind::image_path);
    return BlockResult::Ok;
}

// ── videoGrayscale ────────────────────────────────────────────────────────────

BlockResult video_grayscale_block(BlockIO& io)
{
    return run_filter(io, "videoGrayscale", [](pm::video::VideoFrame& f, std::string&) {
        const std::size_t n = static_cast<std::size_t>(f.width) * f.height;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t* p = f.data.data() + i * 3;
            const std::uint8_t y = clamp_u8(luma_bgr(p[0], p[1], p[2]));
            p[0] = p[1] = p[2] = y;
        }
        return true;
    });
}

// ── videoColor (brightness / contrast / saturation) ───────────────────────────

BlockResult video_color_block(BlockIO& io)
{
    const float brightness = static_cast<float>(io.f64("brightness", 0.0)); // [-1,1] -> ±255
    const float contrast   = static_cast<float>(io.f64("contrast",   1.0)); // around 128
    const float saturation = static_cast<float>(io.f64("saturation", 1.0)); // blend toward luma
    const float add        = brightness * 255.f;

    return run_filter(io, "videoColor", [=](pm::video::VideoFrame& f, std::string&) {
        // No saturation change -> brightness/contrast is a per-byte affine map,
        // identical for B/G/R. Collapse it to a 256-entry LUT and sweep the whole
        // buffer (one load+store per byte, no float math)  -  same result as the
        // scalar path below since the LUT is built with the same clamp/round.
        if (saturation == 1.f) {
            std::uint8_t lut[256];
            for (int v = 0; v < 256; ++v)
                lut[v] = clamp_u8((v - 128.f) * contrast + 128.f + add);
            const std::size_t nbytes = static_cast<std::size_t>(f.width) * f.height * 3;
            std::uint8_t* d = f.data.data();
            for (std::size_t i = 0; i < nbytes; ++i) d[i] = lut[d[i]];
            return true;
        }
        const std::size_t n = static_cast<std::size_t>(f.width) * f.height;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t* p = f.data.data() + i * 3;
            float b = p[0], g = p[1], r = p[2];
            // saturation: lerp between luma (gray) and the colour.
            const float y = luma_bgr(p[0], p[1], p[2]);
            b = y + (b - y) * saturation;
            g = y + (g - y) * saturation;
            r = y + (r - y) * saturation;
            // contrast around mid-gray, then brightness offset.
            b = (b - 128.f) * contrast + 128.f + add;
            g = (g - 128.f) * contrast + 128.f + add;
            r = (r - 128.f) * contrast + 128.f + add;
            p[0] = clamp_u8(b);
            p[1] = clamp_u8(g);
            p[2] = clamp_u8(r);
        }
        return true;
    });
}

// ── videoCrop ─────────────────────────────────────────────────────────────────

BlockResult video_crop_block(BlockIO& io)
{
    const int rx = static_cast<int>(io.i64("x", 0));
    const int ry = static_cast<int>(io.i64("y", 0));
    const int rw = static_cast<int>(io.i64("width", 0));   // 0 = to right edge
    const int rh = static_cast<int>(io.i64("height", 0));  // 0 = to bottom edge

    return run_filter(io, "videoCrop", [=](pm::video::VideoFrame& f, std::string& err) {
        const int x = std::clamp(rx, 0, f.width - 1);
        const int y = std::clamp(ry, 0, f.height - 1);
        int w = rw <= 0 ? (f.width  - x) : rw;
        int h = rh <= 0 ? (f.height - y) : rh;
        w = std::clamp(w, 1, f.width  - x);
        h = std::clamp(h, 1, f.height - y);

        pm::video::VideoFrame out;
        out.width = w; out.height = h; out.stride = w * 3;
        out.format = pm::video::PixelFormat::BGR24;
        out.timestamp_ms = f.timestamp_ms;
        out.data.resize(static_cast<std::size_t>(w) * h * 3);
        for (int row = 0; row < h; ++row) {
            const std::uint8_t* s = f.data.data()
                + static_cast<std::size_t>(y + row) * f.stride + static_cast<std::size_t>(x) * 3;
            std::uint8_t* d = out.data.data() + static_cast<std::size_t>(row) * out.stride;
            std::memcpy(d, s, static_cast<std::size_t>(w) * 3);
        }
        f = std::move(out);
        (void)err;
        return true;
    });
}

// ── videoResize (bilinear) ────────────────────────────────────────────────────

BlockResult video_resize_block(BlockIO& io)
{
    const int tw = static_cast<int>(io.i64("width", 0));
    const int th = static_cast<int>(io.i64("height", 0));

    return run_filter(io, "videoResize", [=](pm::video::VideoFrame& f, std::string& err) {
        int dw = tw, dh = th;
        if (dw <= 0 && dh <= 0) { err = "width and height are both 0"; return false; }
        if (dw <= 0) dw = std::max(1, static_cast<int>(std::lround(double(f.width)  * dh / f.height)));
        if (dh <= 0) dh = std::max(1, static_cast<int>(std::lround(double(f.height) * dw / f.width)));
        if (dw == f.width && dh == f.height) return true;  // no-op

        pm::video::VideoFrame out;
        out.width = dw; out.height = dh; out.stride = dw * 3;
        out.format = pm::video::PixelFormat::BGR24;
        out.timestamp_ms = f.timestamp_ms;
        out.data.resize(static_cast<std::size_t>(dw) * dh * 3);

        const float sx = float(f.width)  / float(dw);
        const float sy = float(f.height) / float(dh);
        for (int y = 0; y < dh; ++y) {
            float fy = (y + 0.5f) * sy - 0.5f;
            int   y0 = static_cast<int>(std::floor(fy));
            float wy = fy - y0;
            const int y0c = std::clamp(y0, 0, f.height - 1);
            const int y1c = std::clamp(y0 + 1, 0, f.height - 1);
            std::uint8_t* d = out.data.data() + static_cast<std::size_t>(y) * out.stride;
            for (int x = 0; x < dw; ++x) {
                float fx = (x + 0.5f) * sx - 0.5f;
                int   x0 = static_cast<int>(std::floor(fx));
                float wx = fx - x0;
                const int x0c = std::clamp(x0, 0, f.width - 1);
                const int x1c = std::clamp(x0 + 1, 0, f.width - 1);
                const std::uint8_t* r0 = f.data.data() + static_cast<std::size_t>(y0c) * f.stride;
                const std::uint8_t* r1 = f.data.data() + static_cast<std::size_t>(y1c) * f.stride;
                for (int c = 0; c < 3; ++c) {
                    const float top = r0[x0c * 3 + c] * (1 - wx) + r0[x1c * 3 + c] * wx;
                    const float bot = r1[x0c * 3 + c] * (1 - wx) + r1[x1c * 3 + c] * wx;
                    d[x * 3 + c] = clamp_u8(top * (1 - wy) + bot * wy);
                }
            }
        }
        f = std::move(out);
        return true;
    });
}

// ── pictureOut ────────────────────────────────────────────────────────────────
// Encode a frame handle (or pass-through image path) to a real file on disk.
// The boundary back out of the in-memory pipeline.

BlockResult picture_out_block(BlockIO& io)
{
    const std::string input    = io.str("input");
    std::string       out_path = io.str("outputPath");
    const int         quality  = std::clamp(static_cast<int>(io.i64("quality", 90)), 1, 100);

    if (out_path.empty()) {
        io.error("pictureOut", "pictureOut: 'outputPath' is required", 1);
        return BlockResult::Error;
    }

    std::string err;
    pm::video::VideoFrame raw;
    if (!resolve_frame(io, input, raw, err)) {
        io.error("pictureOut", "pictureOut: " + err, 1).set("input", input);
        return BlockResult::Error;
    }
    pm::video::VideoFrame frame;
    if (!pm::video::to_bgr24(raw, frame)) {
        io.error("pictureOut", "pictureOut: unsupported pixel format (need RGB24/BGR24)", 1);
        return BlockResult::Error;
    }

    if (!pm::video::save_frame(frame, out_path, err, quality)) {
        io.error("pictureOut", "pictureOut: save failed: " + err, 1);
        return BlockResult::Error;
    }
    if (FrameStore* fs = frame_store(io)) fs->put(out_path, std::move(frame));

    io.ok("pictureOut", "wrote " + out_path)
      .set("path",   out_path)
      .set("width",  static_cast<long long>(raw.width))
      .set("height", static_cast<long long>(raw.height))
      .result(out_path, ParamKind::image_path);
    return BlockResult::Ok;
}

// ── videoWriter ───────────────────────────────────────────────────────────────
// Append a frame (handle or image path) to an .mp4. Stateful: the encoder lives
// in the run scope keyed by output path (or `instance`), so each pass/iteration
// adds one frame. The file is finalized when the run scope is released (run end
// / `xblox session stop`) via the encoder's destructor. Drive it from a for/while
// loop (or a --loop run) to build a real video. PREVIOUS ← output path.

struct VideoWriterInstance {
    std::unique_ptr<pm::video::VideoEncoder> enc;
    bool      started = false;
    int       fps     = 30;
    long long frames  = 0;
};

BlockResult video_writer_block(BlockIO& io)
{
    const std::string input    = io.str("input");
    const std::string out_path = io.str("outputPath");
    const long long   fps_req  = io.i64("fps", 0);   // 0 = follow the source
    const std::string instance = io.str("instance");

    if (out_path.empty()) {
        io.error("videoWriter", "videoWriter: 'outputPath' is required (.mp4)", 1);
        return BlockResult::Error;
    }

    // Resolve fps: an explicit value wins; otherwise follow the source fps the
    // videoSource published (so a file transcode keeps the original speed); 30
    // is the last-resort default for sources with no known rate (e.g. webcam).
    int fps = static_cast<int>(fps_req);
    if (fps <= 0) {
        double src_fps = 0.0;
        if (const nlohmann::json* f = io.context("videoSourceFps"))
            if (f->is_number()) src_fps = f->get<double>();
        fps = src_fps > 0.0 ? static_cast<int>(src_fps + 0.5) : 30;
    }
    fps = std::clamp(fps, 1, 240);

    std::string err;
    pm::video::VideoFrame raw;
    if (!resolve_frame_consume(io, input, raw, err)) {
        io.error("videoWriter", "videoWriter: " + err, 1).set("input", input);
        return BlockResult::Error;
    }
    pm::video::VideoFrame frame;
    if (pm::video::is_packed_bgr24(raw)) {
        frame = std::move(raw);  // pump / filter output -> no copy
    } else if (!pm::video::to_bgr24(raw, frame)) {
        io.error("videoWriter", "videoWriter: unsupported pixel format (need RGB24/BGR24)", 1);
        return BlockResult::Error;
    }

    // Acquire the persistent encoder for this output (survives loop iterations).
    VideoWriterInstance  local;
    VideoWriterInstance* w = &local;
    if (RunScope* scope = io.scope()) {
        const std::string key = "videoWriter:" + (instance.empty() ? out_path : instance);
        w = &scope->acquire<VideoWriterInstance>(key,
                [] { return std::make_shared<VideoWriterInstance>(); });
    }
    if (!w->enc) w->enc = std::make_unique<pm::video::VideoEncoder>();

    if (!w->started) {
        w->fps = fps;
        if (!w->enc->open(out_path, frame.width, frame.height, fps, err)) {
            io.error("videoWriter", "videoWriter: open failed: " + err, 1);
            return BlockResult::Error;
        }
        // Constant-rate PTS: frames are produced faster than real time, so anchor
        // playback to fps (frame_index / fps) instead of wall-clock  -  otherwise the
        // clip plays sped-up. (Live burn-in keeps the encoder's wall-clock default.)
        w->enc->set_constant_rate(true);
        w->started = true;
    }

    if (!w->enc->write(frame, err)) {
        io.error("videoWriter", "videoWriter: write failed: " + err, 1);
        return BlockResult::Error;
    }
    ++w->frames;

    io.ok("videoWriter", "wrote frame " + std::to_string(w->frames) + " -> " + out_path)
      .set("path",   out_path)
      .set("frames", w->frames)
      .set("fps",    static_cast<long long>(w->fps))
      .result(out_path, ParamKind::video_path);
    return BlockResult::Ok;
}

} // namespace

#endif // FEATURE_VIDEO

void register_video_filter_blocks(BlockRegistry& registry)
{
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    using J = nlohmann::json;

    // Shared input/output params: input sources from PREVIOUS (a frame handle or
    // image path); empty outputPath = keep the frame in-memory (mem:// handle).
    const auto in_param = [] {
        return pd_image("input").in().prev().dflt("")
            .desc("Frame handle (mem://...) from a previous filter, or an image path.");
    };
    const auto out_param = [] {
        return pd_outpath("outputPath").adv().dflt("")
            .desc("Output file (.jpg/.png). Empty = keep in-memory (mem:// handle) for the next filter.");
    };

    registry["videoGrayscale"] = bdb(
        "videoGrayscale", video_grayscale_block,
        "Grayscale", "Video",
        "Convert a frame to grayscale (Rec.601 luma). Pipes a frame handle in/out.",
        {{"kind","videoGrayscale"}, {"input",""}, {"outputPath",""}, {"storeAs","imagePath"}})
        .params({ in_param(), out_param(), pd_store_as().dflt("imagePath") })
        .primary_output(ParamKind::image_path, "Grayscale frame")
        .exec({false, false, true});

    registry["videoColor"] = bdb(
        "videoColor", video_color_block,
        "Color Adjust", "Video",
        "Adjust brightness / contrast / saturation of a frame. Pipes a frame handle in/out.",
        {{"kind","videoColor"}, {"input",""}, {"brightness",0.0}, {"contrast",1.0},
         {"saturation",1.0}, {"outputPath",""}, {"storeAs","imagePath"}})
        .params({
            in_param(),
            pd_float("brightness").opt().dflt(0.0).range(-1.0, 1.0)
                .desc("Brightness offset. 0 = none; -1..1 maps to ±255."),
            pd_float("contrast").opt().dflt(1.0).range(0.0, 3.0)
                .desc("Contrast multiplier around mid-gray. 1 = none."),
            pd_float("saturation").opt().dflt(1.0).range(0.0, 3.0)
                .desc("Saturation. 1 = none; 0 = grayscale; >1 = more vivid."),
            out_param(),
            pd_store_as().dflt("imagePath"),
        })
        .primary_output(ParamKind::image_path, "Adjusted frame")
        .exec({false, false, true});

    registry["videoCrop"] = bdb(
        "videoCrop", video_crop_block,
        "Crop", "Video",
        "Crop a rectangular region from a frame. Pipes a frame handle in/out.",
        {{"kind","videoCrop"}, {"input",""}, {"x",0}, {"y",0}, {"width",0}, {"height",0},
         {"outputPath",""}, {"storeAs","imagePath"}})
        .params({
            in_param(),
            pd_int("x").opt().dflt(0).range(0, 32768).desc("Left edge (px)."),
            pd_int("y").opt().dflt(0).range(0, 32768).desc("Top edge (px)."),
            pd_int("width").opt().dflt(0).range(0, 32768).desc("Crop width (px). 0 = to right edge."),
            pd_int("height").opt().dflt(0).range(0, 32768).desc("Crop height (px). 0 = to bottom edge."),
            out_param(),
            pd_store_as().dflt("imagePath"),
        })
        .primary_output(ParamKind::image_path, "Cropped frame")
        .exec({false, false, true});

    registry["videoResize"] = bdb(
        "videoResize", video_resize_block,
        "Resize", "Video",
        "Resize / resample a frame (bilinear). A 0 dimension is derived from the "
        "other to keep aspect. Pipes a frame handle in/out.",
        {{"kind","videoResize"}, {"input",""}, {"width",0}, {"height",0},
         {"outputPath",""}, {"storeAs","imagePath"}})
        .params({
            in_param(),
            pd_int("width").opt().dflt(0).range(0, 16384).desc("Target width (px). 0 = keep aspect from height."),
            pd_int("height").opt().dflt(0).range(0, 16384).desc("Target height (px). 0 = keep aspect from width."),
            out_param(),
            pd_store_as().dflt("imagePath"),
        })
        .primary_output(ParamKind::image_path, "Resized frame")
        .exec({false, false, true});

    registry["pictureOut"] = bdb(
        "pictureOut", picture_out_block,
        "Picture Out", "Video",
        "Encode a frame handle (or pass-through image) to a file on disk. The exit "
        "from the in-memory filter pipeline. Stores the written path in PREVIOUS / storeAs.",
        {{"kind","pictureOut"}, {"input",""}, {"outputPath",""}, {"quality",90}, {"storeAs","imagePath"}})
        .params({
            in_param(),
            pd_outpath("outputPath").in().dflt("")
                .desc("Output file path (.jpg/.png/.bmp). Required."),
            pd_int("quality").adv().dflt(90).range(1, 100).desc("JPEG quality (1-100)."),
            pd_store_as().dflt("imagePath"),
        })
        .primary_output(ParamKind::image_path, "Written image")
        .exec({false, false, true});

    registry["videoWriter"] = bdb(
        "videoWriter", video_writer_block,
        "Video Writer", "Video",
        "Append a frame (handle or image path) to an .mp4. Stateful across loop "
        "iterations  -  drive it from a for/while loop or --loop run to build a video. "
        "Finalized when the run ends. Stores the output path in PREVIOUS / storeAs.",
        {{"kind","videoWriter"}, {"input",""}, {"outputPath",""}, {"fps",0},
         {"instance",""}, {"storeAs","videoPath"}})
        .params({
            in_param(),
            pd_outpath("outputPath").in().dflt("")
                .desc("Output video path (.mp4). Required."),
            pd_int("fps").opt().dflt(0).range(0, 240)
                .desc("Output frame rate. 0 = follow the source (videoSource's detected fps; "
                      "30 if unknown). Set a value to force constant-rate output."),
            pd_str("instance").adv().dflt("")
                .desc("Optional writer id. Empty = keyed by output path. Use distinct ids for parallel writers."),
            pd_store_as().dflt("videoPath"),
        })
        .primary_output(ParamKind::video_path, "Output video")
        .exec({false, false, true});
#else
    (void)registry;
#endif
}

} // namespace media::xblox::blocks
