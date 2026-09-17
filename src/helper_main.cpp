#include "file_descriptor.hpp"
#include "helper_protocol.hpp"
#include "session_paths.hpp"
#include "session_socket.hpp"
#include "session_validation.hpp"
#include "socket_io.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

#include <cstdlib>
#include <iostream>

namespace {

constexpr std::string_view usage{
    "Usage: netlaglab-helper --runtime-dir <absolute-path>\n"};
constexpr int usage_error_exit_code{2};

[[nodiscard]] bool send_helper_error(
    const int supervisor_descriptor,
    const std::string_view message)
{
    std::string response{netlaglab::helper_error_prefix};
    response.append(message);
    response.push_back('\n');
    return netlaglab::send_socket_text(supervisor_descriptor, response);
}

[[nodiscard]] int handle_supervisor_commands(
    const int supervisor_descriptor,
    std::ostream& error)
{
    std::string command_buffer;

    while (true) {
        const netlaglab::SocketReadResult read_result{
            netlaglab::read_socket_data(supervisor_descriptor, command_buffer)};

        if (read_result.status == netlaglab::SocketReadStatus::peer_closed) {
            error << "NetLagLab helper: supervisor disconnected before shutdown\n";
            return 125;
        }

        if (read_result.status == netlaglab::SocketReadStatus::error) {
            error << "NetLagLab helper: failed to read supervisor command: "
                  << std::strerror(read_result.error_code) << '\n';
            return 125;
        }

        while (true) {
            const std::optional<std::string> line{
                netlaglab::take_next_line(command_buffer)};
            if (!line.has_value()) {
                break;
            }

            if (line->size() > netlaglab::maximum_helper_message_size) {
                (void)send_helper_error(
                    supervisor_descriptor, "Command exceeds 1024 bytes.");
                error << "NetLagLab helper: supervisor command exceeds 1024 bytes\n";
                return 125;
            }

            const std::optional<netlaglab::HelperCommand> command{
                netlaglab::parse_helper_command(*line)};

            if (!command.has_value()) {
                (void)send_helper_error(supervisor_descriptor, "Unknown command.");
                error << "NetLagLab helper: received an unknown supervisor command\n";
                return 125;
            }

            if (*command == netlaglab::HelperCommand::shutdown) {
                if (!netlaglab::send_socket_text(
                        supervisor_descriptor,
                        netlaglab::helper_event_message(
                            netlaglab::HelperEvent::stopped))) {
                    error << "NetLagLab helper: failed to send STOPPED to supervisor\n";
                    return 125;
                }

                return 0;
            }
        }

        if (command_buffer.size() > netlaglab::maximum_helper_message_size) {
            (void)send_helper_error(
                supervisor_descriptor, "Command exceeds 1024 bytes.");
            error << "NetLagLab helper: supervisor command exceeds 1024 bytes\n";
            return 125;
        }
    }
}

[[nodiscard]] bool cleanup_helper_listener(
    netlaglab::FileDescriptor& listening_socket,
    netlaglab::SocketPathOwner& socket_path_owner,
    std::ostream& error)
{
    listening_socket.reset();

    const int cleanup_error{socket_path_owner.remove_owned()};
    if (cleanup_error == 0) {
        return true;
    }

    error << "NetLagLab helper: failed to remove helper.sock: "
          << std::strerror(cleanup_error) << '\n';
    return false;
}

[[nodiscard]] std::optional<netlaglab::FileDescriptor> validate_session(
    const netlaglab::SessionPaths& paths,
    const uid_t expected_owner,
    std::ostream& error)
{
    std::optional<netlaglab::FileDescriptor> runtime_directory{
        netlaglab::open_and_validate_runtime_directory(
            paths.xdg_runtime_directory(), expected_owner, error)};
    if (!runtime_directory.has_value()) {
        return std::nullopt;
    }

    std::optional<netlaglab::FileDescriptor> session_directory{
        netlaglab::open_and_validate_session_directory(
            runtime_directory->get(), expected_owner, error)};
    if (!session_directory.has_value()) {
        return std::nullopt;
    }

    const int lock_descriptor{openat(
        session_directory->get(),
        netlaglab::SessionPaths::lock_file_name.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (lock_descriptor == -1) {
        const int open_error{errno};
        error << "NetLagLab helper: failed to open session.lock: "
              << std::strerror(open_error) << '\n';
        return std::nullopt;
    }
    netlaglab::FileDescriptor session_lock{lock_descriptor};

    struct stat lock_status {};
    if (fstat(session_lock.get(), &lock_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab helper: failed to inspect session.lock: "
              << std::strerror(status_error) << '\n';
        return std::nullopt;
    }

    if (!S_ISREG(lock_status.st_mode)) {
        error << "NetLagLab helper: session.lock is not a regular file\n";
        return std::nullopt;
    }

    if (lock_status.st_uid != expected_owner) {
        error << "NetLagLab helper: session.lock has an unexpected owner\n";
        return std::nullopt;
    }

    if ((lock_status.st_mode & 0777) != 0600) {
        error << "NetLagLab helper: session.lock must have permissions 0600\n";
        return std::nullopt;
    }

    if (flock(session_lock.get(), LOCK_EX | LOCK_NB) == 0) {
        error << "NetLagLab helper: session.lock is not held by an active supervisor\n";
        return std::nullopt;
    }

    const int lock_error{errno};
    if (lock_error != EWOULDBLOCK && lock_error != EAGAIN) {
        error << "NetLagLab helper: failed to inspect the session lock: "
              << std::strerror(lock_error) << '\n';
        return std::nullopt;
    }

    return session_directory;
}

[[nodiscard]] std::optional<uid_t> read_sudo_uid(std::ostream& error)
{
    const char* const sudo_uid_text{std::getenv("SUDO_UID")};
    if (sudo_uid_text == nullptr || sudo_uid_text[0] == '\0') {
        error << "NetLagLab helper: SUDO_UID is not set\n";
        return std::nullopt;
    }
    const std::string_view text{sudo_uid_text};

    uid_t parsed_uid{};
    const auto result{
        std::from_chars(text.data(), text.data() + text.size(), parsed_uid)};

    const bool success{
        result.ec == std::errc{} && result.ptr == text.data() + text.size()};
    if (!success) {
        error << "NetLagLab helper: failed to parse SUDO_UID\n";
        return std::nullopt;
    }

    return parsed_uid;
}

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

    const std::optional<uid_t> sudo_uid{read_sudo_uid(std::cerr)};
    if (!sudo_uid.has_value()) {
        return 125;
    }

    const netlaglab::SessionPaths paths{std::string{runtime_directory}};
    const std::optional<netlaglab::FileDescriptor> session_directory{
        validate_session(paths, *sudo_uid, std::cerr)};
    if (!session_directory.has_value()) {
        return 125;
    }

    netlaglab::SocketPathOwner helper_socket_path_owner{
        session_directory->get(),
        netlaglab::SessionPaths::helper_socket_name,
        paths.helper_socket(),
        *sudo_uid};
    if (!helper_socket_path_owner.remove_stale(std::cerr)) {
        return 125;
    }

    const int listening_descriptor{
        helper_socket_path_owner.create_listening_socket(std::cerr)};
    if (listening_descriptor == -1) {
        const int cleanup_error{helper_socket_path_owner.remove_owned()};
        if (cleanup_error != 0) {
            std::cerr << "NetLagLab helper: failed to remove helper.sock after listener "
                         "setup failure: "
                      << std::strerror(cleanup_error) << '\n';
        }
        return 125;
    }
    netlaglab::FileDescriptor listening_socket{listening_descriptor};

    int supervisor_descriptor{};
    do {
        supervisor_descriptor =
            accept4(listening_socket.get(), nullptr, nullptr, SOCK_CLOEXEC);
    } while (supervisor_descriptor == -1 && errno == EINTR);

    if (supervisor_descriptor == -1) {
        const int accept_error{errno};
        std::cerr << "NetLagLab helper: failed to accept supervisor: "
                  << std::strerror(accept_error) << '\n';
        (void)cleanup_helper_listener(
            listening_socket, helper_socket_path_owner, std::cerr);
        return 125;
    }
    const netlaglab::FileDescriptor supervisor_socket{supervisor_descriptor};

    struct ucred peer_credentials {};
    socklen_t credentials_size{sizeof(peer_credentials)};
    if (getsockopt(
            supervisor_socket.get(),
            SOL_SOCKET,
            SO_PEERCRED,
            &peer_credentials,
            &credentials_size)
        == -1) {
        const int credentials_error{errno};
        std::cerr << "NetLagLab helper: failed to inspect supervisor credentials: "
                  << std::strerror(credentials_error) << '\n';
        (void)cleanup_helper_listener(
            listening_socket, helper_socket_path_owner, std::cerr);
        return 125;
    }

    if (credentials_size != static_cast<socklen_t>(sizeof(peer_credentials))
        || peer_credentials.uid != *sudo_uid) {
        std::cerr << "NetLagLab helper: rejected connection from an unexpected user\n";
        (void)cleanup_helper_listener(
            listening_socket, helper_socket_path_owner, std::cerr);
        return 125;
    }

    if (!cleanup_helper_listener(
            listening_socket, helper_socket_path_owner, std::cerr)) {
        return 125;
    }

    if (!netlaglab::send_socket_text(
            supervisor_socket.get(),
            netlaglab::helper_event_message(netlaglab::HelperEvent::ready))) {
        std::cerr << "NetLagLab helper: failed to send READY to supervisor\n";
        return 125;
    }

    return handle_supervisor_commands(supervisor_socket.get(), std::cerr);
}

} // namespace

int main(const int argc, char* argv[])
{
    return run_helper(argc, argv);
}
