#include "xblox_commands.hpp"

#include <CLI/CLI.hpp>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {

int fail(const std::string& error, const std::string& code, int status) {
    nlohmann::json body = {
        {"ok", false},
        {"error", error},
        {"code", code},
        {"status", status},
    };
    std::cout << body.dump() << "\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"pm-xblox"};
    app.require_subcommand(1);

    std::string document;
    bool quiet = false;
    auto* run = app.add_subcommand("run", "Run one xblox document");
    run->add_option("document", document, "Path to a .xblox JSON document")->required();
    run->add_flag("--quiet", quiet, "Suppress stdout blocks");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        if (e.get_exit_code() == static_cast<int>(CLI::ExitCodes::Success)) {
            std::cout << app.help() << "\n";
            return 0;
        }
        std::cerr << e.what() << "\n";
        return 2;
    }

    std::ifstream in(document, std::ios::binary);
    if (!in) {
        return fail("cannot open document", "EIO", 404);
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        return fail(ex.what(), "EPARSE", 400);
    }

    media::xblox::ExecutionOptions options;
    options.quiet = quiet;
    options.execute_external_commands = false;
    options.source_path = document;

    media::xblox::ExecutionResult result;
    try {
        result = media::xblox::run_blocks_file(doc, options);
    } catch (const std::exception& ex) {
        return fail(ex.what(), "ERUN", 500);
    }

    nlohmann::json body = {
        {"ok", result.ok && result.error_count == 0},
        {"exit_code", result.exit_code},
        {"error_count", result.error_count},
        {"event_count", result.event_count},
    };
    std::cout << body.dump() << "\n";
    return body["ok"].get<bool>() ? 0 : 1;
}
