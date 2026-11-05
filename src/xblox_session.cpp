// asio must precede any header that may pull <windows.h> first, so its
// winsock2 ordering is established before winsock1 leaks in (Windows).
#include <asio.hpp>

#include "xblox_constants.hpp"
#include "xblox_session.hpp"

#include "core/settings_store.hpp"
#include "logger/logger.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace media::xblox::session {
namespace {

namespace fs = std::filesystem;
namespace xblox_constants = media::xblox::constants;

// ── portable loopback transport (see src/xblox/ref/cmd_gridsearch-uds.cpp) ──
#if defined(_WIN32)
using ipc_acceptor = asio::ip::tcp::acceptor;
using ipc_socket   = asio::ip::tcp::socket;
constexpr const char* kTransport = xblox_constants::kTransportTcp;
#else
using ipc_acceptor = asio::local::stream_protocol::acceptor;
using ipc_socket   = asio::local::stream_protocol::socket;
constexpr const char* kTransport = xblox_constants::kTransportUds;
#endif

// FNV-1a/64 -> lowercase hex. Non-cryptographic, stable identity only.
std::string fnv1a_hex(const std::string& s)
{
    std::uint64_t h = xblox_constants::kFnv1a64OffsetBasis;
    for (unsigned char c : s) {
        h ^= c;
        h *= xblox_constants::kFnv1a64Prime;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

#if defined(_WIN32)
std::int64_t current_pid() { return static_cast<std::int64_t>(::GetCurrentProcessId()); }

bool process_alive(std::int64_t pid)
{
    if (pid <= 0)
        return false;
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(pid));
    if (!h)
        return false;
    DWORD code = STILL_ACTIVE;
    const BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}
#else
std::int64_t current_pid() { return static_cast<std::int64_t>(::getpid()); }

bool process_alive(std::int64_t pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}
#endif

// Filesystem-safe descriptor stem (keys carry ':' which is invalid on Windows).
std::string sanitize_stem(const std::string& key)
{
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.empty())
        out = "session";
    return out;
}

fs::path descriptor_path(const std::string& key)
{
    return sessions_dir() / (sanitize_stem(key) + ".json");
}

#if !defined(_WIN32)
fs::path socket_path(const std::string& key)
{
    return sessions_dir() / (sanitize_stem(key) + ".sock");
}
#endif

bool write_descriptor(const SessionDescriptor& d, std::string& err)
{
    try {
        const fs::path final_path = descriptor_path(d.key);
        fs::path tmp_path = final_path;
        tmp_path += media::path::path_from_utf8(".tmp");
        {
            std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                err = "cannot open " + media::path::path_to_utf8(tmp_path);
                return false;
            }
            ofs << d.to_json().dump(2);
        }
        std::error_code ec;
        fs::rename(tmp_path, final_path, ec);
        if (ec) {
            // Cross-volume / locked target: best-effort overwrite.
            fs::copy_file(tmp_path, final_path, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp_path, ec);
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

void remove_descriptor(const std::string& key)
{
    std::error_code ec;
    fs::remove(descriptor_path(key), ec);
#if !defined(_WIN32)
    fs::remove(socket_path(key), ec);
#endif
}

// ── framing: 4-byte LE length prefix + UTF-8 JSON ──────────────────────────
void write_frame(ipc_socket& sock, const std::string& json, asio::error_code& ec)
{
    std::uint32_t len = static_cast<std::uint32_t>(json.size());
    asio::write(sock, asio::buffer(&len, 4), ec);
    if (ec)
        return;
    asio::write(sock, asio::buffer(json), ec);
}

bool read_frame(ipc_socket& sock, std::string& out, asio::error_code& ec)
{
    std::uint32_t len = 0;
    asio::read(sock, asio::buffer(&len, 4), ec);
    if (ec)
        return false;
    if (len == 0 || len > xblox_constants::kIpcMaxFrameBytes) {
        ec = asio::error::message_size;
        return false;
    }
    out.assign(len, '\0');
    asio::read(sock, asio::buffer(&out[0], len), ec);
    return !ec;
}

constexpr std::size_t kTypedMagicLen = xblox_constants::kIpcTypedMagicLen;

bool is_typed_frame(const std::string& payload)
{
    return payload.size() >= kTypedMagicLen + 1 &&
           payload.compare(0, kTypedMagicLen, xblox_constants::kIpcTypedMagic, kTypedMagicLen) == 0;
}

void append_u8(std::string& out, std::uint8_t v)
{
    out.push_back(static_cast<char>(v));
}

void append_u32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (i * 8)) & 0xffu));
}

