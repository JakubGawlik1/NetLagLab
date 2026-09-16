#include "attach_client.hpp"

#include "file_descriptor.hpp"
#include "session_paths.hpp"
#include "session_validation.hpp"
#include "socket_io.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <ostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace netlaglab {
namespace {

constexpr std::size_t maximum_status_size{8 * 1024};

[[nodiscard]] bool validate_control_socket(
    const int session_directory_descriptor,
    std::ostream& error)
{
    struct stat socket_status {};
    if (fstatat(
            session_directory_descriptor,
            SessionPaths::control_socket_name.data(),
            &socket_status,
            AT_SYMLINK_NOFOLLOW)
        == -1) {
        const int status_error{errno};
        if (status_error == ENOENT) {
            error << "NetLagLab: no active session\n";
        } else {
            error << "NetLagLab: failed to inspect control.sock: "
                  << std::strerror(status_error) << '\n';
        }
        return false;
    }

    if (!S_ISSOCK(socket_status.st_mode)) {
        error << "NetLagLab: control.sock is not a socket\n";
        return false;
    }

    if (socket_status.st_uid != geteuid()) {
        error << "NetLagLab: control.sock is owned by another user\n";
        return false;
    }

    if ((socket_status.st_mode & 0777) != 0600) {
        error << "NetLagLab: control.sock must have permissions 0600\n";
        return false;
    }

    return true;
}

[[nodiscard]] int connect_to_control_socket(
    const std::string& socket_path,
    std::ostream& error)
{
    struct sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path)) {
        error << "NetLagLab: " << SessionPaths::control_socket_name << " path is too long\n";
        return -1;
    }

    const int raw_socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (raw_socket == -1) {
        const int socket_error{errno};
        error << "NetLagLab: failed to create client socket for "
              << SessionPaths::control_socket_name << ": "
              << std::strerror(socket_error) << '\n';
        return -1;
    }
    FileDescriptor client_socket{raw_socket};

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const auto address_size{static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1)};

    if (connect(
            client_socket.get(),
            reinterpret_cast<const struct sockaddr*>(&address),
            address_size)
        == -1) {
        const int connect_error{errno};
        if (connect_error == ENOENT || connect_error == ECONNREFUSED) {
            error << "NetLagLab: no active session\n";
        } else {
            error << "NetLagLab: failed to connect to session: "
                  << std::strerror(connect_error) << '\n';
        }
        return -1;
    }

    return client_socket.release();
}

enum class ResponseBlock {
    none,
    help,
    status,
};

[[nodiscard]] std::optional<int> handle_supervisor_response_line(
    const std::string_view line,
    ResponseBlock& response_block,
    std::ostream& output,
    std::ostream& error)
{
    if (response_block == ResponseBlock::help) {
        if (line == "HELP_END") {
            response_block = ResponseBlock::none;
        } else {
            output << line << '\n';
        }
        return std::nullopt;
    }

    if (response_block == ResponseBlock::status) {
        if (line == "STATUS_END") {
            response_block = ResponseBlock::none;
        } else {
            output << line << '\n';
        }
        return std::nullopt;
    }

    if (line == "ATTACHED") {
        return std::nullopt;
    }

    if (line == "HELP_BEGIN") {
        response_block = ResponseBlock::help;
        return std::nullopt;
    }

    if (line == "STATUS_BEGIN") {
        response_block = ResponseBlock::status;
        return std::nullopt;
    }

    if (line == "DETACHED") {
        return 0;
    }

    if (line == "SESSION_ENDED") {
        output << "Session ended.\n";
        return 0;
    }

    if (line == "SESSION_FAILED") {
        error << "NetLagLab: session failed\n";
        return 1;
    }

    constexpr std::string_view error_prefix{"ERROR "};
    if (line.starts_with(error_prefix)) {
        const std::string_view message{line.substr(error_prefix.size())};
        error << message << '\n';

        if (message == "Another controller is already attached.") {
            return 1;
        }
        return std::nullopt;
    }

    error << "NetLagLab: invalid response from session\n";
    return 1;
}

