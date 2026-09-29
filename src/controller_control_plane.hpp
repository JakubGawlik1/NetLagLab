#pragma once

#include "netlaglab/network_profile.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <variant>
#include <vector>

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

struct ControllerAttachedEvent {
};

struct ControllerBytesReceivedEvent {
    std::string bytes;
};

struct ControllerDisconnectedEvent {
};

struct ControllerWriteFailedEvent {
};

enum class ControlPlaneProfileChangeResult {
    applied,
    restored_after_failure,
};

struct HelperProfileChangeResultEvent {
    ControlPlaneProfileChangeResult result;
    ProfileChange change;
};

struct HelperProfileStateUnknownEvent {
};

struct SessionStoppingEvent {
};

struct WorkloadTerminalEvent {
};

using ControlPlaneEvent = std::variant<
    ControllerAttachedEvent,
    ControllerBytesReceivedEvent,
    ControllerDisconnectedEvent,
    ControllerWriteFailedEvent,
    HelperProfileChangeResultEvent,
    HelperProfileStateUnknownEvent,
    SessionStoppingEvent,
    WorkloadTerminalEvent>;

struct SendControllerTextAction {
    std::string text;
    bool operator==(const SendControllerTextAction&) const = default;
};

struct DispatchProfileChangeAction {
    ProfileChange change;
    bool operator==(const DispatchProfileChangeAction&) const = default;
};

struct DisconnectControllerAction {
    bool operator==(const DisconnectControllerAction&) const = default;
};

struct RequestLifecycleStopAction {
    bool operator==(const RequestLifecycleStopAction&) const = default;
};

struct FailSessionForUnknownProfileStateAction {
    bool operator==(const FailSessionForUnknownProfileStateAction&) const = default;
};

using ControlPlaneAction = std::variant<
    RequestLifecycleStopAction,
    SendControllerTextAction,
    DisconnectControllerAction,
    FailSessionForUnknownProfileStateAction,
    DispatchProfileChangeAction>;

class ControllerControlPlane {
public:
    ControllerControlPlane(
        pid_t workload_pid,
        char* const child_arguments[]);

    [[nodiscard]] std::vector<ControlPlaneAction> handle(
        const ControlPlaneEvent& event);

private:
    struct QueuedCommand {
        ControllerCommand command;
        std::uint64_t connection_generation;
    };

    struct InFlightProfileChange {
        ProfileChange change;
        std::optional<std::uint64_t> reply_generation;
        bool dispatch_pending_execution;
    };

    void disconnect_controller() noexcept;
    void enter_stopping(std::vector<ControlPlaneAction>& actions);
    void process_command(
        ControllerCommand command,
        std::vector<ControlPlaneAction>& actions);
    void drain_queue(std::vector<ControlPlaneAction>& actions);

    pid_t workload_pid_;
    std::vector<std::string> child_arguments_;
    std::string read_buffer_;
    std::deque<QueuedCommand> command_queue_;
    std::optional<InFlightProfileChange> in_flight_profile_change_;
    NetworkProfile confirmed_profile_;
    std::uint64_t connection_generation_{};
    bool controller_attached_{};
    bool shaping_applied_{};
    bool stopping_{};
    bool workload_terminal_{};
    bool profile_state_failed_{};
};

void reject_additional_controller(int socket_descriptor);

} // namespace netlaglab