void append_i64(std::string& out, std::int64_t v)
{
    const auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((u >> (i * 8)) & 0xffu));
}

void append_string(std::string& out, const std::string& value)
{
    const auto len = std::min<std::size_t>(value.size(), std::numeric_limits<std::uint32_t>::max());
    append_u32(out, static_cast<std::uint32_t>(len));
    out.append(value.data(), len);
}

bool read_u8(const std::string& in, std::size_t& pos, std::uint8_t& out)
{
    if (pos + 1 > in.size())
        return false;
    out = static_cast<std::uint8_t>(in[pos++]);
    return true;
}

bool read_u32(const std::string& in, std::size_t& pos, std::uint32_t& out)
{
    if (pos + 4 > in.size())
        return false;
    out = 0;
    for (int i = 0; i < 4; ++i)
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos++])) << (i * 8);
    return true;
}

bool read_i64(const std::string& in, std::size_t& pos, std::int64_t& out)
{
    if (pos + 8 > in.size())
        return false;
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i)
        u |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[pos++])) << (i * 8);
    out = static_cast<std::int64_t>(u);
    return true;
}

bool read_string(const std::string& in, std::size_t& pos, std::string& out)
{
    std::uint32_t len = 0;
    if (!read_u32(in, pos, len) || pos + len > in.size())
        return false;
    out.assign(in.data() + pos, len);
    pos += len;
    return true;
}

void append_ipc_message(std::string& out, const IpcMessage& msg, bool include_ts)
{
    append_string(out, msg.from);
    append_string(out, msg.message);
    if (include_ts)
        append_i64(out, msg.ts);
    append_u8(out, msg.has_data ? 1 : 0);
    if (msg.has_data) {
        const std::string data = msg.data.dump();
        append_string(out, data);
    }
}

bool read_ipc_message(const std::string& in, std::size_t& pos, IpcMessage& msg, bool include_ts)
{
    std::uint8_t has_data = 0;
    std::string data;
    if (!read_string(in, pos, msg.from) || !read_string(in, pos, msg.message))
        return false;
    if (include_ts && !read_i64(in, pos, msg.ts))
        return false;
    if (!read_u8(in, pos, has_data))
        return false;
    msg.has_data = has_data != 0;
    msg.data = nullptr;
    if (msg.has_data) {
        if (!read_string(in, pos, data))
            return false;
        msg.data = nlohmann::json::parse(data);
    }
    return true;
}

std::string typed_send_batch_request(const std::vector<IpcMessage>& messages)
{
    std::string out;
    out.reserve(kTypedMagicLen + xblox_constants::kIpcTypedSendBatchReserveFixedBytes +
                messages.size() * xblox_constants::kIpcTypedSendBatchReservePerMessageBytes);
    out.append(xblox_constants::kIpcTypedMagic, kTypedMagicLen);
    out.push_back(xblox_constants::kIpcTypedSendBatch);
    append_u32(out, static_cast<std::uint32_t>(std::min<std::size_t>(messages.size(), std::numeric_limits<std::uint32_t>::max())));
    for (const auto& msg : messages)
        append_ipc_message(out, msg, false);
    return out;
}

std::string typed_recv_request(bool peek)
{
    std::string out;
    out.reserve(kTypedMagicLen + xblox_constants::kIpcTypedRecvReserveFixedBytes);
    out.append(xblox_constants::kIpcTypedMagic, kTypedMagicLen);
    out.push_back(xblox_constants::kIpcTypedRecv);
    append_u8(out, peek ? 1 : 0);
    return out;
}

