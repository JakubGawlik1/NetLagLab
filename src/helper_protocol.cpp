#include "helper_protocol.hpp"

#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace netlaglab {
namespace {

constexpr std::size_t maximum_encoded_start_block_size{
    maximum_context_total_size * 2
    + (maximum_context_entry_count * 2 + 4) * 6};
constexpr std::size_t maximum_encoded_context_line_size{
    maximum_context_value_size * 2 + 5};

[[nodiscard]] std::optional<int> parse_bounded_integer(
    const std::string_view text,
    const int minimum,
    const int maximum)
{
    int value{};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || value < minimum || value > maximum) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<int> parse_value_after(
    const std::string_view line,
    const std::string_view prefix,
    const int minimum,
    const int maximum)
{
    if (!line.starts_with(prefix)) {
        return std::nullopt;
    }
    return parse_bounded_integer(line.substr(prefix.size()), minimum, maximum);
}

} // namespace

StartBlockFeedResult HelperStartConversation::receive_bytes(
    const std::string_view bytes)
{
    if (finished_) {
        return {StartBlockState::invalid, std::nullopt};
    }
    buffer_.append(bytes);
    while (true) {
        const std::size_t newline{buffer_.find('\n')};
        if (newline == std::string::npos) {
            if (buffer_.size() > maximum_encoded_context_line_size
                || encoded_size_ > maximum_encoded_start_block_size - buffer_.size()) {
                finished_ = true;
                return {StartBlockState::invalid, std::nullopt};
            }
            return {StartBlockState::incomplete, std::nullopt};
        }
        if (newline > maximum_encoded_context_line_size
            || encoded_size_ > maximum_encoded_start_block_size - (newline + 1)
            || lines_.size() > maximum_context_entry_count * 2 + 2) {
            finished_ = true;
            return {StartBlockState::invalid, std::nullopt};
        }

        std::string line{buffer_.substr(0, newline)};
        buffer_.erase(0, newline + 1);
        encoded_size_ += line.size() + 1;
        if (lines_.empty()) {
            if (line != "START_BEGIN") {
                finished_ = true;
                return {StartBlockState::invalid, std::nullopt};
            }
            started_ = true;
        }
        lines_.push_back(std::move(line));
        if (lines_.back() != "START_END") {
            continue;
        }

        finished_ = true;
        if (!buffer_.empty()) {
            return {StartBlockState::invalid, std::nullopt};
        }
        std::optional<WorkloadContext> context{parse_start_block(lines_)};
        if (!context.has_value()) {
            return {StartBlockState::invalid, std::nullopt};
        }
        return {StartBlockState::complete, std::move(context)};
    }
}

bool HelperStartConversation::started() const noexcept
{
    return started_;
}

RuntimeCommandFeedResult HelperRuntimeConversation::receive_bytes(
    const std::string_view bytes)
{
    if (!valid_) {
        return {false, {}};
    }
    buffer_.append(bytes);
    std::vector<HelperRuntimeCommand> commands;
    while (true) {
        const std::size_t newline{buffer_.find('\n')};
        if (newline == std::string::npos) {
            if (buffer_.size() > maximum_helper_message_size) {
                valid_ = false;
            }
            return {valid_, std::move(commands)};
        }
        if (newline > maximum_helper_message_size) {
            valid_ = false;
            return {false, {}};
        }
        const std::string line{buffer_.substr(0, newline)};
        buffer_.erase(0, newline + 1);
        const std::optional<HelperRuntimeCommand> command{
            parse_helper_runtime_command(line)};
        if (!command.has_value()) {
            valid_ = false;
            return {false, {}};
        }
        commands.push_back(*command);
    }
}

std::optional<HelperRuntimeCommand> parse_helper_runtime_command(
    const std::string_view line)
{
    if (line == "STOP TERM") {
        return HelperRuntimeCommand::stop_terminate;
    }
    if (line == "STOP KILL") {
        return HelperRuntimeCommand::stop_kill;
    }
    if (line == "SHUTDOWN") {
        return HelperRuntimeCommand::shutdown;
    }
    return std::nullopt;
}

std::string_view helper_runtime_command_message(const HelperRuntimeCommand command)
{
    switch (command) {
    case HelperRuntimeCommand::stop_terminate:
        return "STOP TERM\n";
    case HelperRuntimeCommand::stop_kill:
        return "STOP KILL\n";
    case HelperRuntimeCommand::shutdown:
        return "SHUTDOWN\n";
    }
    return {};
}

