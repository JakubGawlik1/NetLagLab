#pragma once

#include "workload_context.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace netlaglab {

enum class HelperRuntimeCommand {
    stop_terminate,
    stop_kill,
    shutdown,
};

enum class HelperConversationEventKind {
    ready,
    activated,
    activation_failed,
    workload_exited,
    workload_signaled,
    cleanup_succeeded,
    cleanup_failed,
    conversation_lost,
};

struct HelperConversationEvent {
    HelperConversationEventKind kind;
    int value{};
};

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
};

class HelperRuntimeConversation {
public:
    [[nodiscard]] RuntimeCommandFeedResult receive_bytes(std::string_view bytes);

private:
    std::string buffer_;
    bool valid_{true};
};

class SupervisorHelperConversation {
public:
    [[nodiscard]] std::vector<HelperConversationEvent> receive_bytes(
        std::string_view bytes);
    [[nodiscard]] std::vector<HelperConversationEvent> peer_closed();

private:
    enum class State {
        waiting_for_ready,
        waiting_for_activation,
        active,
        waiting_for_cleanup,
        finished,
        failed,
    };

    std::string buffer_;
    State state_{State::waiting_for_ready};
};

inline constexpr std::size_t maximum_helper_message_size{1024};
inline constexpr std::string_view helper_error_prefix{"ERROR "};

[[nodiscard]] std::optional<HelperRuntimeCommand> parse_helper_runtime_command(
    std::string_view line);
[[nodiscard]] std::string_view helper_runtime_command_message(
    HelperRuntimeCommand command);

[[nodiscard]] std::string helper_conversation_event_message(
    HelperConversationEventKind event,
    int value = 0);

} // namespace netlaglab