std::string typed_send_ack_reply(const SendMessagesReply& reply)
{
    std::string out;
    out.reserve(kTypedMagicLen + xblox_constants::kIpcTypedSendAckReserveFixedBytes);
    out.append(xblox_constants::kIpcTypedMagic, kTypedMagicLen);
    out.push_back(xblox_constants::kIpcTypedSendAck);
    append_i64(out, reply.accepted);
    append_i64(out, reply.inbox);
    return out;
}

std::string typed_messages_reply(const std::vector<IpcMessage>& messages)
{
    std::string out;
    out.reserve(kTypedMagicLen + xblox_constants::kIpcTypedMessagesReserveFixedBytes +
                messages.size() * xblox_constants::kIpcTypedMessagesReservePerMessageBytes);
    out.append(xblox_constants::kIpcTypedMagic, kTypedMagicLen);
    out.push_back(xblox_constants::kIpcTypedMessages);
    append_u32(out, static_cast<std::uint32_t>(std::min<std::size_t>(messages.size(), std::numeric_limits<std::uint32_t>::max())));
    for (const auto& msg : messages)
        append_ipc_message(out, msg, true);
    return out;
}

bool parse_typed_send_batch_request(const std::string& payload, std::vector<IpcMessage>& messages)
{
    if (!is_typed_frame(payload) || payload[kTypedMagicLen] != xblox_constants::kIpcTypedSendBatch)
        return false;
    std::size_t pos = kTypedMagicLen + 1;
    std::uint32_t count = 0;
    if (!read_u32(payload, pos, count))
        return false;
    messages.clear();
    messages.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        IpcMessage msg;
        if (!read_ipc_message(payload, pos, msg, false))
            return false;
        messages.push_back(std::move(msg));
    }
    return pos == payload.size();
}

bool parse_typed_recv_request(const std::string& payload, bool& peek)
{
    if (!is_typed_frame(payload) || payload[kTypedMagicLen] != xblox_constants::kIpcTypedRecv)
        return false;
    std::size_t pos = kTypedMagicLen + 1;
    std::uint8_t peek_byte = 0;
    if (!read_u8(payload, pos, peek_byte))
        return false;
    peek = peek_byte != 0;
    return pos == payload.size();
}

bool parse_typed_send_ack_reply(const std::string& payload, SendMessagesReply& reply)
{
    if (!is_typed_frame(payload) || payload[kTypedMagicLen] != xblox_constants::kIpcTypedSendAck)
        return false;
    std::size_t pos = kTypedMagicLen + 1;
    std::int64_t accepted = 0;
    std::int64_t inbox = 0;
    if (!read_i64(payload, pos, accepted) || !read_i64(payload, pos, inbox))
        return false;
    reply.accepted = accepted;
    reply.inbox = inbox;
    return pos == payload.size();
}

bool parse_typed_messages_reply(const std::string& payload, std::vector<IpcMessage>& messages)
{
    if (!is_typed_frame(payload) || payload[kTypedMagicLen] != xblox_constants::kIpcTypedMessages)
        return false;
    std::size_t pos = kTypedMagicLen + 1;
    std::uint32_t count = 0;
    if (!read_u32(payload, pos, count))
        return false;
    messages.clear();
    messages.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        IpcMessage msg;
        if (!read_ipc_message(payload, pos, msg, true))
            return false;
        messages.push_back(std::move(msg));
    }
    return pos == payload.size();
}

IpcMessage legacy_json_to_message(const nlohmann::json& msg)
{
    IpcMessage out;
    out.from = msg.value("from", std::string{"?"});
    out.message = msg.value("message", std::string{});
    if (msg.contains("data")) {
        out.has_data = true;
        out.data = msg["data"];
    }
    out.ts = now_unix_ms();
    return out;
}

nlohmann::json message_to_json(const IpcMessage& msg)
{
    nlohmann::json out{
        {"from", msg.from},
        {"message", msg.message},
        {"ts", msg.ts},
    };
    if (msg.has_data)
        out["data"] = msg.data;
    return out;
}

} // namespace

// ── descriptor (de)serialization ───────────────────────────────────────────
nlohmann::json SessionDescriptor::to_json() const
{
    return nlohmann::json{
        {"key", key},
        {"source", source},
        {"label", label},
        {"cwd", cwd},
        {"endpoint", endpoint},
        {"transport", transport},
        {"pid", pid},
        {"startedUnixMs", started_unix_ms},
        {"loop", loop},
    };
}

