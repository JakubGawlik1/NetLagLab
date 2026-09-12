#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view usage{
    "Usage: netlaglab-helper --runtime-dir <absolute-path>\n"};
constexpr int usage_error_exit_code{2};

int usage_error(const std::string_view message)
{
    std::cerr << "NetLagLab helper: " << message << '\n' << usage;
    return usage_error_exit_code;
}

int run_helper(const int argc, char* argv[])
{
    if (argc != 3 || std::string_view{argv[1]} != "--runtime-dir") {
        return usage_error("expected '--runtime-dir' followed by a path");
    }

    const std::string_view runtime_directory{argv[2]};
    if (runtime_directory.empty() || runtime_directory.front() != '/') {
        return usage_error("runtime directory must be an absolute path");
    }

    if (geteuid() != 0) {
        std::cerr << "NetLagLab helper: root privileges are required\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, char* argv[])
{
    return run_helper(argc, argv);
}
