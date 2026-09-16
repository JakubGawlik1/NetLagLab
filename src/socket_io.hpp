#pragma once

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

[[nodiscard]] bool send_socket_text(int socket_descriptor, std::string_view message);

[[nodiscard]] SocketReadResult read_socket_data(
    int socket_descriptor,
    std::string& buffer);

[[nodiscard]] std::optional<std::string> take_next_line(std::string& buffer);

} // namespace netlaglab
