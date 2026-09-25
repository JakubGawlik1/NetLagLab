#include "socket_io.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
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

bool send_standard_descriptors(
    const int socket_descriptor,
    const std::array<int, 3>& descriptors)
{
    constexpr unsigned char transferred_shift{0};
    constexpr unsigned char closed_shift{3};
    unsigned char marker{};
    std::array<int, 3> transferred{};
    std::size_t transferred_count{};

    for (std::size_t index{}; index < descriptors.size(); ++index) {
        errno = 0;
        if (fcntl(descriptors[index], F_GETFD) == -1 && errno == EBADF) {
            marker |= static_cast<unsigned char>(1U << (closed_shift + index));
        } else if (isatty(descriptors[index]) != 1) {
            marker |= static_cast<unsigned char>(1U << (transferred_shift + index));
            transferred[transferred_count++] = descriptors[index];
        }
    }

    struct iovec payload {&marker, sizeof(marker)};
    std::array<unsigned char, CMSG_SPACE(sizeof(int) * 3)> control{};
    struct msghdr message {};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    if (transferred_count != 0) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(int) * transferred_count);
        struct cmsghdr* const header{CMSG_FIRSTHDR(&message)};
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * transferred_count);
        std::memcpy(
            CMSG_DATA(header), transferred.data(), sizeof(int) * transferred_count);
    }

    ssize_t sent{};
    do {
        sent = sendmsg(socket_descriptor, &message, MSG_NOSIGNAL);
    } while (sent == -1 && errno == EINTR);
    return sent == static_cast<ssize_t>(sizeof(marker));
}

std::optional<ReceivedStandardDescriptors> receive_standard_descriptors(
    const int socket_descriptor)
{
    unsigned char marker{};
    struct iovec payload {&marker, sizeof(marker)};
    std::array<unsigned char, CMSG_SPACE(sizeof(int) * 3)> control{};
    struct msghdr message {};
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    ssize_t received{};
    do {
        received = recvmsg(socket_descriptor, &message, MSG_CMSG_CLOEXEC);
    } while (received == -1 && errno == EINTR);
    if (received != static_cast<ssize_t>(sizeof(marker))
        || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0
        || (marker & 0xC0U) != 0) {
        return std::nullopt;
    }

    std::array<int, 3> raw_descriptors{};
    std::size_t raw_count{};
    for (struct cmsghdr* header{CMSG_FIRSTHDR(&message)};
         header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS
            || header->cmsg_len < CMSG_LEN(0)) {
            for (std::size_t index{}; index < raw_count; ++index) {
                close(raw_descriptors[index]);
            }
            return std::nullopt;
        }
        const std::size_t bytes{header->cmsg_len - CMSG_LEN(0)};
        if (bytes % sizeof(int) != 0 || raw_count + bytes / sizeof(int) > 3) {
            for (std::size_t index{}; index < raw_count; ++index) {
                close(raw_descriptors[index]);
            }
            return std::nullopt;
        }
        const std::size_t count{bytes / sizeof(int)};
        std::memcpy(
            raw_descriptors.data() + raw_count, CMSG_DATA(header), bytes);
        raw_count += count;
    }

    ReceivedStandardDescriptors result;
    std::size_t next_descriptor{};
    for (std::size_t index{}; index < result.dispositions.size(); ++index) {
        const bool transferred{(marker & (1U << index)) != 0};
        const bool closed{(marker & (1U << (index + 3))) != 0};
        if (transferred && closed) {
            for (std::size_t raw_index{next_descriptor}; raw_index < raw_count;
                 ++raw_index) {
                close(raw_descriptors[raw_index]);
            }
            return std::nullopt;
        }
        if (transferred) {
            if (next_descriptor == raw_count) {
                return std::nullopt;
            }
            result.dispositions[index] = StandardDescriptorDisposition::transferred;
            result.descriptors[index].emplace(raw_descriptors[next_descriptor++]);
        } else if (closed) {
            result.dispositions[index] =
                StandardDescriptorDisposition::explicitly_closed;
        }
    }
    if (next_descriptor != raw_count) {
        for (std::size_t index{next_descriptor}; index < raw_count; ++index) {
            close(raw_descriptors[index]);
        }
        return std::nullopt;
    }
    return result;
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
