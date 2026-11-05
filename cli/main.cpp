#include "xblox_cli.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    CLI::App app{"pm-xblox"};
    app.require_subcommand(1);

    std::string log_level;
    app.add_option(
            "--log-level",
            log_level,
            "Log level for this process.")
        ->check(CLI::IsMember(
            {"trace", "debug", "info", "warn", "warning", "error", "err", "critical", "off", "none"},
            CLI::ignore_case));

    media::xblox::cli::register_subcommands(app);

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

    return media::xblox::cli::dispatch();
}
