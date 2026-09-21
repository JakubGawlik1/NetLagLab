#include "helper_process.hpp"

#include "helper_protocol.hpp"
#include "session_paths.hpp"
#include "socket_io.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits.h>
#include <ostream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace netlaglab {
namespace {

constexpr std::chrono::milliseconds helper_connection_retry_delay{50};

void report_helper_exit_before_connection(
    const int status,
    std::ostream& error)
{
    if (WIFEXITED(status)) {
        error << "NetLagLab: privileged helper exited before opening helper.sock; exit code: "
              << WEXITSTATUS(status) << '\n';
        return;
    }

    if (WIFSIGNALED(status)) {
        error << "NetLagLab: privileged helper terminated before opening helper.sock; signal: "
              << WTERMSIG(status) << '\n';
        return;
    }

    error << "NetLagLab: privileged helper ended with an unsupported status before opening "
             "helper.sock\n";
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

        int helper_status{};
        pid_t wait_result{};
        do {
            wait_result = waitpid(helper_launcher_pid, &helper_status, WNOHANG);
        } while (wait_result == -1 && errno == EINTR);

        if (wait_result == helper_launcher_pid) {
            report_helper_exit_before_connection(helper_status, error);
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

bool wait_for_helper_ready(
    const int helper_socket_descriptor,
    std::ostream& error)
{
    std::string read_buffer;

    while (true) {
        const SocketReadResult result{
            read_socket_data(helper_socket_descriptor, read_buffer)};

        if (result.status == SocketReadStatus::error) {
            error << "NetLagLab: failed to read from privileged helper: "
                  << std::strerror(result.error_code) << '\n';
            return false;
        }

        if (result.status == SocketReadStatus::peer_closed) {
            error << "NetLagLab: privileged helper closed the connection before sending "
                     "READY\n";
            return false;
        }

        while (true) {
            const std::optional<std::string> line{take_next_line(read_buffer)};
            if (!line.has_value()) {
                break;
            }

            if (line->size() > maximum_helper_message_size) {
                error << "NetLagLab: message from privileged helper exceeds "
                      << maximum_helper_message_size << " bytes\n";
                return false;
            }

            const std::optional<HelperEvent> event{parse_helper_event(*line)};
            if (event == HelperEvent::ready) {
                return true;
            }

            error << "NetLagLab: expected READY from privileged helper, received: "
                  << *line << '\n';
            return false;
        }

        if (read_buffer.size() > maximum_helper_message_size) {
            error << "NetLagLab: message from privileged helper exceeds "
                  << maximum_helper_message_size << " bytes\n";
            return false;
        }
    }
}

} // namespace netlaglab
