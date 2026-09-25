#include "helper_process.hpp"

#include "session_paths.hpp"
#include "socket_io.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <ostream>
#include <spawn.h>
#include <sys/socket.h>
#include <string>
#include <string_view>
#include <system_error>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace netlaglab {
namespace {

constexpr std::chrono::milliseconds helper_connection_retry_delay{50};

[[nodiscard]] std::optional<pid_t> read_parent_pid(const pid_t pid)
{
    std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
    std::string line;
    if (!std::getline(stat, line)) {
        return std::nullopt;
    }
    const std::size_t closing_parenthesis{line.rfind(')')};
    if (closing_parenthesis == std::string::npos
        || closing_parenthesis + 4 >= line.size()) {
        return std::nullopt;
    }
    std::string_view remainder{line};
    remainder.remove_prefix(closing_parenthesis + 2);
    const std::size_t state_separator{remainder.find(' ')};
    if (state_separator == std::string_view::npos) {
        return std::nullopt;
    }
    remainder.remove_prefix(state_separator + 1);
    const std::size_t parent_end{remainder.find(' ')};
    const std::string_view parent_text{remainder.substr(0, parent_end)};
    pid_t parent{};
    const auto parsed{std::from_chars(
        parent_text.data(), parent_text.data() + parent_text.size(), parent)};
    if (parsed.ec != std::errc{} || parsed.ptr != parent_text.data() + parent_text.size()) {
        return std::nullopt;
    }
    return parent;
}

[[nodiscard]] bool belongs_to_helper_launcher(
    const pid_t peer_pid,
    const pid_t helper_launcher_pid)
{
    pid_t current{peer_pid};
    for (std::size_t depth{}; depth < 32 && current > 1; ++depth) {
        if (current == helper_launcher_pid) {
            return true;
        }
        const std::optional<pid_t> parent{read_parent_pid(current)};
        if (!parent.has_value() || *parent == current) {
            return false;
        }
        current = *parent;
    }
    return current == helper_launcher_pid;
}

void report_helper_exit(
    const int status,
    std::ostream& error)
{
    if (WIFEXITED(status)) {
        error << "NetLagLab: privileged helper launcher exited; exit code: "
              << WEXITSTATUS(status) << '\n';
        return;
    }

    if (WIFSIGNALED(status)) {
        error << "NetLagLab: privileged helper launcher terminated; signal: "
              << WTERMSIG(status) << '\n';
        return;
    }

    error << "NetLagLab: privileged helper launcher ended with an unsupported status\n";
}

[[nodiscard]] std::optional<std::string> resolve_helper_executable_path(
    std::ostream& error)
{
    std::array<char, PATH_MAX> executable_path{};
    const ssize_t path_length{
        readlink("/proc/self/exe", executable_path.data(), executable_path.size())};

    if (path_length == -1) {
        const int readlink_error{errno};
        error << "NetLagLab: failed to resolve the executable path: "
              << std::strerror(readlink_error) << '\n';
        return std::nullopt;
    }

    if (static_cast<std::size_t>(path_length) == executable_path.size()) {
        error << "NetLagLab: executable path is too long\n";
        return std::nullopt;
    }

    const std::filesystem::path supervisor_path{
        std::string{executable_path.data(), static_cast<std::size_t>(path_length)}};
    return (supervisor_path.parent_path() / "netlaglab-helper").string();
}

} // namespace

std::optional<pid_t> spawn_helper(
    const SessionPaths& paths,
    std::ostream& error)
{
    const std::optional<std::string> helper_path{
        resolve_helper_executable_path(error)};
    if (!helper_path.has_value()) {
        return std::nullopt;
    }

    std::array<std::string, 5> arguments{
        "sudo",
        "--",
        *helper_path,
        "--runtime-dir",
        paths.xdg_runtime_directory(),
    };

    std::array<char*, 6> argument_pointers{};
    for (std::size_t index{}; index < arguments.size(); ++index) {
        argument_pointers[index] = arguments[index].data();
    }

    pid_t helper_launcher_pid{};
    const int spawn_error{posix_spawnp(
        &helper_launcher_pid,
        argument_pointers[0],
        nullptr,
        nullptr,
        argument_pointers.data(),
        environ)};

    if (spawn_error != 0) {
        error << "NetLagLab: failed to launch privileged helper: "
              << std::strerror(spawn_error) << '\n';
        return std::nullopt;
    }

    return helper_launcher_pid;
}