SessionDescriptor SessionDescriptor::from_json(const nlohmann::json& j)
{
    SessionDescriptor d;
    d.key             = j.value("key", std::string{});
    d.source          = j.value("source", std::string{});
    d.label           = j.value("label", std::string{});
    d.cwd             = j.value("cwd", std::string{});
    d.endpoint        = j.value("endpoint", std::string{});
    d.transport       = j.value("transport", std::string{});
    d.pid             = j.value("pid", std::int64_t{0});
    d.started_unix_ms = j.value("startedUnixMs", std::int64_t{0});
    d.loop            = j.value("loop", false);
    return d;
}

fs::path sessions_dir()
{
    const fs::path dir = media::settings::get_config_dir() / "xblox" / "sessions";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string make_file_key(const fs::path& src)
{
    std::error_code ec;
    fs::path abs = fs::absolute(src, ec);
    std::string s = media::path::path_to_generic_utf8((ec ? src : abs));
#if defined(_WIN32)
    for (char& c : s)
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
#endif
    return "file:" + fnv1a_hex(s);
}

std::string make_data_key(const nlohmann::json& blocks_file)
{
    return "data:" + fnv1a_hex(blocks_file.dump());
}

// ── SessionServer ───────────────────────────────────────────────────────────
struct SessionServer::Impl {
    asio::io_context              io;
    std::unique_ptr<ipc_acceptor> acceptor;
    std::thread                   thread;
    std::atomic<bool>             running{false};
    std::atomic<bool>             stop{false};
    SessionDescriptor             descriptor;

    // Shared control-plane state, touched by the accept thread (inbound frames)
    // and the run thread (variable bridge). One mutex guards all of it.
    std::mutex            state_mtx;
    nlohmann::json        vars = nlohmann::json::object(); // published run variables
    std::vector<VarPair>  pending_sets;                    // externally-issued sets
    std::deque<IpcMessage>     inbox;                      // received messages
    nlohmann::json        artifacts = nlohmann::json::array(); // harvested file writes
    static constexpr std::size_t kInboxCap = xblox_constants::kIpcInboxCap;

    std::mutex                                  client_mtx;
    std::vector<std::thread>                   client_threads;
    std::vector<std::shared_ptr<ipc_socket>>   client_sockets;

    void handle_client(std::shared_ptr<ipc_socket> sock)
    {
        for (;;) {
            asio::error_code ec;
            std::string      payload;
            if (!read_frame(*sock, payload, ec))
                break;

            if (is_typed_frame(payload)) {
                try {
                    if (payload[kTypedMagicLen] == xblox_constants::kIpcTypedSendBatch) {
                        std::vector<IpcMessage> messages;
                        if (!parse_typed_send_batch_request(payload, messages))
                            break;
                        SendMessagesReply typed_reply;
                        {
                            std::lock_guard<std::mutex> lock(state_mtx);
                            for (auto& msg : messages) {
                                msg.ts = now_unix_ms();
                                inbox.push_back(std::move(msg));
                                ++typed_reply.accepted;
                            }
                            while (inbox.size() > kInboxCap)
                                inbox.pop_front();
                            typed_reply.inbox = static_cast<long long>(inbox.size());
                        }
                        write_frame(*sock, typed_send_ack_reply(typed_reply), ec);
                        if (ec)
                            break;
                        continue;
                    }
                    if (payload[kTypedMagicLen] == xblox_constants::kIpcTypedRecv) {
                        bool peek = false;
                        if (!parse_typed_recv_request(payload, peek))
                            break;
                        std::vector<IpcMessage> messages;
                        {
                            std::lock_guard<std::mutex> lock(state_mtx);
                            messages.assign(inbox.begin(), inbox.end());
                            if (!peek)
                                inbox.clear();
                        }
                        write_frame(*sock, typed_messages_reply(messages), ec);
                        if (ec)
                            break;
                        continue;
                    }
                } catch (...) {
                    break;
                }
            }

            nlohmann::json msg;
            try {
                msg = nlohmann::json::parse(payload);
            } catch (...) {
                continue;
            }

            const std::string action = msg.value("action", std::string{});
            const std::string id      = msg.value("id", std::string{});

            nlohmann::json reply;
            if (!id.empty())
                reply["id"] = id;
            reply["key"] = descriptor.key;
            reply["pid"] = descriptor.pid;
            reply["ts"]  = now_unix_ms();

            if (action == "ping") {
                reply["type"] = "pong";
            } else if (action == "pong") {
                reply["type"] = "ping";
            } else if (action == "status" || action == "info") {
                reply["type"]    = "status";
                reply["session"] = descriptor.to_json();
            } else if (action == "stop") {
                reply["type"] = "stop_ack";
                stop.store(true);
            } else if (action == "send") {
                // Directed/broadcast message: stash into the inbox for the owner
                // (or a peer `recv`) to drain. `from` lets the receiver attribute it.
                IpcMessage m = legacy_json_to_message(msg);
                std::lock_guard<std::mutex> lock(state_mtx);
                inbox.push_back(std::move(m));
                while (inbox.size() > kInboxCap)
                    inbox.pop_front();
                reply["type"]  = "send_ack";
                reply["inbox"] = inbox.size();
            } else if (action == "send_batch") {
                const auto& messages = msg.contains("messages") && msg["messages"].is_array()
                    ? msg["messages"]
                    : nlohmann::json::array();
                long long accepted = 0;
                std::lock_guard<std::mutex> lock(state_mtx);
                for (const auto& item : messages) {
                    IpcMessage m;
                    m.ts = now_unix_ms();
                    if (item.is_object()) {
                        m.from = item.value("from", msg.value("from", std::string{"?"}));
                        m.message = item.value("message", msg.value("message", std::string{}));
                        if (item.contains("data")) {
                            m.has_data = true;
                            m.data = item["data"];
                        } else if (msg.contains("data")) {
                            m.has_data = true;
                            m.data = msg["data"];
                        }
                    } else {
                        m.from = msg.value("from", std::string{"?"});
                        m.message = msg.value("message", std::string{});
                        m.has_data = true;
                        m.data = item;
                    }
                    inbox.push_back(std::move(m));
                    ++accepted;
                }
                while (inbox.size() > kInboxCap)
                    inbox.pop_front();
                reply["type"]     = "send_batch_ack";
                reply["accepted"] = accepted;
                reply["inbox"]    = inbox.size();
            } else if (action == "recv") {
                // Drain the inbox (default) or peek without consuming.
                const bool peek = msg.value("peek", false);
                std::lock_guard<std::mutex> lock(state_mtx);
                reply["type"]     = "messages";
                reply["messages"] = nlohmann::json::array();
                for (const auto& m : inbox)
                    reply["messages"].push_back(message_to_json(m));
                if (!peek)
                    inbox.clear();
            } else if (action == "var.set") {
                const std::string name = msg.value("name", std::string{});
                if (name.empty()) {
                    reply["type"]  = "error";
                    reply["error"] = "var.set requires a name";
                } else {
                    nlohmann::json value = msg.contains("value") ? msg["value"] : nlohmann::json(nullptr);
                    std::lock_guard<std::mutex> lock(state_mtx);
                    vars[name] = value;             // visible to get immediately
                    pending_sets.emplace_back(name, value); // applied next pass
                    reply["type"]  = "var_ack";
                    reply["name"]  = name;
                    reply["value"] = value;
                }
            } else if (action == "var.get") {
                const std::string name = msg.value("name", std::string{});
                std::lock_guard<std::mutex> lock(state_mtx);
                reply["type"]  = "var";
                reply["name"]  = name;
                reply["found"] = vars.contains(name);
                reply["value"] = vars.contains(name) ? vars[name] : nlohmann::json(nullptr);
            } else if (action == "var.list") {
                std::lock_guard<std::mutex> lock(state_mtx);
                reply["type"] = "vars";
                reply["vars"] = vars;
            } else if (action == "artifacts") {
                std::lock_guard<std::mutex> lock(state_mtx);
                reply["type"]  = "artifacts";
                reply["files"] = artifacts;
                reply["count"] = artifacts.size();
            } else {
                reply["type"]  = "error";
                reply["error"] = "unknown action: " + action;
            }

            write_frame(*sock, reply.dump(), ec);
            if (ec || action == "stop")
                break;
        }
    }

    void accept_loop()
    {
        while (running.load()) {
            auto             sock = std::make_shared<ipc_socket>(io);
            asio::error_code ec;
            acceptor->accept(*sock, ec);
            if (ec || !running.load())
                break;
            std::lock_guard<std::mutex> lock(client_mtx);
            client_sockets.push_back(sock);
            client_threads.emplace_back([this, sock] { handle_client(sock); });
        }
    }

    void close_clients()
    {
        std::lock_guard<std::mutex> lock(client_mtx);
        for (auto& sock : client_sockets) {
            if (!sock)
                continue;
            asio::error_code ec;
            sock->close(ec);
        }
    }

    void join_clients()
    {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(client_mtx);
            threads.swap(client_threads);
            client_sockets.clear();
        }
        for (auto& t : threads) {
            if (t.joinable())
                t.join();
        }
    }
};

