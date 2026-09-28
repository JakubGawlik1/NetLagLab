#pragma once

#include "netlaglab/network_profile.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <variant>

namespace netlaglab {

struct HelpControllerCommand {
};

struct StatusControllerCommand {
};

struct StopControllerCommand {
};

struct DetachControllerCommand {
};

struct IgnoredControllerCommand {
};

struct SetDelay {
    TrafficDirection direction;
    std::chrono::milliseconds value;
};

struct SetJitter {
    TrafficDirection direction;
    std::chrono::milliseconds value;
};

struct SetPacketLoss {
    TrafficDirection direction;
    double percent;
};

struct SetBandwidth {
    TrafficDirection direction;
    std::uint64_t kbps;
};

struct ResetSetting {
    TrafficDirection direction;
    NetworkSetting setting;
};

using ProfileChange = std::variant<
    SetDelay,
    SetJitter,
    SetPacketLoss,
    SetBandwidth,
    ResetSetting>;

enum class ControllerParseError {
    command_too_long,
    invalid_character,
    unknown_command,
    wrong_argument_count,
    invalid_direction,
    invalid_setting,
    invalid_number,
    invalid_unit,
    value_out_of_range,
    value_overflow,
    value_underflow,
};

using ControllerCommand = std::variant<
    HelpControllerCommand,
    StatusControllerCommand,
    ProfileChange,
    StopControllerCommand,
    DetachControllerCommand>;

using ControllerParseResult = std::variant<
    ControllerCommand,
    IgnoredControllerCommand,
    ControllerParseError>;

[[nodiscard]] ControllerParseResult parse_controller_command(
    std::string_view line);

inline constexpr std::size_t maximum_controller_command_size{1024};

enum class ControllerReadResult {
    connected,
    disconnected,
    stop_requested,
};

class ControllerConversation {
public:
    ControllerConversation(
        pid_t workload_pid,
        char* const child_arguments[]) noexcept;

    [[nodiscard]] ControllerReadResult receive(int socket_descriptor);

private:
    pid_t workload_pid_;
    char* const* child_arguments_;
    std::string read_buffer_;
};

[[nodiscard]] bool send_controller_attached(int socket_descriptor);
void reject_additional_controller(int socket_descriptor);

} // namespace netlaglab
