#include <cerrno>
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>

extern char** environ;

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

int spawn_error_exit_code(const int error_code)
{
    if (error_code == ENOENT) {
        return 127;
    }

    if (error_code == EACCES || error_code == ENOEXEC) {
        return 126;
    }

    return 125;
}

int wait_for_child(const pid_t child_pid, std::ostream& error)
{
    int status{};
    pid_t wait_result{};

    do {
        wait_result = waitpid(child_pid, &status, 0);
    } while (wait_result == -1 && errno == EINTR);

    if (wait_result == -1) {
        const int wait_error{errno};
        error << "NetLagLab: failed to wait for child process: " << std::strerror(wait_error) << '\n';
        return 125;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    error << "NetLagLab: child process ended with an unsupported status\n";
    return 125;
}

int run_program(char* const child_arguments[], std::ostream& error)
{
    pid_t child_pid{};
    const int spawn_error{
        posix_spawnp(&child_pid, child_arguments[0], nullptr, nullptr, child_arguments, environ)};

    if (spawn_error != 0) {
        error << "NetLagLab: failed to launch '" << child_arguments[0]
              << "': " << std::strerror(spawn_error) << '\n';
        return spawn_error_exit_code(spawn_error);
    }

    return wait_for_child(child_pid, error);
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

        return run_program(argv + 3, error);
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
