// XBlox videoStream block - named lifecycle wrapper for `video stream --server`.

#include "blocks/video_blocks.hpp"
#include "video/streaming/streaming_session.hpp"

#include "logging/logger.h"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "string/string_utils.hpp"
#endif

#endif

namespace media::xblox::blocks {

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM

namespace {

using media::strutil::lower_ascii;

#if defined(_WIN32)
std::wstring widen_utf8(const std::string& value)
{
    if (value.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return std::wstring(value.begin(), value.end());
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}

std::wstring quote_arg(std::wstring value)
{
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
            continue;
        }
        if (backslashes) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }
    if (backslashes)
        out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::filesystem::path fallback_executable_path()
{
    std::vector<wchar_t> buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size() - 1u));
    if (n == 0) return {};
    return std::filesystem::path(std::wstring(buf.data(), n));
}
#endif

class VideoStreamInstance {
public:
    ~VideoStreamInstance() { stop(); }

    bool start(const std::filesystem::path& exe,
               const std::string& input,
               const std::string& bind,
               int port,
               int fps,
               int bitrate_kbps,
               int mjpeg_quality,
               bool include_cursor,
               const std::string& token,
               const std::string& encoder,
               bool open,
               std::string& err)
    {
#if defined(_WIN32)
        if (running()) {
            err = "stream instance is already running";
            return false;
        }
        if (port <= 0) {
            err = "videoStream block requires a fixed positive port";
            return false;
        }
        if (exe.empty()) {
            err = "could not resolve pm-image executable path";
            return false;
        }

        input_ = input.empty() ? "screen:0" : input;
        bind_ = bind.empty() ? "127.0.0.1" : bind;
        port_ = port;
        fps_ = fps;
        bitrate_kbps_ = bitrate_kbps;
        mjpeg_quality_ = mjpeg_quality;
        include_cursor_ = include_cursor;
        token_ = token.empty() ? pm::video::streaming::make_stream_token() : token;
        encoder_ = encoder.empty() ? "auto" : encoder;
        url_ = "http://" + pm::video::streaming::stream_url_host(bind_) + ":" + std::to_string(port_) + "/";

        std::wstring cmd = quote_arg(exe.wstring());
        cmd += L" video stream --server";
        cmd += L" --input " + quote_arg(widen_utf8(input_));
        cmd += L" --bind " + quote_arg(widen_utf8(bind_));
        cmd += L" --port " + std::to_wstring(port_);
        cmd += L" --fps " + std::to_wstring(fps_);
        cmd += L" --bitrate-kbps " + std::to_wstring(bitrate_kbps_);
        cmd += L" --mjpeg-quality " + std::to_wstring(mjpeg_quality_);
        cmd += L" --token " + quote_arg(widen_utf8(token_));
        cmd += L" --encoder " + quote_arg(widen_utf8(encoder_));
        if (!include_cursor_)
            cmd += L" --no-cursor";
        if (open)
            cmd += L" --open";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');
        const std::filesystem::path cwd = exe.parent_path();
        const std::wstring wide_cwd = cwd.wstring();
        const BOOL ok = ::CreateProcessW(
            exe.wstring().c_str(),
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
            nullptr,
            wide_cwd.empty() ? nullptr : wide_cwd.c_str(),
            &si,
            &pi);
        if (!ok) {
            err = "CreateProcess failed (err=" + std::to_string(::GetLastError()) + ")";
            return false;
        }

        process_ = pi.hProcess;
        thread_ = pi.hThread;
        pid_ = pi.dwProcessId;
        started_ = std::chrono::steady_clock::now();
        return true;
#else
        (void)exe; (void)input; (void)bind; (void)port; (void)fps; (void)bitrate_kbps;
        (void)mjpeg_quality; (void)include_cursor; (void)token; (void)encoder; (void)open;
        err = "videoStream process backend is currently implemented for Win32";
        return false;
#endif
    }

