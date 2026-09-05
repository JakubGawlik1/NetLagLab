#include "session.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view help_text{
    "NetLagLab\n"
    "Run a local application in an isolated network environment with controllable network "
    "conditions.\n\n"
    "Usage:\n"
    "  netlaglab [OPTION]\n"
    "  netlaglab run -- <program> [arguments...]\n\n"
    "Commands:\n"
    "  run -- <program> [arguments...]\n"
    "      Run a program and wait for it to exit\n\n"
    "Options:\n"
    "  -h, --help  Show this help message\n"
    "      --version  Show version information\n"};

int run_usage_error(const std::string_view message, std::ostream& error)
{
    error << "NetLagLab: " << message << '\n'
          << "Usage: netlaglab run -- <program> [arguments...]\n";
    return 2;
}

int run_cli(
    const int argc,
    char* argv[],
    const std::string_view version,
    std::ostream& output,
    std::ostream& error)
{
    if (argc == 1) {
        output << help_text;
        return 0;
    }

    const std::string_view argument{argv[1]};

    if (argument == "run") {
        if (argc == 2) {
            return run_usage_error("expected '--' followed by a program", error);
        }

        if (std::string_view{argv[2]} != "--") {
            return run_usage_error("expected '--' after 'run'", error);
        }

        if (argc == 3) {
            return run_usage_error("expected a program after '--'", error);
        }

        return netlaglab::run_session(argv + 3, error);
    }

    if (argc > 2) {
        error << "NetLagLab: expected at most one global option\n"
              << "Try 'netlaglab --help' for usage.\n";
        return 2;
    }

    if (argument == "--help" || argument == "-h") {
        output << help_text;
        return 0;
    }

    if (argument == "--version") {
        output << "NetLagLab " << version << '\n';
        return 0;
    }

    error << "NetLagLab: unknown argument '" << argument << "'\n"
          << "Try 'netlaglab --help' for usage.\n";
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    return run_cli(argc, argv, NETLAGLAB_VERSION, std::cout, std::cerr);
}