std::string helper_conversation_event_message(
    const HelperConversationEventKind event,
    const int value)
{
    switch (event) {
    case HelperConversationEventKind::ready:
        return "READY\n";
    case HelperConversationEventKind::activated:
        return "ACTIVE " + std::to_string(value) + '\n';
    case HelperConversationEventKind::activation_failed:
        return "START_FAILED " + std::to_string(value) + '\n';
    case HelperConversationEventKind::workload_exited:
        return "WORKLOAD_EXITED " + std::to_string(value) + '\n';
    case HelperConversationEventKind::workload_signaled:
        return "WORKLOAD_SIGNALED " + std::to_string(value) + '\n';
    case HelperConversationEventKind::cleanup_succeeded:
        return "CLEANUP_OK\n";
    case HelperConversationEventKind::cleanup_failed:
        return "CLEANUP_FAILED\n";
    case HelperConversationEventKind::conversation_lost:
        return {};
    }
    return {};
}

std::vector<HelperConversationEvent> SupervisorHelperConversation::receive_bytes(
    const std::string_view bytes)
{
    std::vector<HelperConversationEvent> events;
    if (state_ == State::failed) {
        return events;
    }

    buffer_.append(bytes);
    while (true) {
        const std::size_t newline{buffer_.find('\n')};
        if (newline == std::string::npos) {
            if (buffer_.size() > maximum_helper_message_size) {
                state_ = State::failed;
                buffer_.clear();
                events.push_back({HelperConversationEventKind::conversation_lost});
            }
            return events;
        }

        if (newline > maximum_helper_message_size) {
            state_ = State::failed;
            buffer_.clear();
            events.push_back({HelperConversationEventKind::conversation_lost});
            return events;
        }

        const std::string line{buffer_.substr(0, newline)};
        buffer_.erase(0, newline + 1);

        if (line.starts_with(helper_error_prefix)) {
            state_ = State::failed;
            events.push_back({HelperConversationEventKind::conversation_lost});
            return events;
        }

        std::optional<HelperConversationEvent> event;
        if (state_ == State::waiting_for_ready && line == "READY") {
            event = HelperConversationEvent{HelperConversationEventKind::ready};
            state_ = State::waiting_for_activation;
        } else if (state_ == State::waiting_for_activation) {
            if (const auto pid{parse_value_after(line, "ACTIVE ", 1, 1'000'000'000)}) {
                event = HelperConversationEvent{
                    HelperConversationEventKind::activated, *pid};
                state_ = State::active;
            } else if (const auto result{
                           parse_value_after(line, "START_FAILED ", 125, 127)}) {
                event = HelperConversationEvent{
                    HelperConversationEventKind::activation_failed, *result};
                state_ = State::waiting_for_cleanup;
            }
        } else if (state_ == State::active) {
            if (const auto result{
                    parse_value_after(line, "WORKLOAD_EXITED ", 0, 255)}) {
                event = HelperConversationEvent{
                    HelperConversationEventKind::workload_exited, *result};
                state_ = State::waiting_for_cleanup;
            } else if (const auto signal{
                           parse_value_after(line, "WORKLOAD_SIGNALED ", 1, 127)}) {
                event = HelperConversationEvent{
                    HelperConversationEventKind::workload_signaled, *signal};
                state_ = State::waiting_for_cleanup;
            }
        } else if (state_ == State::waiting_for_cleanup) {
            if (line == "CLEANUP_OK") {
                event = HelperConversationEvent{
                    HelperConversationEventKind::cleanup_succeeded};
                state_ = State::finished;
            } else if (line == "CLEANUP_FAILED") {
                event = HelperConversationEvent{
                    HelperConversationEventKind::cleanup_failed};
                state_ = State::finished;
            }
        }

        if (!event.has_value()) {
            state_ = State::failed;
            buffer_.clear();
            events.push_back({HelperConversationEventKind::conversation_lost});
            return events;
        }
        events.push_back(*event);
    }
}

std::vector<HelperConversationEvent> SupervisorHelperConversation::peer_closed()
{
    if (state_ == State::failed) {
        return {};
    }
    if (state_ == State::finished && buffer_.empty()) {
        return {};
    }
    state_ = State::failed;
    buffer_.clear();
    return {{HelperConversationEventKind::conversation_lost}};
}

} // namespace netlaglab
