#pragma once
// XBlox command surface shared by pm-xblox and tanit-cli.
//
// register_subcommands() adds info, selftest, run, and session onto the app
// it is given. pm-xblox passes its root app. tanit-cli passes the `xblox`
// subcommand. Host-only work (settings commands.json, fs policy, consent,
// run-ipc, the video recorder, command-finish) is installed with set_hooks().

#include "artifact_ledger.hpp"
#include "runtime.hpp"
#include "xblox_session.hpp"

#include <CLI/CLI.hpp>

#include <functional>
#include <string>
#include <vector>

namespace media::xblox::cli {

struct ParentRefs {
    const std::string* cwd = nullptr;
    const std::vector<std::string>* process_args = nullptr;
    const std::string* process_command_line = nullptr;
};

struct RunAttach {
    ExecutionOptions& options;
    session::SessionServer* session = nullptr;
    bool session_hosted = false;
    bool will_loop = false;
    std::string cwd;
    std::string label;
    std::string source;
    std::string command_line;
    artifacts::ArtifactLedger* ledger = nullptr;
    std::function<nlohmann::json()> artifacts_snapshot;
    std::string consent_ui;
    std::string consent_owner;
};

struct FinishInfo {
    const ExecutionResult& result;
    nlohmann::json& report;
    const nlohmann::json& artifacts;
    artifacts::ArtifactLedger* ledger = nullptr;
    std::filesystem::path src;
};

struct Hooks {
    // --commands path. Empty path means "host default" for tanit-cli and an
    // empty document for pm-xblox.
    std::function<bool(const std::string& commands_path, nlohmann::json& out, std::string& err)> load_commands;
    // Path shown in `info` when the host owns commands.json. Empty uses --commands.
    std::function<std::string()> commands_path;
    std::function<void(nlohmann::json& registered, nlohmann::json& custom)> list_commands;
    std::function<void(ExecutionOptions&)> prepare_options;
    // Returns a function run after the document, before the session shuts down.
    std::function<std::function<void()>(RunAttach&)> attach;
    std::function<void()> install_interrupts;
    std::function<bool()> cancel_requested;
    std::function<void()> request_cancel;
    std::function<std::string(const std::string&)> expand_path;
    // When set, its return value is the process exit code.
    std::function<int(FinishInfo&)> finish;
};

void set_hooks(Hooks hooks);
void set_parent_refs(ParentRefs refs);
void register_subcommands(CLI::App& cmd);
int dispatch();

} // namespace media::xblox::cli