SessionServer::SessionServer() : impl_(std::make_unique<Impl>()) {}
SessionServer::~SessionServer() { shutdown(); }

bool SessionServer::start(const SessionInfo& info, std::string& err)
{
    if (impl_->running.load()) {
        err = "session already started";
        return false;
    }
    if (info.key.empty()) {
        err = "session key is empty";
        return false;
    }

    SessionDescriptor& d = impl_->descriptor;
    d.key             = info.key;
    d.source          = info.source;
    d.label           = info.label.empty() ? (info.source.empty() ? info.key : info.source) : info.label;
    d.cwd             = info.cwd;
    d.transport       = kTransport;
    d.pid             = current_pid();
    d.started_unix_ms = now_unix_ms();
    d.loop            = info.loop;

    try {
#if defined(_WIN32)
        asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
        impl_->acceptor = std::make_unique<ipc_acceptor>(impl_->io);
        impl_->acceptor->open(ep.protocol());
        impl_->acceptor->set_option(asio::socket_base::reuse_address(true));
        impl_->acceptor->bind(ep);
        impl_->acceptor->listen();
        d.endpoint = std::to_string(impl_->acceptor->local_endpoint().port());
#else
        const fs::path path = socket_path(info.key);
        std::error_code rmec;
        fs::remove(path, rmec);
        asio::local::stream_protocol::endpoint ep(media::path::path_to_utf8(path));
        impl_->acceptor = std::make_unique<ipc_acceptor>(impl_->io, ep);
        d.endpoint = media::path::path_to_utf8(path);
#endif
    } catch (const std::exception& e) {
        err = std::string("bind failed: ") + e.what();
        impl_->acceptor.reset();
        return false;
    }

    if (!write_descriptor(d, err)) {
        impl_->acceptor.reset();
        return false;
    }

    impl_->running.store(true);
    impl_->thread = std::thread([this] { impl_->accept_loop(); });
    logger::info("[xblox-session] hosting " + d.key + " on " + d.transport + ":" + d.endpoint);
    return true;
}