std::optional<FileDescriptor> wait_for_helper_connection(
    const SessionPaths& paths,
    const pid_t helper_launcher_pid,
    std::ostream& error)
{
    while (true) {
        UnixSocketConnectResult connection{
            connect_to_unix_socket(paths.helper_socket())};
        if (connection.socket.has_value()) {
            struct ucred credentials {};
            socklen_t credentials_size{sizeof(credentials)};
            if (getsockopt(
                    connection.socket->get(),
                    SOL_SOCKET,
                    SO_PEERCRED,
                    &credentials,
                    &credentials_size)
                    == -1
                || credentials_size != static_cast<socklen_t>(sizeof(credentials))
                || credentials.uid != 0
                || !belongs_to_helper_launcher(
                    credentials.pid, helper_launcher_pid)) {
                error << "NetLagLab: rejected helper.sock peer not belonging to the "
                         "launched root helper process tree\n";
                return std::nullopt;
            }
            return std::move(*connection.socket);
        }

        if (connection.failure == UnixSocketConnectFailure::path_too_long) {
            error << "NetLagLab: " << SessionPaths::helper_socket_name
                  << " path is too long\n";
            return std::nullopt;
        }

        if (connection.failure == UnixSocketConnectFailure::socket_creation) {
            error << "NetLagLab: failed to create client socket for "
                  << SessionPaths::helper_socket_name << ": "
                  << std::strerror(connection.error_code) << '\n';
            return std::nullopt;
        }

        if (connection.error_code != ENOENT
            && connection.error_code != ECONNREFUSED) {
            error << "NetLagLab: failed to connect to privileged helper: "
                  << std::strerror(connection.error_code) << '\n';
            return std::nullopt;
        }

        siginfo_t helper_info {};
        int wait_result{};
        do {
            wait_result = waitid(
                P_PID,
                static_cast<id_t>(helper_launcher_pid),
                &helper_info,
                WEXITED | WNOHANG | WNOWAIT);
        } while (wait_result == -1 && errno == EINTR);

        if (wait_result == 0 && helper_info.si_pid == helper_launcher_pid) {
            int helper_status{};
            pid_t reaped{};
            do {
                reaped = waitpid(helper_launcher_pid, &helper_status, 0);
            } while (reaped == -1 && errno == EINTR);
            if (reaped == helper_launcher_pid) {
                report_helper_exit(helper_status, error);
            } else {
                error << "NetLagLab: failed to reap privileged helper before connection\n";
            }
            return std::nullopt;
        }

        if (wait_result == -1) {
            const int wait_error{errno};
            error << "NetLagLab: failed to check privileged helper process: "
                  << std::strerror(wait_error) << '\n';
            return std::nullopt;
        }

        std::this_thread::sleep_for(helper_connection_retry_delay);
    }
}

bool reap_helper_launcher(const pid_t helper_launcher_pid, std::ostream& error)
{
    const auto wait_until = [helper_launcher_pid](
                                const std::chrono::steady_clock::time_point deadline,
                                int& status) -> std::optional<bool> {
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t result{};
            do {
                result = waitpid(helper_launcher_pid, &status, WNOHANG);
            } while (result == -1 && errno == EINTR);
            if (result == helper_launcher_pid) {
                return true;
            }
            if (result == -1) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(helper_connection_retry_delay);
        }
        return false;
    };

    int status{};
    const std::optional<bool> natural{
        wait_until(std::chrono::steady_clock::now() + std::chrono::seconds{2}, status)};
    if (!natural.has_value()) {
        error << "NetLagLab: failed to reap privileged helper launcher: "
              << std::strerror(errno) << '\n';
        return false;
    }
    if (*natural) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return true;
        }
        report_helper_exit(status, error);
        return false;
    }

    (void)kill(helper_launcher_pid, SIGTERM);
    const std::optional<bool> after_term{
        wait_until(std::chrono::steady_clock::now() + std::chrono::seconds{2}, status)};
    if (!after_term.has_value()) {
        error << "NetLagLab: failed to reap privileged helper launcher after SIGTERM\n";
        return false;
    }
    if (*after_term) {
        error << "NetLagLab: privileged helper launcher required SIGTERM during reaping\n";
        return false;
    }

    (void)kill(helper_launcher_pid, SIGKILL);
    pid_t result{};
    do {
        result = waitpid(helper_launcher_pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (result != helper_launcher_pid) {
        error << "NetLagLab: failed to reap privileged helper launcher after SIGKILL\n";
        return false;
    }
    error << "NetLagLab: privileged helper launcher required SIGKILL during reaping\n";
    return false;
}

} // namespace netlaglab
