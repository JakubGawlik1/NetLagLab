#include "session.hpp"

#include "netlaglab/network_profile.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
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
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace netlaglab {
namespace {

constexpr std::string_view session_directory_name{"netlaglab"};
constexpr std::string_view lock_file_name{"session.lock"};
constexpr std::string_view control_socket_name{"control.sock"};
constexpr int poll_timeout_ms{100};
constexpr int listen_backlog{1};
constexpr std::size_t maximum_command_size{1024};
constexpr std::size_t maximum_status_size{8 * 1024};

class FileDescriptor {
public:
    explicit FileDescriptor(const int descriptor)
        : descriptor_{descriptor}
    {
    }

    ~FileDescriptor()
    {
        reset();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const
    {
        return descriptor_;
    }

    void reset()
    {
        if (descriptor_ != -1) {
            close(descriptor_);
            descriptor_ = -1;
        }
    }

    [[nodiscard]] int release()
    {
        const int released_descriptor{descriptor_};
        descriptor_ = -1;
        return released_descriptor;
    }

private:
    int descriptor_;
};

class ControlSocketPath {
public:
    explicit ControlSocketPath(const int session_directory_descriptor)
        : session_directory_descriptor_{session_directory_descriptor}
    {
    }

    ~ControlSocketPath()
    {
        (void)remove_owned();
    }

    ControlSocketPath(const ControlSocketPath&) = delete;
    ControlSocketPath& operator=(const ControlSocketPath&) = delete;

    [[nodiscard]] bool remove_stale(std::ostream& error) const
    {
        struct stat socket_status {};
        if (fstatat(
                session_directory_descriptor_,
                control_socket_name.data(),
                &socket_status,
                AT_SYMLINK_NOFOLLOW)
            == -1) {
            if (errno == ENOENT) {
                return true;
            }

            const int status_error{errno};
            error << "NetLagLab: failed to inspect control.sock: "
                  << std::strerror(status_error) << '\n';
            return false;
        }

        if (!S_ISSOCK(socket_status.st_mode)) {
            error << "NetLagLab: refusing to remove control.sock because it is not a socket\n";
            return false;
        }

        if (socket_status.st_uid != geteuid()) {
            error << "NetLagLab: refusing to remove control.sock owned by another user\n";
            return false;
        }

        if (unlinkat(session_directory_descriptor_, control_socket_name.data(), 0) == -1) {
            const int unlink_error{errno};
            error << "NetLagLab: failed to remove stale control.sock: "
                  << std::strerror(unlink_error) << '\n';
            return false;
        }

        return true;
    }

    void mark_owned()
    {
        owned_ = true;
    }

    [[nodiscard]] int remove_owned()
    {
        if (!owned_) {
            return 0;
        }

        if (unlinkat(session_directory_descriptor_, control_socket_name.data(), 0) == 0) {
            owned_ = false;
            return 0;
        }

        const int unlink_error{errno};
        if (unlink_error == ENOENT) {
            owned_ = false;
            return 0;
        }

        return unlink_error;
    }

private:
    int session_directory_descriptor_;
    bool owned_{false};
};

[[nodiscard]] bool validate_runtime_directory(const int descriptor, std::ostream& error)
{
    struct stat directory_status {};
    if (fstat(descriptor, &directory_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab: failed to inspect XDG_RUNTIME_DIR: " << std::strerror(status_error)
              << '\n';
        return false;
    }

    if (!S_ISDIR(directory_status.st_mode)) {
        error << "NetLagLab: XDG_RUNTIME_DIR is not a directory\n";
        return false;
    }

    if (directory_status.st_uid != geteuid()) {
        error << "NetLagLab: XDG_RUNTIME_DIR is not owned by the current user\n";
        return false;
    }

    if ((directory_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        error << "NetLagLab: XDG_RUNTIME_DIR must not be accessible by group or other users\n";
        return false;
    }

    return true;
}

[[nodiscard]] bool validate_session_directory(const int descriptor, std::ostream& error)
{
    struct stat directory_status {};
    if (fstat(descriptor, &directory_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab: failed to inspect the netlaglab runtime directory: "
              << std::strerror(status_error) << '\n';
        return false;
    }

    if (!S_ISDIR(directory_status.st_mode)) {
        error << "NetLagLab: the netlaglab runtime path is not a directory\n";
        return false;
    }

    if (directory_status.st_uid != geteuid()) {
        error << "NetLagLab: the netlaglab runtime directory is owned by another user\n";
        return false;
    }

    if ((directory_status.st_mode & 0777) != 0700) {
        error << "NetLagLab: the netlaglab runtime directory must have permissions 0700\n";
        return false;
    }

    return true;
}

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

[[nodiscard]] std::string make_control_socket_path(const std::string_view runtime_path)
{
    std::string path{runtime_path};
    if (path.back() != '/') {
        path.push_back('/');
    }

    path.append(session_directory_name);
    path.push_back('/');
    path.append(control_socket_name);
    return path;
}

[[nodiscard]] int create_listening_socket(
    const std::string& socket_path,
    const int session_directory_descriptor,
    ControlSocketPath& socket_path_owner,
    std::ostream& error)
{
    struct sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path)) {
        error << "NetLagLab: control socket path is too long\n";
        return -1;
    }

    const int raw_socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (raw_socket == -1) {
        const int socket_error{errno};
        error << "NetLagLab: failed to create control socket: " << std::strerror(socket_error)
              << '\n';
        return -1;
    }
    FileDescriptor listening_socket{raw_socket};

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const auto address_size{static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1)};

    if (bind(
            listening_socket.get(),
            reinterpret_cast<const struct sockaddr*>(&address),
            address_size)
        == -1) {
        const int bind_error{errno};
        error << "NetLagLab: failed to bind control socket: " << std::strerror(bind_error) << '\n';
        return -1;
    }
    socket_path_owner.mark_owned();

    if (fchmodat(session_directory_descriptor, control_socket_name.data(), 0600, 0) == -1) {
        const int chmod_error{errno};
        error << "NetLagLab: failed to set permissions on control.sock: "
              << std::strerror(chmod_error) << '\n';
        return -1;
    }

    if (listen(listening_socket.get(), listen_backlog) == -1) {
        const int listen_error{errno};
        error << "NetLagLab: failed to listen on control socket: "
              << std::strerror(listen_error) << '\n';
        return -1;
    }

    return listening_socket.release();
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

[[nodiscard]] bool send_text(const int socket_descriptor, const std::string_view message)
{
    std::size_t sent_size{};
    while (sent_size < message.size()) {
        const ssize_t result{send(
            socket_descriptor,
            message.data() + sent_size,
            message.size() - sent_size,
            MSG_NOSIGNAL)};

        if (result > 0) {
            sent_size += static_cast<std::size_t>(result);
            continue;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
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
        return send_text(client_descriptor, help_response) ? CommandResult::keep_connected
                                                           : CommandResult::disconnect;
    }

    if (command == "status") {
        const std::string status{build_status(child_pid, child_arguments)};
        return send_text(client_descriptor, status) ? CommandResult::keep_connected
                                                    : CommandResult::disconnect;
    }

    if (command == "detach") {
        (void)send_text(client_descriptor, "DETACHED\n");
        return CommandResult::disconnect;
    }

    const std::string response{"ERROR Unknown command: " + escape_command(command) + '\n'};
    return send_text(client_descriptor, response) ? CommandResult::keep_connected
                                                  : CommandResult::disconnect;
}

[[nodiscard]] bool read_client_commands(
    const int client_descriptor,
    std::string& command_buffer,
    const pid_t child_pid,
    char* const child_arguments[])
{
    std::array<char, 4096> read_buffer{};
    const ssize_t read_size{read(client_descriptor, read_buffer.data(), read_buffer.size())};

    if (read_size == 0) {
        return false;
    }

    if (read_size == -1) {
        return errno == EINTR;
    }

    command_buffer.append(read_buffer.data(), static_cast<std::size_t>(read_size));

    while (true) {
        const std::size_t newline_position{command_buffer.find('\n')};
        if (newline_position == std::string::npos) {
            break;
        }

        if (newline_position > maximum_command_size) {
            (void)send_text(client_descriptor, "ERROR Command exceeds 1024 bytes.\n");
            return false;
        }

        const std::string command{command_buffer.substr(0, newline_position)};
        command_buffer.erase(0, newline_position + 1);

        if (command.empty()) {
            continue;
        }

        if (handle_command(client_descriptor, command, child_pid, child_arguments)
            == CommandResult::disconnect) {
            return false;
        }
    }

    if (command_buffer.size() > maximum_command_size) {
        (void)send_text(client_descriptor, "ERROR Command exceeds 1024 bytes.\n");
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
        (void)send_text(
            rejected_client.get(), "ERROR Another controller is already attached.\n");
        return true;
    }

    client.emplace(accepted_descriptor);
    command_buffer.clear();
    if (!send_text(client->get(), "ATTACHED\n")) {
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
    ControlSocketPath& socket_path_owner,
    std::optional<FileDescriptor>& client,
    std::ostream& error)
{
    const int application_exit_code{child_exit_code(child_status, error)};
    listening_socket.reset();

    const int cleanup_error{socket_path_owner.remove_owned()};
    if (cleanup_error != 0) {
        if (client.has_value()) {
            (void)send_text(client->get(), "SESSION_FAILED\n");
        }
        client.reset();
        error << "NetLagLab: application exited with status " << application_exit_code
              << ", but control.sock could not be removed: "
              << std::strerror(cleanup_error) << '\n';
        return 125;
    }

    if (client.has_value()) {
        (void)send_text(client->get(), "SESSION_ENDED\n");
    }
    client.reset();
    print_terminal_summary(child_status, error);
    return application_exit_code;
}

void close_control_channel_after_supervisor_error(
    FileDescriptor& listening_socket,
    ControlSocketPath& socket_path_owner,
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
        (void)send_text(client->get(), "SESSION_FAILED\n");
    }
    client.reset();
}

[[nodiscard]] int wait_after_supervisor_error(
    const pid_t child_pid,
    FileDescriptor& listening_socket,
    ControlSocketPath& socket_path_owner,
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
    ControlSocketPath& socket_path_owner,
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
    const char* const runtime_path{std::getenv("XDG_RUNTIME_DIR")};
    if (runtime_path == nullptr || runtime_path[0] == '\0') {
        error << "NetLagLab: XDG_RUNTIME_DIR is not set\n";
        return 125;
    }

    if (runtime_path[0] != '/') {
        error << "NetLagLab: XDG_RUNTIME_DIR must be an absolute path\n";
        return 125;
    }

    const int runtime_descriptor{
        open(runtime_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (runtime_descriptor == -1) {
        const int open_error{errno};
        error << "NetLagLab: failed to open XDG_RUNTIME_DIR: " << std::strerror(open_error)
              << '\n';
        return 125;
    }
    const FileDescriptor runtime_directory{runtime_descriptor};

    if (!validate_runtime_directory(runtime_directory.get(), error)) {
        return 125;
    }

    const int mkdir_result{mkdirat(runtime_directory.get(), session_directory_name.data(), 0700)};
    if (mkdir_result == -1 && errno != EEXIST) {
        const int mkdir_error{errno};
        error << "NetLagLab: failed to create the netlaglab runtime directory: "
              << std::strerror(mkdir_error) << '\n';
        return 125;
    }

    if (mkdir_result == 0
        && fchmodat(runtime_directory.get(), session_directory_name.data(), 0700, 0) == -1) {
        const int chmod_error{errno};
        error << "NetLagLab: failed to set permissions on the netlaglab runtime directory: "
              << std::strerror(chmod_error) << '\n';
        return 125;
    }

    const int session_directory_descriptor{openat(
        runtime_directory.get(),
        session_directory_name.data(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (session_directory_descriptor == -1) {
        const int open_error{errno};
        error << "NetLagLab: failed to open the netlaglab runtime directory: "
              << std::strerror(open_error) << '\n';
        return 125;
    }
    const FileDescriptor session_directory{session_directory_descriptor};

    if (!validate_session_directory(session_directory.get(), error)) {
        return 125;
    }

    const int lock_descriptor{openat(
        session_directory.get(),
        lock_file_name.data(),
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

    ControlSocketPath control_socket_path_owner{session_directory.get()};
    if (!control_socket_path_owner.remove_stale(error)) {
        return 125;
    }

    const std::string control_socket_path{make_control_socket_path(runtime_path)};
    const int listening_descriptor{create_listening_socket(
        control_socket_path,
        session_directory.get(),
        control_socket_path_owner,
        error)};
    if (listening_descriptor == -1) {
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
