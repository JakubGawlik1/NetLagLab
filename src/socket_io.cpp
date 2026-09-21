#include "socket_io.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace netlaglab {

UnixSocketConnectResult connect_to_unix_socket(const std::string_view socket_path)
{
    struct sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path)) {
        return {
            std::nullopt,
            UnixSocketConnectFailure::path_too_long,
            ENAMETOOLONG,
        };
    }

    const int raw_socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (raw_socket == -1) {
        return {
            std::nullopt,
            UnixSocketConnectFailure::socket_creation,
            errno,
        };
    }
    FileDescriptor client_socket{raw_socket};

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
    const auto address_size{static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1)};

    if (connect(
            client_socket.get(),
            reinterpret_cast<const struct sockaddr*>(&address),
            address_size)
        == -1) {
        const int connect_error{errno};
        return {
            std::nullopt,
            UnixSocketConnectFailure::connection,
            connect_error,
        };
    }

    return {
        std::move(client_socket),
        UnixSocketConnectFailure::none,
        0,
    };
}

bool send_socket_text(const int socket_descriptor, const std::string_view message)
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

SocketReadResult read_socket_data(
    const int socket_descriptor,
    std::string& buffer)
{
    std::array<char, 4096> read_buffer{};
    ssize_t read_size{};

    do {
        read_size = read(socket_descriptor, read_buffer.data(), read_buffer.size());
    } while (read_size == -1 && errno == EINTR);

    if (read_size > 0) {
        buffer.append(read_buffer.data(), static_cast<std::size_t>(read_size));
        return {SocketReadStatus::data_received};
    }

    if (read_size == 0) {
        return {SocketReadStatus::peer_closed};
    }

    return {SocketReadStatus::error, errno};
}

std::optional<std::string> take_next_line(std::string& buffer)
{
    const std::size_t newline_position{buffer.find('\n')};
    if (newline_position == std::string::npos) {
        return std::nullopt;
    }

    std::string line{buffer.substr(0, newline_position)};
    buffer.erase(0, newline_position + 1);
    return line;
}

} // namespace netlaglab
