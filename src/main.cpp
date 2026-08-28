#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view help_text{
    "NetLagLab\n"
    "Run a local application in an isolated network environment with controllable network "
    "conditions.\n\n"
    "Usage:\n"
    "  netlaglab [OPTION]\n\n"
    "Options:\n"
    "  -h, --help  Show this help message\n"
    "      --version  Show version information\n"};

int run_cli(
    const std::vector<std::string_view>& arguments,
    const std::string_view version,
    std::ostream& output,
    std::ostream& error)
{
    if (arguments.empty()) {
        output << help_text;
        return 0;
    }

    if (arguments.size() > 1) {
        error << "Error: expected at most one argument\n"
              << "Try 'netlaglab --help' for usage.\n";
        return 2;
    }

    const std::string_view argument{arguments.front()};

    if (argument == "--help" || argument == "-h") {
        output << help_text;
        return 0;
    }

    if (argument == "--version") {
        output << "NetLagLab " << version << '\n';
        return 0;
    }

    error << "Error: unknown argument '" << argument << "'\n"
          << "Try 'netlaglab --help' for usage.\n";
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    std::vector<std::string_view> arguments;

    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    return run_cli(arguments, NETLAGLAB_VERSION, std::cout, std::cerr);
}