    void stop()
    {
#if defined(_WIN32)
        if (process_) {
            if (::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
                ::TerminateProcess(process_, 0);
            ::WaitForSingleObject(process_, 3000);
            ::CloseHandle(process_);
            process_ = nullptr;
        }
        if (thread_) {
            ::CloseHandle(thread_);
            thread_ = nullptr;
        }
#endif
    }

    bool running() const
    {
#if defined(_WIN32)
        return process_ && ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
        return false;
#endif
    }

    nlohmann::json status_json() const
    {
        const long long uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        return {
            {"running", running()},
            {"pid", static_cast<long long>(pid_)},
            {"url", url_},
            {"statusUrl", url_.empty() ? std::string{} : url_ + "status"},
            {"input", input_},
            {"bind", bind_},
            {"port", port_},
            {"fps", fps_},
            {"bitrateKbps", bitrate_kbps_},
            {"mjpegQuality", mjpeg_quality_},
            {"includeCursor", include_cursor_},
            {"encoder", encoder_},
            {"token", token_},
            {"uptimeMs", uptime_ms},
        };
    }

private:
#if defined(_WIN32)
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    DWORD  pid_ = 0;
#endif
    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
    std::string input_;
    std::string bind_;
    int port_ = 0;
    int fps_ = 30;
    int bitrate_kbps_ = 8000;
    int mjpeg_quality_ = 85;
    bool include_cursor_ = true;
    std::string token_;
    std::string encoder_ = "auto";
    std::string url_;
};

std::mutex& stream_instances_mutex()
{
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::string, std::shared_ptr<VideoStreamInstance>>& stream_instances()
{
    static std::unordered_map<std::string, std::shared_ptr<VideoStreamInstance>> instances;
    return instances;
}

std::filesystem::path stream_executable_path(BlockIO& io)
{
    if (!io.runtime().options.cli_executable_path.empty())
        return io.runtime().options.cli_executable_path;
#if defined(_WIN32)
    return fallback_executable_path();
#else
    return {};
#endif
}

} // namespace

BlockResult video_stream_block(BlockIO& io)
{
    const std::string kind_id = "videoStream";
    std::string action = lower_ascii(io.str("action", "Start"));
    if (action.empty()) action = "start";
    std::string instance_name = io.str("instance", "default");
    if (instance_name.empty())
        instance_name = "default";

    if (action == "stop") {
        std::shared_ptr<VideoStreamInstance> inst;
        {
            std::lock_guard<std::mutex> lk(stream_instances_mutex());
            auto it = stream_instances().find(instance_name);
            if (it != stream_instances().end()) {
                inst = it->second;
                stream_instances().erase(it);
            }
        }
        if (!inst) {
            io.warn(kind_id, "videoStream: no stream instance named '" + instance_name + "'")
              .set("instance", instance_name);
            return BlockResult::Ok;
        }
        const nlohmann::json before = inst->status_json();
        inst->stop();
        io.ok(kind_id, "stopped stream instance '" + instance_name + "'")
          .set("instance", instance_name)
          .payload(before)
          .result(Value::from_json(before), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action == "status") {
        std::shared_ptr<VideoStreamInstance> inst;
        {
            std::lock_guard<std::mutex> lk(stream_instances_mutex());
            auto it = stream_instances().find(instance_name);
            if (it != stream_instances().end())
                inst = it->second;
        }
        nlohmann::json status = inst ? inst->status_json() : nlohmann::json{{"running", false}, {"instance", instance_name}};
        status["instance"] = instance_name;
        io.ok(kind_id, "stream status for '" + instance_name + "'")
          .payload(status)
          .result(Value::from_json(std::move(status)), ParamKind::json_value);
        return BlockResult::Ok;
    }

    if (action != "start") {
        io.error(kind_id, "videoStream: unknown action '" + action + "'", 1);
        return BlockResult::Error;
    }

    {
        std::lock_guard<std::mutex> lk(stream_instances_mutex());
        if (stream_instances().find(instance_name) != stream_instances().end()) {
            io.error(kind_id, "videoStream: stream instance already running: " + instance_name, 1)
              .set("instance", instance_name);
            return BlockResult::Error;
        }
    }

    auto inst = std::make_shared<VideoStreamInstance>();
    std::string err;
    std::string input = io.str("input", "screen:0");
    if (input.empty())
        input = "screen:0";
    std::string bind = io.str("bind", "127.0.0.1");
    if (bind.empty())
        bind = "127.0.0.1";
    const int port = static_cast<int>(io.i64("port", 8787));
    const int fps = std::clamp(static_cast<int>(io.i64("fps", 30)), 1, 240);
    const int bitrate = std::max(1, static_cast<int>(io.i64("bitrateKbps", 8000)));
    const int quality = std::clamp(static_cast<int>(io.i64("mjpegQuality", 85)), 1, 100);
    const bool include_cursor = io.flag("includeCursor", true);
    const std::string token = io.str("token");
    std::string encoder = io.str("encoder", "auto");
    if (encoder.empty())
        encoder = "auto";
    const bool open = io.flag("open", false);

    if (!inst->start(stream_executable_path(io), input, bind, port, fps, bitrate, quality,
                     include_cursor, token, encoder, open, err)) {
        io.error(kind_id, "videoStream: " + err, 1)
          .set("instance", instance_name);
        return BlockResult::Error;
    }

    {
        std::lock_guard<std::mutex> lk(stream_instances_mutex());
        stream_instances()[instance_name] = inst;
    }

    nlohmann::json status = inst->status_json();
    status["instance"] = instance_name;
    io.ok(kind_id, "started stream instance '" + instance_name + "': " + status.value("url", std::string{}))
      .set("instance", instance_name)
      .set("url", status.value("url", std::string{}))
      .set("token", status.value("token", std::string{}))
      .payload(status)
      .result(Value::from_json(std::move(status)), ParamKind::json_value);
    return BlockResult::Ok;
}

#else

BlockResult video_stream_block(BlockIO& io)
{
    io.error("videoStream", "videoStream requires FEATURE_VIDEO_STREAM", 1);
    return BlockResult::Error;
}

#endif

} // namespace media::xblox::blocks
