#pragma once
// frame_store.hpp  -  run-scoped, in-memory frame cache for the video/image
// filter pipeline.
//
// xblox's PREVIOUS currency is JSON (string/number/object), so it cannot carry
// a raw pixel buffer. To still pipe decoded frames between chained filter blocks
// without re-encoding to disk at every stage, producers stash the VideoFrame in
// this run-scoped cache and emit a *token string* as PREVIOUS; consumers resolve
// that token back to the live buffer (zero-copy fast path), or decode from disk
// when the token is a plain file path that was not cached.
//
// Token scheme (the PREVIOUS string a filter block emits / accepts):
//   "mem://<n>"    -  memory-only frame (no file written); valid only within the
//                   same run scope (a pass, or a persisting --loop).
//   "<path>"       -  file-backed frame. The producer wrote this path to disk AND
//                   cached it under the same key, so the next stage skips the
//                   decode; if it is not cached (e.g. an author-supplied image),
//                   resolve_frame() falls back to pm::video::load_frame().
//
// The cache is a small ring (kMax). Memory-only tokens past the cap are evicted;
// keep chains short or write to disk (pictureOut) for long-lived artifacts.

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "blocks/block_types.hpp"
#include "runtime.hpp"
#include "core/video.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace media::xblox::blocks {

struct FrameStore {
    static constexpr const char* kKey = "xblox:frameStore";
    static constexpr std::size_t kMax = 32;  // ring cap (decoded frames are large)

    std::mutex                                                                mtx;
    std::unordered_map<std::string, std::shared_ptr<pm::video::VideoFrame>>   map;
    std::deque<std::string>                                                   order;
    std::atomic<std::uint64_t>                                                next_id{1};

    // Cache `frame` under `key`, evicting the oldest entries past the ring cap.
    std::string put(const std::string& key, pm::video::VideoFrame frame) {
        std::lock_guard<std::mutex> lk(mtx);
        if (map.find(key) == map.end()) order.push_back(key);
        map[key] = std::make_shared<pm::video::VideoFrame>(std::move(frame));
        while (order.size() > kMax) {
            const std::string old = order.front();
            order.pop_front();
            if (old != key) map.erase(old);
        }
        return key;
    }

    // Cache a memory-only frame under a fresh "mem://<n>" token.
    std::string put_mem(pm::video::VideoFrame frame) {
        return put("mem://" + std::to_string(next_id.fetch_add(1)), std::move(frame));
    }

    std::shared_ptr<pm::video::VideoFrame> get(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map.find(key);
        return it == map.end() ? nullptr : it->second;
    }

    // Remove and return the frame under `key` (single-use handoff). The stale key
    // left in `order` is harmless  -  eviction erase()s a missing key as a no-op.
    std::shared_ptr<pm::video::VideoFrame> take(const std::string& key) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map.find(key);
        if (it == map.end()) return nullptr;
        auto sp = std::move(it->second);
        map.erase(it);
        return sp;
    }
};

// Acquire (or lazily create) the run-scoped frame store. Null when the run has
// no scope (e.g. detached background work)  -  callers must fall back to disk.
inline FrameStore* frame_store(BlockIO& io) {
    if (RunScope* s = io.scope())
        return &s->acquire<FrameStore>(FrameStore::kKey,
                                       [] { return std::make_shared<FrameStore>(); });
    return nullptr;
}

// Resolve an input spec (PREVIOUS token or path) to a frame: cached handle
// (zero-copy) first, else decode the file from disk. Output is whatever the
// producer/decoder stored (call to_bgr24() before pixel work).
inline bool resolve_frame(BlockIO& io, const std::string& spec,
                          pm::video::VideoFrame& out, std::string& err) {
    if (spec.empty()) { err = "no input image (expected a path or frame handle)"; return false; }
    if (FrameStore* fs = frame_store(io)) {
        if (auto sp = fs->get(spec)) { out = *sp; return true; }
    }
    if (spec.rfind("mem://", 0) == 0) {
        err = "frame handle expired or out of scope: " + spec;
        return false;
    }
    return pm::video::load_frame(spec, out, err);
}

// Like resolve_frame, but for a single-use pipeline handle ("mem://...") it MOVES
// the buffer out of the store (so the consumer can mutate in place with no copy).
// File-path tokens are still copied/decoded (they may be referenced again). This
// is the per-frame fast path for the videoSource flat pipe: each mem:// handle is
// produced by one stage and consumed by exactly the next.
inline bool resolve_frame_consume(BlockIO& io, const std::string& spec,
                                  pm::video::VideoFrame& out, std::string& err) {
    if (spec.empty()) { err = "no input image (expected a path or frame handle)"; return false; }
    if (FrameStore* fs = frame_store(io)) {
        if (spec.rfind("mem://", 0) == 0) {
            if (auto sp = fs->take(spec)) {
                if (sp.use_count() == 1) out = std::move(*sp);  // sole owner -> steal it
                else                     out = *sp;
                return true;
            }
            err = "frame handle expired or out of scope: " + spec;
            return false;
        }
        if (auto sp = fs->get(spec)) { out = *sp; return true; }  // path: keep cached
    }
    if (spec.rfind("mem://", 0) == 0) {
        err = "frame handle expired or out of scope: " + spec;
        return false;
    }
    return pm::video::load_frame(spec, out, err);
}

// Emit a processed frame. When `out_path` is non-empty and `in_memory` is false
// the frame is encoded to disk AND cached under the path (so the next stage
// skips decode); otherwise it is cached memory-only under a "mem://" token.
// `token` receives the PREVIOUS value (path or handle).
inline bool emit_frame(BlockIO& io, pm::video::VideoFrame frame,
                       const std::string& out_path, bool in_memory,
                       std::string& token, std::string& err) {
    FrameStore* fs = frame_store(io);
    if (in_memory || out_path.empty()) {
        if (!fs) { err = "in-memory frames require a run scope"; return false; }
        token = fs->put_mem(std::move(frame));
        return true;
    }
    if (!pm::video::save_frame(frame, out_path, err)) return false;
    if (fs) fs->put(out_path, std::move(frame));
    token = out_path;
    return true;
}

} // namespace media::xblox::blocks

#endif // FEATURE_VIDEO
