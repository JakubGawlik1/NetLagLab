#include "controller_control_plane.hpp"

#include "socket_io.hpp"

#include "netlaglab/network_profile.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string_view>

namespace netlaglab {
namespace {

constexpr std::size_t maximum_command_size{1024};
constexpr std::size_t maximum_status_size{8 * 1024};

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
                '\\', 'x', hex_digits[byte >> 4], hex_digits[byte & 0x0F]};
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
    const pid_t workload_pid,
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
    header_stream << "STATUS_BEGIN\nstate: running\npid: " << workload_pid
                  << "\nprogram: ";
    std::string response{header_stream.str()};

    bool truncated{false};
    if (!append_escaped_limited(response, child_arguments[0], content_limit - 1)) {
        truncated = true;
    }
    response.push_back('\n');
    if (!truncated) {
        for (std::size_t source_index{1}; child_arguments[source_index] != nullptr;
             ++source_index) {
            const std::string prefix{
                "argument[" + std::to_string(source_index - 1) + "]: "};
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
    stop_requested,
};

[[nodiscard]] CommandResult handle_command(
    const int socket_descriptor,
    const std::string_view command,
    const pid_t workload_pid,
    char* const child_arguments[])
{
    if (command == "help") {
        constexpr std::string_view response{
            "HELP_BEGIN\n"
            "help - Show controller commands\n"
            "status - Show the active session\n"
            "stop - Stop the active Workload\n"
            "detach - Disconnect this controller\n"
            "HELP_END\n"};
        return send_socket_text(socket_descriptor, response)
            ? CommandResult::keep_connected : CommandResult::disconnect;
    }
    if (command == "status") {
        return send_socket_text(
                   socket_descriptor,
                   build_status(workload_pid, child_arguments))
            ? CommandResult::keep_connected : CommandResult::disconnect;
    }
    if (command == "stop") {
        return send_socket_text(socket_descriptor, "STOPPING\n")
            ? CommandResult::stop_requested : CommandResult::disconnect;
    }
    if (command == "detach") {
        (void)send_socket_text(socket_descriptor, "DETACHED\n");
        return CommandResult::disconnect;
    }

    const std::string response{
        "ERROR Unknown command: " + escape_command(command) + '\n'};
    return send_socket_text(socket_descriptor, response)
        ? CommandResult::keep_connected : CommandResult::disconnect;
}

} // namespace

ControllerConversation::ControllerConversation(
    const pid_t workload_pid,
    char* const child_arguments[]) noexcept
    : workload_pid_{workload_pid}, child_arguments_{child_arguments}
{
}

ControllerReadResult ControllerConversation::receive(
    const int socket_descriptor)
{
    const SocketReadResult read_result{
        read_socket_data(socket_descriptor, read_buffer_)};
    if (read_result.status != SocketReadStatus::data_received) {
        return ControllerReadResult::disconnected;
    }

    while (true) {
        const std::optional<std::string> command{take_next_line(read_buffer_)};
        if (!command.has_value()) {
            break;
        }
        if (command->size() > maximum_command_size) {
            (void)send_socket_text(
                socket_descriptor, "ERROR Command exceeds 1024 bytes.\n");
            return ControllerReadResult::disconnected;
        }
        if (command->empty()) {
            continue;
        }
        const CommandResult result{handle_command(
            socket_descriptor, *command, workload_pid_, child_arguments_)};
        if (result == CommandResult::disconnect) {
            return ControllerReadResult::disconnected;
        }
        if (result == CommandResult::stop_requested) {
            return ControllerReadResult::stop_requested;
        }
    }

    if (read_buffer_.size() > maximum_command_size) {
        (void)send_socket_text(
            socket_descriptor, "ERROR Command exceeds 1024 bytes.\n");
        return ControllerReadResult::disconnected;
    }
    return ControllerReadResult::connected;
}

bool send_controller_attached(const int socket_descriptor)
{
    return send_socket_text(socket_descriptor, "ATTACHED\n");
}

void reject_additional_controller(const int socket_descriptor)
{
    (void)send_socket_text(
        socket_descriptor, "ERROR Another controller is already attached.\n");
}

void send_controller_session_result(
    const int socket_descriptor,
    const bool session_succeeded)
{
    (void)send_socket_text(
        socket_descriptor,
        session_succeeded ? "SESSION_ENDED\n" : "SESSION_FAILED\n");
}

} // namespace netlaglab
