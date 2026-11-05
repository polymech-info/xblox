#include "blocks/video_vcam_blocks.hpp"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM

#include "blocks/frame_store.hpp"
#include "video/vcam/vcam_sink.hpp"

#include <memory>
#include <mutex>
#include <string>

#endif

namespace media::xblox::blocks {

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_VCAM) && FEATURE_VIDEO_VCAM

namespace {

pm::vcam::VcamSink& process_sink() {
    static pm::vcam::VcamSink sink;
    return sink;
}

std::mutex& sink_mu() {
    static std::mutex mu;
    return mu;
}

BlockResult video_virtual_out_block(BlockIO& io) {
    const std::string input = io.str("input");
    std::string err;
    pm::video::VideoFrame raw;
    if (!resolve_frame_consume(io, input, raw, err)) {
        io.error("videoVirtualOut", "videoVirtualOut: " + err, 1).set("input", input);
        return BlockResult::Error;
    }
    pm::video::VideoFrame frame;
    if (pm::video::is_packed_bgr24(raw)) {
        frame = std::move(raw);
    } else if (!pm::video::to_bgr24(raw, frame)) {
        io.error("videoVirtualOut", "videoVirtualOut: need RGB24/BGR24", 1);
        return BlockResult::Error;
    }

    pm::vcam::VcamOptions opt;
    opt.name = io.str("name", "Tanit Cam");
    {
        std::lock_guard<std::mutex> lk(sink_mu());
        auto& sink = process_sink();
        if (!sink.is_open() && !sink.start(opt, err)) {
            io.error("videoVirtualOut", "videoVirtualOut: " + err, 1);
            return BlockResult::Error;
        }
        if (!sink.push(frame, err)) {
            io.error("videoVirtualOut", "videoVirtualOut: " + err, 1);
            return BlockResult::Error;
        }
    }

    const long long ow = frame.width;
    const long long oh = frame.height;
    std::string token;
    if (!emit_frame(io, std::move(frame), "", true, token, err)) {
        io.error("videoVirtualOut", "videoVirtualOut: " + err, 1);
        return BlockResult::Error;
    }
    io.ok("videoVirtualOut", "videoVirtualOut -> " + token)
        .set("path", token)
        .set("device", opt.name)
        .set("width", ow)
        .set("height", oh)
        .result(token, ParamKind::image_path);
    return BlockResult::Ok;
}

} // namespace

void register_video_vcam_blocks(BlockRegistry& registry) {
    registry["videoVirtualOut"] = bdb(
        "videoVirtualOut", video_virtual_out_block,
        "Virtual Camera Out", "Video",
        "Push a frame to the Pixlwiz virtual webcam (MF Frame Server). "
        "Windows 11 only (MFCreateVirtualCamera). Same sink as "
        "`video record --virtual-out`. Zoom / Teams pick the device while this "
        "process is running.",
        {{"kind", "videoVirtualOut"}, {"input", ""}, {"name", "Tanit Cam"}})
        .params({
            pd_image("input").in().prev().dflt("").desc(
                "Frame handle (mem://...) or image path."),
            pd_str("name").opt().dflt("Tanit Cam").desc(
                "Device name shown to Zoom / Teams (Windows adds “Windows Virtual Camera”)."),
        })
        .primary_output(ParamKind::image_path, "Same frame")
        .exec({false, false, true});
}

#else

void register_video_vcam_blocks(BlockRegistry& registry) { (void)registry; }

#endif

} // namespace media::xblox::blocks
