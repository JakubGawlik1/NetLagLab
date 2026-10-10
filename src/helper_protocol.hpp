#pragma once

#include "netlaglab/network_profile.hpp"
#include "workload_context.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace netlaglab {

struct StopTerminateCommand {
    bool operator==(const StopTerminateCommand&) const = default;
};

struct StopKillCommand {
    bool operator==(const StopKillCommand&) const = default;
};

struct CompatibilityShutdownCommand {
    bool operator==(const CompatibilityShutdownCommand&) const = default;
};

using HelperRuntimeCommand = std::variant<
    StopTerminateCommand,
    StopKillCommand,
    CompatibilityShutdownCommand,
    ProfileChange>;

struct ReadyEvent {
    bool operator==(const ReadyEvent&) const = default;
};

struct ActivatedEvent {
    int workload_pid;
    bool operator==(const ActivatedEvent&) const = default;
};

struct ActivationFailedEvent {
    int result;
    bool operator==(const ActivationFailedEvent&) const = default;
};

struct WorkloadExitedEvent {
    int result;
    bool operator==(const WorkloadExitedEvent&) const = default;
};

struct WorkloadSignaledEvent {
    int signal;
    bool operator==(const WorkloadSignaledEvent&) const = default;
};

struct CleanupSucceededEvent {
    bool operator==(const CleanupSucceededEvent&) const = default;
};

struct CleanupFailedEvent {
    bool operator==(const CleanupFailedEvent&) const = default;
};

struct ConversationLostEvent {
    bool operator==(const ConversationLostEvent&) const = default;
};

enum class ProfileChangeResult {
    applied,
    restored_after_failure,
};

struct ProfileChangeResultEvent {
    ProfileChangeResult result;
    ProfileChange change;
    bool operator==(const ProfileChangeResultEvent&) const = default;
};

struct ProfileStateUnknownEvent {
    bool operator==(const ProfileStateUnknownEvent&) const = default;
};

struct ConnectivityFailedEvent {
    bool operator==(const ConnectivityFailedEvent&) const = default;
};

using HelperConversationEvent = std::variant<
    ReadyEvent,
    ActivatedEvent,
    ActivationFailedEvent,
    WorkloadExitedEvent,
    WorkloadSignaledEvent,
    CleanupSucceededEvent,
    CleanupFailedEvent,
    ConversationLostEvent,
    ProfileChangeResultEvent,
    ProfileStateUnknownEvent,
    ConnectivityFailedEvent>;

enum class StartBlockState {
    incomplete,
    complete,
    invalid,
};

struct StartBlockFeedResult {
    StartBlockState state;
    std::optional<WorkloadContext> context;
};

class HelperStartConversation {
public:
    [[nodiscard]] StartBlockFeedResult receive_bytes(std::string_view bytes);
    [[nodiscard]] bool started() const noexcept;

private:
    std::string buffer_;
    std::vector<std::string> lines_;
    std::size_t encoded_size_{};
    bool started_{};
    bool finished_{};
};

struct RuntimeCommandFeedResult {
    bool valid;
    std::vector<HelperRuntimeCommand> commands;
    std::optional<std::string> response;
};

enum class ProfileChangeCompletion {
    applied,
    restored_after_failure,
    state_unknown,
};

class ProfileChangeAdapter {
public:
    virtual ~ProfileChangeAdapter() = default;
    [[nodiscard]] virtual ProfileChangeCompletion apply(
        const ProfileChange& change) = 0;
};

class HelperRuntimeConversation {
public:
    [[nodiscard]] RuntimeCommandFeedResult receive_bytes(std::string_view bytes);
    void connectivity_failed() noexcept;
    [[nodiscard]] std::optional<std::string> complete_profile_change(
        ProfileChangeCompletion completion);
    void workload_finished() noexcept;

private:
    std::string buffer_;
    std::optional<ProfileChange> pending_profile_change_;
    bool valid_{true};
    bool workload_finished_{};
    bool stopping_{};
    bool profile_state_unknown_{};
    bool connectivity_failed_{};
};

[[nodiscard]] std::optional<std::string> complete_profile_change(
    HelperRuntimeConversation& conversation,
    const ProfileChange& change,
    ProfileChangeAdapter& adapter);

class SupervisorHelperConversation {
public:
    [[nodiscard]] std::optional<std::string> begin_profile_change(
        const ProfileChange& change);
    [[nodiscard]] std::vector<HelperConversationEvent> receive_bytes(
        std::string_view bytes);
    [[nodiscard]] std::vector<HelperConversationEvent> peer_closed();

private:
    enum class State {
        waiting_for_ready,
        waiting_for_activation,
        active,
        profile_state_unknown,
        connectivity_failed,
        waiting_for_cleanup,
        finished,
        failed,
    };

    std::string buffer_;
    std::optional<ProfileChange> pending_profile_change_;
    State state_{State::waiting_for_ready};
};

inline constexpr std::size_t maximum_helper_message_size{1024};
inline constexpr std::string_view helper_error_prefix{"ERROR "};
inline constexpr std::string_view invalid_runtime_command_message{
    "ERROR INVALID_RUNTIME_COMMAND\n"};

[[nodiscard]] std::optional<HelperRuntimeCommand> parse_helper_runtime_command(
    std::string_view line);
[[nodiscard]] std::string helper_runtime_command_message(
    const HelperRuntimeCommand& command);

[[nodiscard]] std::string helper_conversation_event_message(
    const HelperConversationEvent& event);
[[nodiscard]] std::string helper_error_message(std::string_view safe_message);

} // namespace netlaglab
