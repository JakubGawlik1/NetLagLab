#include "session.hpp"
#include "file_descriptor.hpp"
#include "helper_process.hpp"
#include "session_paths.hpp"
#include "session_socket.hpp"
#include "session_validation.hpp"
#include "socket_io.hpp"

#include "netlaglab/network_profile.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <ostream>
#include <poll.h>
#include <sstream>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace netlaglab {
namespace {

constexpr int poll_timeout_ms{100};
constexpr std::size_t maximum_command_size{1024};
constexpr std::size_t maximum_status_size{8 * 1024};

[[nodiscard]] int spawn_error_exit_code(const int error_code)
{
    if (error_code == ENOENT) {
        return 127;
    }

    if (error_code == EACCES || error_code == ENOEXEC) {
        return 126;
    }

    return 125;
}

[[nodiscard]] int child_exit_code(const int status, std::ostream& error)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    error << "NetLagLab: child process ended with an unsupported status\n";
    return 125;
}

[[nodiscard]] int wait_for_child(const pid_t child_pid, std::ostream& error)
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

    return child_exit_code(status, error);
}

[[nodiscard]] bool append_escaped_limited(
    std::string& output,
    const std::string_view input,
    const std::size_t limit)
{
    constexpr std::string_view hex_digits{"0123456789ABCDEF"};

    const auto append_piece = [&output, limit](const std::string_view piece) {
        if (output.size() > limit || piece.size() > limit - output.size()) {
            return false;
        }

        output.append(piece);
        return true;
    };

    for (const unsigned char byte : input) {
        if (byte == '\\') {
            if (!append_piece("\\\\")) {
                return false;
            }
        } else if (byte == '\t') {
            if (!append_piece("\\t")) {
                return false;
            }
        } else if (byte == '\n') {
            if (!append_piece("\\n")) {
                return false;
            }
        } else if (byte == '\r') {
            if (!append_piece("\\r")) {
                return false;
            }
        } else if (byte < 0x20 || byte == 0x7F) {
            const std::array<char, 4> escaped{
                '\\',
                'x',
                hex_digits[byte >> 4],
                hex_digits[byte & 0x0F],
            };
            if (!append_piece(std::string_view{escaped.data(), escaped.size()})) {
                return false;
            }
        } else {
            const char character{static_cast<char>(byte)};
            if (!append_piece(std::string_view{&character, 1})) {
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] std::string escape_command(const std::string_view command)
{
    std::string escaped;
    escaped.reserve(command.size());
    (void)append_escaped_limited(escaped, command, maximum_command_size * 4);
    return escaped;
}

void append_direction_status(
    std::ostringstream& output,
    const std::string_view name,
    const DirectionSettings& settings)
{
    output << name << ":\n"
           << "  delay: " << settings.delay.count() << " ms\n"
           << "  jitter: " << settings.jitter.count() << " ms\n"
           << "  packet loss: " << settings.packet_loss_percent << "%\n"
           << "  bandwidth: ";

    if (settings.bandwidth_kbps.has_value()) {
        output << *settings.bandwidth_kbps << " kbps\n";
    } else {
        output << "unlimited\n";
    }
}

[[nodiscard]] std::string build_status(
    const pid_t child_pid,
    char* const child_arguments[])
{
    const NetworkProfile profile{};

    std::ostringstream tail_stream;
    tail_stream << "shaping: not applied\n";
    append_direction_status(tail_stream, "outbound", profile.outbound);
    append_direction_status(tail_stream, "inbound", profile.inbound);
    tail_stream << "STATUS_END\n";
    const std::string tail{tail_stream.str()};

    constexpr std::string_view truncated_notice{"arguments: truncated\n"};
    const std::size_t content_limit{
        maximum_status_size - tail.size() - truncated_notice.size()};

    std::ostringstream header_stream;
    header_stream << "STATUS_BEGIN\n"
                  << "state: running\n"
                  << "pid: " << child_pid << '\n'
                  << "program: ";
    std::string response{header_stream.str()};

    bool truncated{false};
    if (!append_escaped_limited(response, child_arguments[0], content_limit - 1)) {
        truncated = true;
    }
    response.push_back('\n');

    if (!truncated) {
        for (std::size_t source_index{1}; child_arguments[source_index] != nullptr;
             ++source_index) {
            const std::size_t argument_index{source_index - 1};
            const std::string prefix{
                "argument[" + std::to_string(argument_index) + "]: "};

            if (response.size() + prefix.size() + 1 > content_limit) {
                truncated = true;
                break;
            }

            response.append(prefix);
            if (!append_escaped_limited(
                    response, child_arguments[source_index], content_limit - 1)) {
                truncated = true;
                response.push_back('\n');
                break;
            }
            response.push_back('\n');
        }
    }

    if (truncated) {
        response.append(truncated_notice);
    }
    response.append(tail);
    return response;
}

enum class CommandResult {
    keep_connected,
    disconnect,
};

[[nodiscard]] CommandResult handle_command(
    const int client_descriptor,
    const std::string_view command,
    const pid_t child_pid,
    char* const child_arguments[])
{
    if (command == "help") {
        constexpr std::string_view help_response{
            "HELP_BEGIN\n"
            "help - Show controller commands\n"
            "status - Show the active session\n"
            "detach - Disconnect this controller\n"
            "HELP_END\n"};
        return send_socket_text(client_descriptor, help_response)
            ? CommandResult::keep_connected
            : CommandResult::disconnect;
    }

    if (command == "status") {
        const std::string status{build_status(child_pid, child_arguments)};
        return send_socket_text(client_descriptor, status) ? CommandResult::keep_connected
                                                           : CommandResult::disconnect;
    }

    if (command == "detach") {
        (void)send_socket_text(client_descriptor, "DETACHED\n");
        return CommandResult::disconnect;
    }

    const std::string response{"ERROR Unknown command: " + escape_command(command) + '\n'};
    return send_socket_text(client_descriptor, response) ? CommandResult::keep_connected
                                                         : CommandResult::disconnect;
}

[[nodiscard]] bool read_client_commands(
    const int client_descriptor,
    std::string& command_buffer,
    const pid_t child_pid,
    char* const child_arguments[])
{
    const SocketReadResult read_result{read_socket_data(client_descriptor, command_buffer)};
    if (read_result.status != SocketReadStatus::data_received) {
        return false;
    }

    while (true) {
        const std::optional<std::string> command{take_next_line(command_buffer)};
        if (!command.has_value()) {
            break;
        }

        if (command->size() > maximum_command_size) {
            (void)send_socket_text(
                client_descriptor, "ERROR Command exceeds 1024 bytes.\n");
            return false;
        }

        if (command->empty()) {
            continue;
        }

        if (handle_command(client_descriptor, *command, child_pid, child_arguments)
            == CommandResult::disconnect) {
            return false;
        }
    }

    if (command_buffer.size() > maximum_command_size) {
        (void)send_socket_text(client_descriptor, "ERROR Command exceeds 1024 bytes.\n");
        return false;
    }

    return true;
}

[[nodiscard]] bool accept_controller(
    const int listening_descriptor,
    std::optional<FileDescriptor>& client,
    std::string& command_buffer,
    std::ostream& error)
{
    int accepted_descriptor{};
    do {
        accepted_descriptor = accept4(listening_descriptor, nullptr, nullptr, SOCK_CLOEXEC);
    } while (accepted_descriptor == -1 && errno == EINTR);

    if (accepted_descriptor == -1) {
        const int accept_error{errno};
        error << "NetLagLab: failed to accept controller: " << std::strerror(accept_error) << '\n';
        return false;
    }

    if (client.has_value()) {
        FileDescriptor rejected_client{accepted_descriptor};
        (void)send_socket_text(
            rejected_client.get(), "ERROR Another controller is already attached.\n");
        return true;
    }

    client.emplace(accepted_descriptor);
    command_buffer.clear();
    if (!send_socket_text(client->get(), "ATTACHED\n")) {
        client.reset();
    }

    return true;
}

void print_terminal_summary(const int child_status, std::ostream& error)
{
    if (isatty(STDERR_FILENO) != 1) {
        return;
    }

    if (WIFEXITED(child_status)) {
        error << "NetLagLab: session ended; application exit code: "
              << WEXITSTATUS(child_status) << '\n';
    } else if (WIFSIGNALED(child_status)) {
        error << "NetLagLab: session ended; application terminated by signal: "
              << WTERMSIG(child_status) << '\n';
    }
}

[[nodiscard]] int finish_session(
    const int child_status,
    FileDescriptor& listening_socket,
    SocketPathOwner& socket_path_owner,
    std::optional<FileDescriptor>& client,
    std::ostream& error)
{
    const int application_exit_code{child_exit_code(child_status, error)};
    listening_socket.reset();

    const int cleanup_error{socket_path_owner.remove_owned()};
    if (cleanup_error != 0) {
        if (client.has_value()) {
            (void)send_socket_text(client->get(), "SESSION_FAILED\n");
        }
        client.reset();
        error << "NetLagLab: application exited with status " << application_exit_code
              << ", but control.sock could not be removed: "
              << std::strerror(cleanup_error) << '\n';
        return 125;
    }

    if (client.has_value()) {
        (void)send_socket_text(client->get(), "SESSION_ENDED\n");
    }
    client.reset();
    print_terminal_summary(child_status, error);
    return application_exit_code;
}

void close_control_channel_after_supervisor_error(
    FileDescriptor& listening_socket,
    SocketPathOwner& socket_path_owner,
    std::optional<FileDescriptor>& client,
    std::ostream& error)
{
    listening_socket.reset();
    const int cleanup_error{socket_path_owner.remove_owned()};
    if (cleanup_error != 0) {
        error << "NetLagLab: failed to remove control.sock after supervisor error: "
              << std::strerror(cleanup_error) << '\n';
    }

    if (client.has_value()) {
        (void)send_socket_text(client->get(), "SESSION_FAILED\n");
    }
    client.reset();
}

[[nodiscard]] int wait_after_supervisor_error(
    const pid_t child_pid,
    FileDescriptor& listening_socket,
    SocketPathOwner& socket_path_owner,
    std::optional<FileDescriptor>& client,
    std::ostream& error)
{
    close_control_channel_after_supervisor_error(
        listening_socket, socket_path_owner, client, error);
    (void)wait_for_child(child_pid, error);
    return 125;
}

[[nodiscard]] int supervise_child(
    const pid_t child_pid,
    char* const child_arguments[],
    FileDescriptor& listening_socket,
    SocketPathOwner& socket_path_owner,
    std::ostream& error)
{
    std::optional<FileDescriptor> client;
    std::string command_buffer;

    while (true) {
        std::array<struct pollfd, 2> descriptors{{
            {listening_socket.get(), POLLIN, 0},
            {client.has_value() ? client->get() : -1, POLLIN, 0},
        }};

        const int poll_result{poll(descriptors.data(), descriptors.size(), poll_timeout_ms)};
        if (poll_result == -1 && errno != EINTR) {
            const int poll_error{errno};
            error << "NetLagLab: poll failed: " << std::strerror(poll_error) << '\n';
            return wait_after_supervisor_error(
                child_pid, listening_socket, socket_path_owner, client, error);
        }

        if (poll_result > 0) {
            const short client_events{descriptors[1].revents};
            if (client.has_value() && (client_events & POLLIN) != 0) {
                if (!read_client_commands(
                        client->get(), command_buffer, child_pid, child_arguments)) {
                    client.reset();
                    command_buffer.clear();
                }
            }

            if (client.has_value()
                && (client_events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                client.reset();
                command_buffer.clear();
            }

            const short listening_events{descriptors[0].revents};
            if ((listening_events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                error << "NetLagLab: listening socket reported an error\n";
                return wait_after_supervisor_error(
                    child_pid, listening_socket, socket_path_owner, client, error);
            }

            if ((listening_events & POLLIN) != 0
                && !accept_controller(
                    listening_socket.get(), client, command_buffer, error)) {
                return wait_after_supervisor_error(
                    child_pid, listening_socket, socket_path_owner, client, error);
            }
        }

        int child_status{};
        const pid_t wait_result{waitpid(child_pid, &child_status, WNOHANG)};
        if (wait_result == child_pid) {
            return finish_session(
                child_status, listening_socket, socket_path_owner, client, error);
        }

        if (wait_result == -1 && errno != EINTR) {
            const int wait_error{errno};
            error << "NetLagLab: failed to check child process: "
                  << std::strerror(wait_error) << '\n';
            close_control_channel_after_supervisor_error(
                listening_socket, socket_path_owner, client, error);
            return 125;
        }
    }
}

} // namespace

int run_session(char* const child_arguments[], std::ostream& error)
{
    const std::optional<SessionPaths> paths{
        make_session_paths_from_environment(error)};
    if (!paths.has_value()) {
        return 125;
    }

    const std::optional<FileDescriptor> runtime_directory{
        open_and_validate_runtime_directory(
            paths->xdg_runtime_directory(), geteuid(), error)};
    if (!runtime_directory.has_value()) {
        return 125;
    }

    const int mkdir_result{mkdirat(
        runtime_directory->get(), SessionPaths::session_directory_name.data(), 0700)};
    if (mkdir_result == -1 && errno != EEXIST) {
        const int mkdir_error{errno};
        error << "NetLagLab: failed to create the netlaglab runtime directory: "
              << std::strerror(mkdir_error) << '\n';
        return 125;
    }

    if (mkdir_result == 0
        && fchmodat(
               runtime_directory->get(), SessionPaths::session_directory_name.data(), 0700, 0)
            == -1) {
        const int chmod_error{errno};
        error << "NetLagLab: failed to set permissions on the netlaglab runtime directory: "
              << std::strerror(chmod_error) << '\n';
        return 125;
    }

    const std::optional<FileDescriptor> session_directory{
        open_and_validate_session_directory(runtime_directory->get(), geteuid(), error)};
    if (!session_directory.has_value()) {
        return 125;
    }

    const int lock_descriptor{openat(
        session_directory->get(),
        SessionPaths::lock_file_name.data(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        0600)};
    if (lock_descriptor == -1) {
        const int open_error{errno};
        error << "NetLagLab: failed to open session.lock: " << std::strerror(open_error) << '\n';
        return 125;
    }
    const FileDescriptor session_lock{lock_descriptor};

    struct stat lock_status {};
    if (fstat(session_lock.get(), &lock_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab: failed to inspect session.lock: " << std::strerror(status_error)
              << '\n';
        return 125;
    }

    if (!S_ISREG(lock_status.st_mode) || lock_status.st_uid != geteuid()) {
        error << "NetLagLab: session.lock must be a regular file owned by the current user\n";
        return 125;
    }

    if (fchmod(session_lock.get(), 0600) == -1) {
        const int chmod_error{errno};
        error << "NetLagLab: failed to set permissions on session.lock: "
              << std::strerror(chmod_error) << '\n';
        return 125;
    }

    if (flock(session_lock.get(), LOCK_EX | LOCK_NB) == -1) {
        const int lock_error{errno};
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
            error << "NetLagLab: another session is already active\n";
        } else {
            error << "NetLagLab: failed to lock session.lock: " << std::strerror(lock_error)
                  << '\n';
        }
        return 125;
    }

    const std::optional<pid_t> helper_launcher_pid{spawn_helper(*paths, error)};
    if (!helper_launcher_pid.has_value()) {
        return 125;
    }

    const std::optional<FileDescriptor> helper_connection_descriptor{
        wait_for_helper_connection(*paths, *helper_launcher_pid, error)};
    if (!helper_connection_descriptor.has_value()) {
        return 125;
    }

    if (!wait_for_helper_ready(helper_connection_descriptor->get(), error)) {
        return 125;
    }

    SocketPathOwner control_socket_path_owner{
        session_directory->get(),
        SessionPaths::control_socket_name,
        paths->control_socket(),
        geteuid()};
    if (!control_socket_path_owner.remove_stale(error)) {
        return 125;
    }

    const int listening_descriptor{
        control_socket_path_owner.create_listening_socket(error)};
    if (listening_descriptor == -1) {
        const int cleanup_error{control_socket_path_owner.remove_owned()};
        if (cleanup_error != 0) {
            error << "NetLagLab: failed to remove control.sock after listener setup failure: "
                  << std::strerror(cleanup_error) << '\n';
        }
        return 125;
    }
    FileDescriptor listening_socket{listening_descriptor};

    pid_t child_pid{};
    const int spawn_error{
        posix_spawnp(&child_pid, child_arguments[0], nullptr, nullptr, child_arguments, environ)};

    if (spawn_error != 0) {
        error << "NetLagLab: failed to launch '" << child_arguments[0]
              << "': " << std::strerror(spawn_error) << '\n';
        listening_socket.reset();
        const int cleanup_error{control_socket_path_owner.remove_owned()};
        if (cleanup_error != 0) {
            error << "NetLagLab: failed to remove control.sock after launch failure: "
                  << std::strerror(cleanup_error) << '\n';
        }
        return spawn_error_exit_code(spawn_error);
    }

    return supervise_child(
        child_pid,
        child_arguments,
        listening_socket,
        control_socket_path_owner,
        error);
}

} // namespace netlaglab
