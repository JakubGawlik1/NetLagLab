#include "controller_control_plane.hpp"

#include "socket_io.hpp"

#include "netlaglab/network_profile.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace netlaglab {
namespace {

constexpr std::size_t maximum_status_size{8 * 1024};

[[nodiscard]] bool is_token_separator(const char character) noexcept
{
    return character == ' ' || character == '\t';
}

[[nodiscard]] std::vector<std::string_view> tokenize(
    const std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t position{};
    while (position < line.size()) {
        while (position < line.size() && is_token_separator(line[position])) {
            ++position;
        }
        const std::size_t start{position};
        while (position < line.size() && !is_token_separator(line[position])) {
            ++position;
        }
        if (start != position) {
            tokens.push_back(line.substr(start, position - start));
        }
    }
    return tokens;
}

[[nodiscard]] std::optional<TrafficDirection> parse_direction(
    const std::string_view token) noexcept
{
    if (token == "outbound") {
        return TrafficDirection::outbound;
    }
    if (token == "inbound") {
        return TrafficDirection::inbound;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<NetworkSetting> parse_setting(
    const std::string_view token) noexcept
{
    if (token == "delay") {
        return NetworkSetting::delay;
    }
    if (token == "jitter") {
        return NetworkSetting::jitter;
    }
    if (token == "loss") {
        return NetworkSetting::packet_loss;
    }
    if (token == "bandwidth") {
        return NetworkSetting::bandwidth;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_decimal_integer(const std::string_view value) noexcept
{
    if (value.empty()) {
        return false;
    }
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_loss_decimal(const std::string_view value) noexcept
{
    if (value.empty()) {
        return false;
    }
    bool saw_decimal_point{false};
    std::size_t digits_after_point{};
    for (const char character : value) {
        if (character >= '0' && character <= '9') {
            if (saw_decimal_point) {
                ++digits_after_point;
            }
            continue;
        }
        if (character == '.' && !saw_decimal_point) {
            saw_decimal_point = true;
            continue;
        }
        return false;
    }
    return value.front() != '.'
        && (!saw_decimal_point || digits_after_point != 0);
}

struct NumberAndUnit {
    std::string_view number;
    std::string_view unit;
};

[[nodiscard]] NumberAndUnit split_attached_unit(
    const std::string_view token) noexcept
{
    for (std::size_t index{}; index < token.size(); ++index) {
        const char character{token[index]};
        if ((character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z') || character == '%') {
            return {token.substr(0, index), token.substr(index)};
        }
    }
    return {token, {}};
}

[[nodiscard]] bool decimal_is_zero(const std::string_view value) noexcept
{
    for (const char character : value) {
        if (character >= '1' && character <= '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::variant<std::uint64_t, ControllerParseError>
parse_scaled_unsigned(
    const std::string_view number,
    const std::uint64_t scale,
    const std::uint64_t maximum)
{
    std::uint64_t parsed{};
    const auto result{
        std::from_chars(number.data(), number.data() + number.size(), parsed)};
    if (result.ec == std::errc::result_out_of_range) {
        return ControllerParseError::value_overflow;
    }
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size()) {
        return ControllerParseError::invalid_number;
    }
    if (parsed > maximum / scale) {
        return ControllerParseError::value_overflow;
    }
    return parsed * scale;
}

[[nodiscard]] bool loss_is_out_of_range(const std::string_view number) noexcept
{
    const std::size_t decimal_point{number.find('.')};
    std::string_view integer_part{number.substr(0, decimal_point)};
    const std::size_t first_nonzero{integer_part.find_first_not_of('0')};
    integer_part = first_nonzero == std::string_view::npos
        ? std::string_view{"0"}
        : integer_part.substr(first_nonzero);
    if (integer_part.size() > 3
        || (integer_part.size() == 3 && integer_part > "100")) {
        return true;
    }
    if (integer_part != "100" || decimal_point == std::string_view::npos) {
        return false;
    }
    return !decimal_is_zero(number.substr(decimal_point + 1));
}

[[nodiscard]] std::variant<double, ControllerParseError> parse_loss(
    const std::string_view number)
{
    if (loss_is_out_of_range(number)) {
        return ControllerParseError::value_out_of_range;
    }
    double value{};
    const auto result{std::from_chars(
        number.data(),
        number.data() + number.size(),
        value,
        std::chars_format::fixed)};
    if (result.ec == std::errc::result_out_of_range
        || (value == 0.0 && !decimal_is_zero(number))) {
        return ControllerParseError::value_underflow;
    }
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size()
        || !std::isfinite(value)) {
        return ControllerParseError::invalid_number;
    }
    return value;
}

[[nodiscard]] ControllerParseResult parse_set_command(
    const std::vector<std::string_view>& tokens)
{
    if (tokens.size() != 4 && tokens.size() != 5) {
        return ControllerParseError::wrong_argument_count;
    }
    const std::optional<TrafficDirection> direction{parse_direction(tokens[1])};
    if (!direction.has_value()) {
        return ControllerParseError::invalid_direction;
    }
    const std::optional<NetworkSetting> setting{parse_setting(tokens[2])};
    if (!setting.has_value()) {
        return ControllerParseError::invalid_setting;
    }

    NumberAndUnit value{split_attached_unit(tokens[3])};
    if (tokens.size() == 5) {
        value = {tokens[3], tokens[4]};
    }
    const bool is_loss{*setting == NetworkSetting::packet_loss};
    if ((is_loss && !is_loss_decimal(value.number))
        || (!is_loss && !is_decimal_integer(value.number))) {
        return ControllerParseError::invalid_number;
    }

    if (*setting == NetworkSetting::delay || *setting == NetworkSetting::jitter) {
        std::uint64_t scale{1};
        if (value.unit == "s") {
            scale = 1000;
        } else if (!value.unit.empty() && value.unit != "ms") {
            return ControllerParseError::invalid_unit;
        }
        using MillisecondsRep = std::chrono::milliseconds::rep;
        const auto normalized{parse_scaled_unsigned(
            value.number,
            scale,
            static_cast<std::uint64_t>(
                std::numeric_limits<MillisecondsRep>::max()))};
        if (const auto* error{std::get_if<ControllerParseError>(&normalized)}) {
            return *error;
        }
        const std::chrono::milliseconds milliseconds{
            static_cast<MillisecondsRep>(std::get<std::uint64_t>(normalized))};
        if (*setting == NetworkSetting::delay) {
            return ControllerCommand{
                ProfileChange{SetDelay{*direction, milliseconds}}};
        }
        return ControllerCommand{
            ProfileChange{SetJitter{*direction, milliseconds}}};
    }

    if (*setting == NetworkSetting::bandwidth) {
        std::uint64_t scale{1};
        if (value.unit == "mbps") {
            scale = 1000;
        } else if (!value.unit.empty() && value.unit != "kbps") {
            return ControllerParseError::invalid_unit;
        }
        if (decimal_is_zero(value.number)) {
            return ControllerParseError::value_out_of_range;
        }
        const auto normalized{parse_scaled_unsigned(
            value.number, scale, std::numeric_limits<std::uint64_t>::max())};
        if (const auto* error{std::get_if<ControllerParseError>(&normalized)}) {
            return *error;
        }
        return ControllerCommand{ProfileChange{SetBandwidth{
            *direction, std::get<std::uint64_t>(normalized)}}};
    }

    if (!value.unit.empty() && value.unit != "%") {
        return ControllerParseError::invalid_unit;
    }
    const auto loss{parse_loss(value.number)};
    if (const auto* error{std::get_if<ControllerParseError>(&loss)}) {
        return *error;
    }
    return ControllerCommand{ProfileChange{
        SetPacketLoss{*direction, std::get<double>(loss)}}};
}

[[nodiscard]] ControllerParseResult parse_reset_command(
    const std::vector<std::string_view>& tokens)
{
    if (tokens.size() != 3) {
        return ControllerParseError::wrong_argument_count;
    }
    const std::optional<TrafficDirection> direction{parse_direction(tokens[1])};
    if (!direction.has_value()) {
        return ControllerParseError::invalid_direction;
    }
    const std::optional<NetworkSetting> setting{parse_setting(tokens[2])};
    if (!setting.has_value()) {
        return ControllerParseError::invalid_setting;
    }
    return ControllerCommand{
        ProfileChange{ResetSetting{*direction, *setting}}};
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

[[nodiscard]] std::string_view controller_parse_error_response(
    const ControllerParseError error) noexcept
{
    switch (error) {
    case ControllerParseError::command_too_long:
        return "ERROR Command exceeds 1024 bytes.\n";
    case ControllerParseError::invalid_character:
        return "ERROR Command contains an invalid character.\n";
    case ControllerParseError::unknown_command:
        return "ERROR Unknown command.\n";
    case ControllerParseError::wrong_argument_count:
        return "ERROR Wrong number of arguments.\n";
    case ControllerParseError::invalid_direction:
        return "ERROR Direction must be outbound or inbound.\n";
    case ControllerParseError::invalid_setting:
        return "ERROR Setting must be delay, jitter, loss, or bandwidth.\n";
    case ControllerParseError::invalid_number:
        return "ERROR Invalid numeric value.\n";
    case ControllerParseError::invalid_unit:
        return "ERROR Invalid unit for setting.\n";
    case ControllerParseError::value_out_of_range:
        return "ERROR Value is outside the allowed range.\n";
    case ControllerParseError::value_overflow:
        return "ERROR Value is too large.\n";
    case ControllerParseError::value_underflow:
        return "ERROR Value is too small to represent.\n";
    }
    return "ERROR Unknown command.\n";
}

[[nodiscard]] bool send_controller_parse_error(
    const int socket_descriptor,
    const ControllerParseError error)
{
    return send_socket_text(
        socket_descriptor, controller_parse_error_response(error));
}

[[nodiscard]] CommandResult handle_command(
    const int socket_descriptor,
    const std::string_view command,
    const pid_t workload_pid,
    char* const child_arguments[])
{
    const ControllerParseResult parsed{parse_controller_command(command)};
    if (std::holds_alternative<IgnoredControllerCommand>(parsed)) {
        return CommandResult::keep_connected;
    }
    if (const auto* error{std::get_if<ControllerParseError>(&parsed)}) {
        const bool sent{send_controller_parse_error(socket_descriptor, *error)};
        return sent && *error != ControllerParseError::command_too_long
            ? CommandResult::keep_connected
            : CommandResult::disconnect;
    }

    const ControllerCommand& typed_command{std::get<ControllerCommand>(parsed)};
    if (std::holds_alternative<HelpControllerCommand>(typed_command)) {
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
    if (std::holds_alternative<StatusControllerCommand>(typed_command)) {
        return send_socket_text(
                   socket_descriptor,
                   build_status(workload_pid, child_arguments))
            ? CommandResult::keep_connected : CommandResult::disconnect;
    }
    if (std::holds_alternative<ProfileChange>(typed_command)) {
        return send_socket_text(
                   socket_descriptor,
                   "ERROR Profile changes are not available yet.\n")
            ? CommandResult::keep_connected
            : CommandResult::disconnect;
    }
    if (std::holds_alternative<StopControllerCommand>(typed_command)) {
        return send_socket_text(socket_descriptor, "STOPPING\n")
            ? CommandResult::stop_requested : CommandResult::disconnect;
    }
    if (std::holds_alternative<DetachControllerCommand>(typed_command)) {
        (void)send_socket_text(socket_descriptor, "DETACHED\n");
        return CommandResult::disconnect;
    }
    return CommandResult::disconnect;
}

} // namespace

ControllerParseResult parse_controller_command(std::string_view line)
{
    if (line.size() > maximum_controller_command_size) {
        return ControllerParseError::command_too_long;
    }
    for (const unsigned char byte : line) {
        if (byte != '\t' && (byte < 0x20 || byte > 0x7E)) {
            return ControllerParseError::invalid_character;
        }
    }

    const std::vector<std::string_view> tokens{tokenize(line)};
    if (tokens.empty()) {
        return IgnoredControllerCommand{};
    }
    const std::string_view command{tokens.front()};
    if (command != "help" && command != "status" && command != "set"
        && command != "reset" && command != "stop" && command != "detach") {
        return ControllerParseError::unknown_command;
    }
    if (command == "set") {
        return parse_set_command(tokens);
    }
    if (command == "reset") {
        return parse_reset_command(tokens);
    }
    if (tokens.size() != 1) {
        return ControllerParseError::wrong_argument_count;
    }
    if (command == "help") {
        return ControllerCommand{HelpControllerCommand{}};
    }
    if (command == "status") {
        return ControllerCommand{StatusControllerCommand{}};
    }
    if (command == "stop") {
        return ControllerCommand{StopControllerCommand{}};
    }
    return command == "detach"
        ? ControllerParseResult{ControllerCommand{DetachControllerCommand{}}}
        : ControllerParseResult{ControllerParseError::unknown_command};
}

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
        const CommandResult result{handle_command(
            socket_descriptor, *command, workload_pid_, child_arguments_)};
        if (result == CommandResult::disconnect) {
            return ControllerReadResult::disconnected;
        }
        if (result == CommandResult::stop_requested) {
            return ControllerReadResult::stop_requested;
        }
    }

    if (read_buffer_.size() > maximum_controller_command_size) {
        (void)send_controller_parse_error(
            socket_descriptor, ControllerParseError::command_too_long);
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