void SessionServer::shutdown()
{
    if (!impl_ || !impl_->running.exchange(false))
        return;
    if (impl_->acceptor) {
        std::error_code ec;
        impl_->acceptor->close(ec);
    }
    impl_->close_clients();
    if (impl_->thread.joinable())
        impl_->thread.join();
    impl_->join_clients();
    impl_->acceptor.reset();
    remove_descriptor(impl_->descriptor.key);
    logger::info("[xblox-session] released " + impl_->descriptor.key);
}

bool SessionServer::stop_requested() const { return impl_ && impl_->stop.load(); }

const SessionDescriptor& SessionServer::descriptor() const { return impl_->descriptor; }

void SessionServer::publish_vars(const nlohmann::json& vars)
{
    if (!impl_)
        return;
    std::lock_guard<std::mutex> lock(impl_->state_mtx);
    impl_->vars = vars.is_object() ? vars : nlohmann::json::object();
}

std::vector<VarPair> SessionServer::take_pending_sets()
{
    std::vector<VarPair> out;
    if (!impl_)
        return out;
    std::lock_guard<std::mutex> lock(impl_->state_mtx);
    out.swap(impl_->pending_sets);
    return out;
}

void SessionServer::push_message(const IpcMessage& msg)
{
    if (!impl_)
        return;
    IpcMessage copy = msg;
    if (copy.ts == 0)
        copy.ts = now_unix_ms();
    std::lock_guard<std::mutex> lock(impl_->state_mtx);
    impl_->inbox.push_back(std::move(copy));
    while (impl_->inbox.size() > Impl::kInboxCap)
        impl_->inbox.pop_front();
}

