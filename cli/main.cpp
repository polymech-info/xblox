#include "xblox_cli.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <vector>

namespace pm_xblox_standalone {
void set_settings_file(std::string path);
void set_config_dir(std::string path);
}

int main(int argc, char** argv) {
    // tanit-cli tests use both `xblox --log-level off run` and
    // `--log-level warn xblox run`. The package usage check uses `run --src`.
    std::vector<std::string> owned;
    std::vector<char*> args;
    args.push_back(argv[0]);
    const bool first_is_xblox = argc > 1 && std::string(argv[1]) == "xblox";
    const bool first_is_flag = argc > 1 && argv[1][0] == '-';
    if (argc > 1 && !first_is_xblox && !first_is_flag) {
        owned.emplace_back("xblox");
        args.push_back(owned.back().data());
    }
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);

    CLI::App app{"pm-xblox"};
    app.require_subcommand(1);
    auto* xblox = app.add_subcommand("xblox", "Run XBlox block-tree command flows.");
    xblox->require_subcommand(1);

    std::string log_level;
    std::string settings_file;
    std::string config_dir;
    const auto log_check = CLI::IsMember(
        {"trace", "debug", "info", "warn", "warning", "error", "err", "critical", "off", "none"},
        CLI::ignore_case);
    app.add_option("--log-level", log_level, "Log level for this process.")->check(log_check);
    app.add_option("--settings", settings_file, "Settings JSON file.");
    app.add_option("--config-dir", config_dir, "Profile directory.");
    xblox->add_option("--log-level", log_level, "Log level for this process.")->check(log_check);

    media::xblox::cli::register_subcommands(*xblox);

    try {
        app.parse(static_cast<int>(args.size()), args.data());
    } catch (const CLI::ParseError& e) {
        if (e.get_exit_code() == static_cast<int>(CLI::ExitCodes::Success)) {
            std::cout << app.help() << "\n";
            return 0;
        }
        std::cerr << e.what() << "\n";
        return 2;
    }

    pm_xblox_standalone::set_settings_file(std::move(settings_file));
    pm_xblox_standalone::set_config_dir(std::move(config_dir));
    return media::xblox::cli::dispatch();
}
