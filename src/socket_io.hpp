#pragma once

#include "file_descriptor.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace netlaglab {

enum class SocketReadStatus {
    data_received,
    peer_closed,
    error,
};

struct SocketReadResult {
    SocketReadStatus status;
    int error_code{};
};

enum class UnixSocketConnectFailure {
    none,
    path_too_long,
    socket_creation,
    connection,
};

struct UnixSocketConnectResult {
    std::optional<FileDescriptor> socket;
    UnixSocketConnectFailure failure{UnixSocketConnectFailure::none};
    int error_code{};
};

enum class StandardDescriptorDisposition {
    inherit_terminal,
    explicitly_closed,
    transferred,
};

struct ReceivedStandardDescriptors {
    std::array<StandardDescriptorDisposition, 3> dispositions{
        StandardDescriptorDisposition::inherit_terminal,
        StandardDescriptorDisposition::inherit_terminal,
        StandardDescriptorDisposition::inherit_terminal,
    };
    std::array<std::optional<FileDescriptor>, 3> descriptors;
};

[[nodiscard]] UnixSocketConnectResult connect_to_unix_socket(
    std::string_view socket_path);

[[nodiscard]] bool send_socket_text(int socket_descriptor, std::string_view message);

[[nodiscard]] bool send_standard_descriptors(
    int socket_descriptor,
    const std::array<int, 3>& descriptors);

[[nodiscard]] std::optional<ReceivedStandardDescriptors>
receive_standard_descriptors(int socket_descriptor);

[[nodiscard]] SocketReadResult read_socket_data(
    int socket_descriptor,
    std::string& buffer);

[[nodiscard]] std::optional<std::string> take_next_line(std::string& buffer);

} // namespace netlaglab