void SessionServer::record_artifact(const nlohmann::json& artifact)
{
    if (!impl_ || !artifact.is_object())
        return;
    std::lock_guard<std::mutex> lock(impl_->state_mtx);
    impl_->artifacts.push_back(artifact);
}

nlohmann::json SessionServer::artifacts_snapshot() const
{
    if (!impl_)
        return nlohmann::json::array();
    std::lock_guard<std::mutex> lock(impl_->state_mtx);
    return impl_->artifacts;
}

// ── registry queries ─────────────────────────────────────────────────────────
std::vector<SessionDescriptor> list_sessions()
{
    std::vector<SessionDescriptor> out;
    const fs::path dir = sessions_dir();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".json")
            continue;
        try {
            std::ifstream ifs(it->path(), std::ios::binary);
            if (!ifs)
                continue;
            std::ostringstream ss;
            ss << ifs.rdbuf();
            SessionDescriptor d = SessionDescriptor::from_json(nlohmann::json::parse(ss.str()));
            if (!process_alive(d.pid)) {
                remove_descriptor(d.key); // reap stale session
                continue;
            }
            out.push_back(std::move(d));
        } catch (...) {
            // malformed descriptor  -  leave it for the owner to overwrite
        }
    }
    return out;
}

bool find_session(const std::string& key, SessionDescriptor& out)
{
    const auto sessions = list_sessions();
    // exact key
    for (const auto& d : sessions)
        if (d.key == key) {
            out = d;
            return true;
        }
    // descriptor stem (filename without extension)
    for (const auto& d : sessions)
        if (sanitize_stem(d.key) == key) {
            out = d;
            return true;
        }
    // unambiguous key prefix
    const SessionDescriptor* match = nullptr;
    int                      hits  = 0;
    for (const auto& d : sessions)
        if (d.key.rfind(key, 0) == 0) {
            match = &d;
            ++hits;
        }
    if (hits == 1) {
        out = *match;
        return true;
    }
    return false;
}

// ── reusable client ───────────────────────────────────────────────────────────
struct SessionClient::Impl {
    asio::io_context        io;
    std::unique_ptr<ipc_socket> sock;
    SessionDescriptor       descriptor;
    bool                    is_connected = false;

    void close()
    {
        if (sock) {
            asio::error_code ec;
            sock->close(ec);
            sock.reset();
        }
        is_connected = false;
        io.restart();
    }
};

SessionClient::SessionClient() : impl_(std::make_unique<Impl>()) {}
SessionClient::~SessionClient() { close(); }

bool SessionClient::connect(const SessionDescriptor& d, std::string& err, int timeout_ms)
{
    if (!impl_)
        return false;
    impl_->close();

    try {
        impl_->sock = std::make_unique<ipc_socket>(impl_->io);
        asio::error_code connect_ec = asio::error::would_block;
        impl_->io.restart();
#if defined(_WIN32)
        unsigned short port = 0;
        try {
            port = static_cast<unsigned short>(std::stoi(d.endpoint));
        } catch (...) {
            err = "invalid endpoint: " + d.endpoint;
            impl_->close();
            return false;
        }
        asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
        impl_->sock->async_connect(ep, [&](const asio::error_code& ec) { connect_ec = ec; });
#else
        asio::local::stream_protocol::endpoint ep(d.endpoint);
        impl_->sock->async_connect(ep, [&](const asio::error_code& ec) { connect_ec = ec; });
#endif
        impl_->io.run_for(std::chrono::milliseconds(timeout_ms));
        if (connect_ec == asio::error::would_block) {
            err = "connect timed out";
            impl_->close();
            return false;
        }
        if (connect_ec) {
            err = "connect failed: " + connect_ec.message();
            impl_->close();
            return false;
        }

        impl_->descriptor = d;
        impl_->is_connected = true;
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        impl_->close();
        return false;
    }
}

