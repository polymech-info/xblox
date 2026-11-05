// xblox_session  -  self-contained, cross-process session registry + control bus.
//
// A "session" is a live xblox run (or a bare host) that advertises a small
// JSON descriptor on disk and listens on a loopback control socket. Other
// processes discover sessions by scanning the descriptor directory and talk to
// them with length-prefixed JSON frames (ping/pong/stop/status).
//
// Transport mirrors src/xblox/ref/cmd_gridsearch-uds.cpp: loopback TCP on
// Windows, AF_UNIX stream socket elsewhere. The wire format is a 4-byte
// little-endian length prefix followed by a UTF-8 JSON object  -  the exact frame
// the Node test harness in tests/orchestrator/test-commons.js already speaks.
//
// This layer is intentionally free of the runtime/PassEngine internals; the
// looping executor and the CLI both consume it without pulling in asio.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::xblox::session {

// Identity + metadata supplied when a process opens a session.
struct SessionInfo {
    std::string key;          // stable identity: "file:<hex>" / "data:<hex>" / "name:<x>"
    std::string source;       // originating document path, or "" for inline/bare
    std::string label;        // human-friendly label (defaults to source/key)
    std::string cwd;          // working directory the run executes in
    bool        loop = false; // true when hosted by a looping run
};

// On-disk descriptor: what `list` returns and what clients connect against.
struct SessionDescriptor {
    std::string  key;
    std::string  source;
    std::string  label;
    std::string  cwd;
    std::string  endpoint;        // win: "<port>"; posix: absolute .sock path
    std::string  transport;       // "tcp" | "uds"
    std::int64_t pid = 0;
    std::int64_t started_unix_ms = 0;
    bool         loop = false;

    nlohmann::json        to_json() const;
    static SessionDescriptor from_json(const nlohmann::json& j);
};

using VarPair = std::pair<std::string, nlohmann::json>;

// Hot-path inter-session message. `data` is intentionally optional JSON:
// message routing/counting stays on std primitives, while structured user
// payloads cross the JSON boundary only when a block explicitly supplies them.
struct IpcMessage {
    std::string from;
    std::string message;
    bool        has_data = false;
    nlohmann::json data = nullptr;
    std::int64_t ts = 0;
};

struct SendMessagesReply {
    long long accepted = 0;
    long long inbox = 0;
};

// Directory holding <stem>.json descriptors (and posix <stem>.sock files).
std::filesystem::path sessions_dir();

// Stable identity keys. File key hashes the absolute path; data key hashes the
// compact document so two identical inline runs collapse to one identity.
std::string make_file_key(const std::filesystem::path& src);
std::string make_data_key(const nlohmann::json& blocks_file);

// RAII control server. start() binds the loopback socket, writes the descriptor
// and spawns an accept thread; shutdown()/dtor closes the socket, joins, and
// removes the descriptor. A peer "stop" frame flips stop_requested().
class SessionServer {
public:
    SessionServer();
    ~SessionServer();
    SessionServer(const SessionServer&)            = delete;
    SessionServer& operator=(const SessionServer&) = delete;

    bool start(const SessionInfo& info, std::string& err);
    void shutdown();
    bool stop_requested() const;
    const SessionDescriptor& descriptor() const;

    // Variable bridge for a looping run (called on the run thread at pass
    // boundaries; the server thread touches the same store under a mutex):
    //  - publish_vars(): snapshot the run's variables so peers can `get`/`vars`.
    //  - take_pending_sets(): drain externally-issued `set`s to apply this pass.
    void                 publish_vars(const nlohmann::json& vars);
    std::vector<VarPair> take_pending_sets();

    // In-process inbox push (no socket round-trip). Used by the run thread to
    // surface live events — e.g. file artifacts — to ipcReceive / session recv.
    void push_message(const IpcMessage& msg);

    // Run-scoped artifact ledger: structured file-write records harvested from
    // block events. Peers poll via action:"artifacts" or read from cmd_ipc status.
    void           record_artifact(const nlohmann::json& artifact);
    nlohmann::json artifacts_snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reusable control client. Keeps one socket open so hot block loops can exchange
// repeated frames without reconnecting for every send/recv action.
class SessionClient {
public:
    SessionClient();
    ~SessionClient();
    SessionClient(const SessionClient&)            = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    bool connect(const SessionDescriptor& descriptor, std::string& err, int timeout_ms = 3000);
    bool request(const std::string&    action,
                 const nlohmann::json& extra,
                 nlohmann::json&       reply,
                 std::string&          err,
                 int                   timeout_ms = 3000);
    bool send_messages(const std::vector<IpcMessage>& messages,
                       SendMessagesReply&             reply,
                       std::string&                   err,
                       int                            timeout_ms = 3000);
    bool receive_messages(bool                    peek,
                          std::vector<IpcMessage>& messages,
                          std::string&            err,
                          int                     timeout_ms = 3000);
    void close();
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Enumerate live sessions, reaping descriptors whose owning pid has exited.
std::vector<SessionDescriptor> list_sessions();

// Resolve a session by exact key, descriptor stem, or unambiguous key prefix.
bool find_session(const std::string& key, SessionDescriptor& out);

// Connect to a session and exchange one request/reply frame. `extra` is merged
// into the outgoing {action,...} object. Returns false (with err) on transport
// failure or timeout.
bool send_action(const SessionDescriptor& descriptor,
                 const std::string&        action,
                 const nlohmann::json&     extra,
                 nlohmann::json&           reply,
                 std::string&              err,
                 int                       timeout_ms = 3000);

bool send_messages(const SessionDescriptor&       descriptor,
                   const std::vector<IpcMessage>& messages,
                   SendMessagesReply&             reply,
                   std::string&                   err,
                   int                            timeout_ms = 3000);

bool receive_messages(const SessionDescriptor& descriptor,
                      bool                     peek,
                      std::vector<IpcMessage>& messages,
                      std::string&             err,
                      int                      timeout_ms = 3000);

} // namespace media::xblox::session