[[nodiscard]] std::optional<int> read_and_handle_supervisor_responses(
    const int socket_descriptor,
    std::string& response_buffer,
    ResponseBlock& response_block,
    std::ostream& output,
    std::ostream& error)
{
    const SocketReadResult read_result{read_socket_data(socket_descriptor, response_buffer)};
    if (read_result.status == SocketReadStatus::peer_closed) {
        error << "NetLagLab: connection to session lost; session result is unknown.\n";
        return 1;
    }

    if (read_result.status == SocketReadStatus::error) {
        error << "NetLagLab: failed to read from session: "
              << std::strerror(read_result.error_code) << '\n';
        return 1;
    }

    while (true) {
        const std::optional<std::string> line{take_next_line(response_buffer)};
        if (!line.has_value()) {
            break;
        }

        const std::optional<int> line_result{
            handle_supervisor_response_line(*line, response_block, output, error)};
        if (line_result.has_value()) {
            return line_result;
        }
    }

    if (response_buffer.size() > maximum_status_size) {
        error << "NetLagLab: response from session exceeds 8 KiB\n";
        return 1;
    }

    output.flush();
    error.flush();
    return std::nullopt;
}

[[nodiscard]] int run_attached_controller(
    const int socket_descriptor,
    std::ostream& output,
    std::ostream& error)
{
    std::string response_buffer;
    ResponseBlock response_block{ResponseBlock::none};
    bool read_stdin{true};
    bool input_ends_with_newline{true};

    while (true) {
        std::array<struct pollfd, 2> descriptors{{
            {read_stdin ? STDIN_FILENO : -1, POLLIN, 0},
            {socket_descriptor, POLLIN, 0},
        }};

        const int poll_result{poll(descriptors.data(), descriptors.size(), -1)};
        if (poll_result == -1) {
            if (errno == EINTR) {
                continue;
            }

            const int poll_error{errno};
            error << "NetLagLab: poll failed: " << std::strerror(poll_error) << '\n';
            return 1;
        }

        const short socket_events{descriptors[1].revents};
        if ((socket_events & (POLLIN | POLLHUP)) != 0) {
            const std::optional<int> response_result{
                read_and_handle_supervisor_responses(
                    socket_descriptor,
                    response_buffer,
                    response_block,
                    output,
                    error)};
            if (response_result.has_value()) {
                return *response_result;
            }
        }

        if ((socket_events & (POLLERR | POLLNVAL)) != 0) {
            error << "NetLagLab: connection to session lost; session result is unknown.\n";
            return 1;
        }

        const short stdin_events{descriptors[0].revents};
        if (read_stdin && (stdin_events & (POLLIN | POLLHUP)) != 0) {
            std::array<char, 4096> input_buffer{};
            const ssize_t read_size{read(STDIN_FILENO, input_buffer.data(), input_buffer.size())};

            if (read_size > 0) {
                const std::string_view input{
                    input_buffer.data(), static_cast<std::size_t>(read_size)};
                if (!send_socket_text(socket_descriptor, input)) {
                    error << "NetLagLab: connection to session lost; session result is unknown.\n";
                    return 1;
                }
                input_ends_with_newline = input.back() == '\n';
            } else if (read_size == 0) {
                if (!input_ends_with_newline
                    && !send_socket_text(socket_descriptor, "\n")) {
                    error << "NetLagLab: connection to session lost; session result is unknown.\n";
                    return 1;
                }

                if (!send_socket_text(socket_descriptor, "detach\n")) {
                    error << "NetLagLab: connection to session lost; session result is unknown.\n";
                    return 1;
                }
                read_stdin = false;
            } else if (errno != EINTR) {
                const int read_error{errno};
                error << "NetLagLab: failed to read stdin: " << std::strerror(read_error) << '\n';
                return 1;
            }
        }

        if (read_stdin && (stdin_events & (POLLERR | POLLNVAL)) != 0) {
            error << "NetLagLab: stdin is not readable\n";
            return 1;
        }
    }
}

} // namespace

int attach_to_session(std::ostream& output, std::ostream& error)
{
    const std::optional<SessionPaths> paths{
        make_session_paths_from_environment(error)};
    if (!paths.has_value()) {
        return 1;
    }

    const std::optional<FileDescriptor> runtime_directory{
        open_and_validate_runtime_directory(
            paths->xdg_runtime_directory(), geteuid(), error)};
    if (!runtime_directory.has_value()) {
        return 1;
    }

    const std::optional<FileDescriptor> session_directory{
        open_and_validate_session_directory(runtime_directory->get(), geteuid(), error)};
    if (!session_directory.has_value()
        || !validate_control_socket(session_directory->get(), error)) {
        return 1;
    }

    const int connected_descriptor{
        connect_to_control_socket(paths->control_socket(), error)};
    if (connected_descriptor == -1) {
        return 1;
    }
    const FileDescriptor client_socket{connected_descriptor};

    return run_attached_controller(client_socket.get(), output, error);
}

} // namespace netlaglab