bool SessionClient::request(const std::string&    action,
                            const nlohmann::json& extra,
                            nlohmann::json&       reply,
                            std::string&          err,
                            int)
{
    if (!impl_ || !impl_->is_connected || !impl_->sock) {
        err = "client not connected";
        return false;
    }

    try {
        nlohmann::json request = extra.is_object() ? extra : nlohmann::json::object();
        request["action"] = action;

        asio::error_code io_ec;
        write_frame(*impl_->sock, request.dump(), io_ec);
        if (io_ec) {
            err = "write failed: " + io_ec.message();
            impl_->close();
            return false;
        }

        std::string payload;
        if (!read_frame(*impl_->sock, payload, io_ec)) {
            err = "read failed: " + io_ec.message();
            impl_->close();
            return false;
        }
        reply = nlohmann::json::parse(payload);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        impl_->close();
        return false;
    }
}

bool SessionClient::send_messages(const std::vector<IpcMessage>& messages,
                                  SendMessagesReply&             reply,
                                  std::string&                   err,
                                  int)
{
    if (!impl_ || !impl_->is_connected || !impl_->sock) {
        err = "client not connected";
        return false;
    }

    try {
        asio::error_code io_ec;
        write_frame(*impl_->sock, typed_send_batch_request(messages), io_ec);
        if (io_ec) {
            err = "write failed: " + io_ec.message();
            impl_->close();
            return false;
        }

        std::string payload;
        if (!read_frame(*impl_->sock, payload, io_ec)) {
            err = "read failed: " + io_ec.message();
            impl_->close();
            return false;
        }
        if (!parse_typed_send_ack_reply(payload, reply)) {
            err = "invalid typed send ack";
            impl_->close();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        impl_->close();
        return false;
    }
}

bool SessionClient::receive_messages(bool                    peek,
                                     std::vector<IpcMessage>& messages,
                                     std::string&            err,
                                     int)
{
    if (!impl_ || !impl_->is_connected || !impl_->sock) {
        err = "client not connected";
        return false;
    }

    try {
        asio::error_code io_ec;
        write_frame(*impl_->sock, typed_recv_request(peek), io_ec);
        if (io_ec) {
            err = "write failed: " + io_ec.message();
            impl_->close();
            return false;
        }

        std::string payload;
        if (!read_frame(*impl_->sock, payload, io_ec)) {
            err = "read failed: " + io_ec.message();
            impl_->close();
            return false;
        }
        if (!parse_typed_messages_reply(payload, messages)) {
            err = "invalid typed message reply";
            impl_->close();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        impl_->close();
        return false;
    }
}

void SessionClient::close()
{
    if (impl_)
        impl_->close();
}

bool SessionClient::connected() const
{
    return impl_ && impl_->is_connected;
}

// ── client ───────────────────────────────────────────────────────────────────
bool send_action(const SessionDescriptor& d,
                 const std::string&        action,
                 const nlohmann::json&     extra,
                 nlohmann::json&           reply,
                 std::string&              err,
                 int                       timeout_ms)
{
    SessionClient client;
    if (!client.connect(d, err, timeout_ms))
        return false;
    return client.request(action, extra, reply, err, timeout_ms);
}

bool send_messages(const SessionDescriptor&       d,
                   const std::vector<IpcMessage>& messages,
                   SendMessagesReply&             reply,
                   std::string&                   err,
                   int                            timeout_ms)
{
    SessionClient client;
    if (!client.connect(d, err, timeout_ms))
        return false;
    return client.send_messages(messages, reply, err, timeout_ms);
}

bool receive_messages(const SessionDescriptor& d,
                      bool                     peek,
                      std::vector<IpcMessage>& messages,
                      std::string&             err,
                      int                      timeout_ms)
{
    SessionClient client;
    if (!client.connect(d, err, timeout_ms))
        return false;
    return client.receive_messages(peek, messages, err, timeout_ms);
}

} // namespace media::xblox::session
