#include "socket_io.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <sys/socket.h>
#include <unistd.h>

namespace netlaglab {

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
