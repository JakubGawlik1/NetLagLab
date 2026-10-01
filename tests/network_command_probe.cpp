#include <charconv>
#include <csignal>
#include <chrono>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

extern char** environ;

int main(const int argc, char* argv[])
{
    if (argc == 3 && std::string_view{argv[1]} == "exit") {
        int exit_code{};
        const std::string_view value{argv[2]};
        const auto result{std::from_chars(
            value.data(), value.data() + value.size(), exit_code)};
        return result.ec == std::errc{} ? exit_code : 43;
    }
    if (argc == 2 && std::string_view{argv[1]} == "signal") {
        raise(SIGTERM);
        return 44;
    }
    if (argc == 3 && std::string_view{argv[1]} == "stderr") {
        std::size_t size{};
        const std::string_view value{argv[2]};
        const auto result{
            std::from_chars(value.data(), value.data() + value.size(), size)};
        if (result.ec != std::errc{}) {
            return 45;
        }
        const std::string output(size, 'x');
        std::size_t written{};
        while (written < output.size()) {
            const ssize_t count{
                write(STDERR_FILENO, output.data() + written, output.size() - written)};
            if (count <= 0) {
                return 46;
            }
            written += static_cast<std::size_t>(count);
        }
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "sleep") {
        int milliseconds{};
        const std::string_view value{argv[2]};
        const auto result{std::from_chars(
            value.data(), value.data() + value.size(), milliseconds)};
        if (result.ec != std::errc{} || milliseconds < 0) {
            return 47;
        }
        const std::string pid{std::to_string(getpid()) + '\n'};
        if (write(STDERR_FILENO, pid.data(), pid.size())
            != static_cast<ssize_t>(pid.size())) {
            return 48;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "create-marker") {
        const int descriptor{open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0600)};
        if (descriptor == -1) {
            return 49;
        }
        return close(descriptor) == 0 ? 0 : 50;
    }
    if (argc != 4 || std::string_view{argv[0]} != argv[1]
        || std::string_view{argv[2]} != "first argument"
        || std::string_view{argv[3]} != "second\nargument") {
        return 41;
    }
    return environ[0] == nullptr ? 0 : 42;
}
