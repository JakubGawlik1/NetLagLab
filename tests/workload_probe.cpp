#include <array>
#include <cstring>
#include <string_view>
#include <unistd.h>

extern char** environ;

int main(const int argc, char* argv[])
{
    if (argc != 2 || std::string_view{argv[1]} != "argument\nwith-tab\t") {
        return 41;
    }

    std::array<char, 256> current_directory{};
    if (getcwd(current_directory.data(), current_directory.size()) == nullptr
        || std::string_view{current_directory.data()} != "/tmp") {
        return 42;
    }

    constexpr std::array<std::string_view, 4> expected_environment{
        "PATH=/bin", "DUP=first", "DUP=second", "EMPTY="};
    for (std::size_t index{}; index < expected_environment.size(); ++index) {
        if (environ[index] == nullptr
            || std::string_view{environ[index]} != expected_environment[index]) {
            return 43;
        }
    }
    return environ[expected_environment.size()] == nullptr ? 0 : 44;
}
