#include "helper_protocol.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

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

[[nodiscard]] std::optional<std::vector<std::string_view>> split_wire_tokens(
    const std::string_view line)
{
    if (line.empty() || line.front() == ' ' || line.back() == ' ') {
        return std::nullopt;
    }
    std::vector<std::string_view> tokens;
    std::size_t begin{};
    while (begin < line.size()) {
        const std::size_t separator{line.find(' ', begin)};
        const std::size_t end{
            separator == std::string_view::npos ? line.size() : separator};
        if (end == begin) {
            return std::nullopt;
        }
        tokens.push_back(line.substr(begin, end - begin));
        if (separator == std::string_view::npos) {
            break;
        }
        begin = separator + 1;
    }
    return tokens;
}

[[nodiscard]] std::optional<TrafficDirection> parse_direction(
    const std::string_view token)
{
    if (token == "OUTBOUND") {
        return TrafficDirection::outbound;
    }
    if (token == "INBOUND") {
        return TrafficDirection::inbound;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<NetworkSetting> parse_setting(
    const std::string_view token)
{
    if (token == "DELAY") {
        return NetworkSetting::delay;
    }
    if (token == "JITTER") {
        return NetworkSetting::jitter;
    }
    if (token == "LOSS") {
        return NetworkSetting::packet_loss;
    }
    if (token == "BANDWIDTH") {
        return NetworkSetting::bandwidth;
    }
    return std::nullopt;
}

[[nodiscard]] bool has_canonical_unsigned_spelling(const std::string_view token)
{
    if (token.empty() || (token.size() > 1 && token.front() == '0')) {
        return false;
    }
    for (const char character : token) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> parse_canonical_unsigned(
    const std::string_view token,
    const std::uint64_t maximum)
{
    if (!has_canonical_unsigned_spelling(token)) {
        return std::nullopt;
    }
    std::uint64_t value{};
    const auto result{
        std::from_chars(token.data(), token.data() + token.size(), value)};
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()
        || value > maximum) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string> serialize_loss(const double value)
{
    if (!std::isfinite(value) || value < 0.0 || value > 100.0) {
        return std::nullopt;
    }
    const double normalized{value == 0.0 ? 0.0 : value};
    char buffer[64]{};
    const auto result{std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        normalized,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10)};
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }
    return std::string(buffer, result.ptr);
}

[[nodiscard]] std::optional<double> parse_canonical_loss(
    const std::string_view token)
{
    double value{};
    const auto result{std::from_chars(
        token.data(), token.data() + token.size(), value, std::chars_format::general)};
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    const std::optional<std::string> canonical{serialize_loss(value)};
    if (!canonical.has_value() || *canonical != token) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string_view direction_token(const TrafficDirection direction)
{
    switch (direction) {
    case TrafficDirection::outbound:
        return "OUTBOUND";
    case TrafficDirection::inbound:
        return "INBOUND";
    }
    return {};
}

[[nodiscard]] std::string_view setting_token(const NetworkSetting setting)
{
    switch (setting) {
    case NetworkSetting::delay:
        return "DELAY";
    case NetworkSetting::jitter:
        return "JITTER";
    case NetworkSetting::packet_loss:
        return "LOSS";
    case NetworkSetting::bandwidth:
        return "BANDWIDTH";
    }
    return {};
}

[[nodiscard]] std::optional<ProfileChange> parse_profile_change(
    const std::vector<std::string_view>& tokens)
{
    if (tokens.size() != 3) {
        return std::nullopt;
    }
    const std::optional<TrafficDirection> direction{parse_direction(tokens[1])};
    if (!direction.has_value()) {
        return std::nullopt;
    }

    if (tokens[0] == "PROFILE_SET_DELAY"
        || tokens[0] == "PROFILE_SET_JITTER") {
        using Rep = std::chrono::milliseconds::rep;
        const auto value{parse_canonical_unsigned(
            tokens[2], static_cast<std::uint64_t>(std::numeric_limits<Rep>::max()))};
        if (!value.has_value()) {
            return std::nullopt;
        }
        const std::chrono::milliseconds milliseconds{static_cast<Rep>(*value)};
        if (tokens[0] == "PROFILE_SET_DELAY") {
            return ProfileChange{SetDelay{*direction, milliseconds}};
        }
        return ProfileChange{SetJitter{*direction, milliseconds}};
    }
    if (tokens[0] == "PROFILE_SET_BANDWIDTH") {
        const auto value{parse_canonical_unsigned(
            tokens[2], std::numeric_limits<std::uint64_t>::max())};
        if (!value.has_value() || *value == 0) {
            return std::nullopt;
        }
        return ProfileChange{SetBandwidth{*direction, *value}};
    }
    if (tokens[0] == "PROFILE_SET_LOSS") {
        const auto value{parse_canonical_loss(tokens[2])};
        if (!value.has_value()) {
            return std::nullopt;
        }
        return ProfileChange{SetPacketLoss{*direction, *value}};
    }
    if (tokens[0] == "PROFILE_RESET") {
        const auto setting{parse_setting(tokens[2])};
        if (!setting.has_value()) {
            return std::nullopt;
        }
        return ProfileChange{ResetSetting{*direction, *setting}};
    }
    return std::nullopt;
}

[[nodiscard]] std::string serialize_profile_change(const ProfileChange& change)
{
    if (std::holds_alternative<std::vector<ValidationError>>(
            apply_profile_change(NetworkProfile{}, change))) {
        return {};
    }
    return std::visit(
        [](const auto& operation) -> std::string {
            using Operation = std::decay_t<decltype(operation)>;
            const std::string_view direction{direction_token(operation.direction)};
            if (direction.empty()) {
                return {};
            }
            if constexpr (std::is_same_v<Operation, SetDelay>) {
                return "PROFILE_SET_DELAY " + std::string(direction) + ' '
                    + std::to_string(operation.value.count()) + '\n';
            } else if constexpr (std::is_same_v<Operation, SetJitter>) {
                return "PROFILE_SET_JITTER " + std::string(direction) + ' '
                    + std::to_string(operation.value.count()) + '\n';
            } else if constexpr (std::is_same_v<Operation, SetPacketLoss>) {
                const auto loss{serialize_loss(operation.percent)};
                if (!loss.has_value()) {
                    return {};
                }
                return "PROFILE_SET_LOSS " + std::string(direction) + ' '
                    + *loss + '\n';
            } else if constexpr (std::is_same_v<Operation, SetBandwidth>) {
                return "PROFILE_SET_BANDWIDTH " + std::string(direction) + ' '
                    + std::to_string(operation.kbps) + '\n';
            } else {
                const std::string_view setting{setting_token(operation.setting)};
                if (setting.empty()) {
                    return {};
                }
                return "PROFILE_RESET " + std::string(direction) + ' '
                    + std::string(setting) + '\n';
            }
        },
        change);
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
        return {false, {}, std::nullopt};
    }
    buffer_.append(bytes);
    std::vector<HelperRuntimeCommand> commands;
    while (true) {
        const std::size_t newline{buffer_.find('\n')};
        if (newline == std::string::npos) {
            if (buffer_.size() > maximum_helper_message_size) {
                valid_ = false;
                buffer_.clear();
                return {false, std::move(commands),
                        std::string(invalid_runtime_command_message)};
            }
            return {true, std::move(commands), std::nullopt};
        }
        if (newline > maximum_helper_message_size) {
            valid_ = false;
            buffer_.clear();
            return {false, std::move(commands),
                    std::string(invalid_runtime_command_message)};
        }

        const std::string line{buffer_.substr(0, newline)};
        buffer_.erase(0, newline + 1);
        const std::optional<HelperRuntimeCommand> command{
            parse_helper_runtime_command(line)};
        if (!command.has_value() || workload_finished_
            || (std::holds_alternative<ProfileChange>(*command)
                && pending_profile_change_.has_value())) {
            valid_ = false;
            buffer_.clear();
            return {false, std::move(commands),
                    std::string(invalid_runtime_command_message)};
        }
        if (const auto* change{std::get_if<ProfileChange>(&*command)}) {
            pending_profile_change_ = *change;
        }
        commands.push_back(*command);
    }
}

std::optional<std::string> HelperRuntimeConversation::complete_profile_change(
    const ProfileChangeCompletion completion)
{
    if (!valid_ || workload_finished_ || !pending_profile_change_.has_value()) {
        return std::nullopt;
    }
    pending_profile_change_.reset();
    switch (completion) {
    case ProfileChangeCompletion::applied:
        return "PROFILE_OK\n";
    case ProfileChangeCompletion::restored_after_failure:
        return "PROFILE_FAILED APPLY_FAILED\n";
    case ProfileChangeCompletion::state_unknown:
        valid_ = false;
        buffer_.clear();
        return "ERROR PROFILE_STATE_UNKNOWN\n";
    }
    return std::nullopt;
}

void HelperRuntimeConversation::workload_finished() noexcept
{
    workload_finished_ = true;
    pending_profile_change_.reset();
    buffer_.clear();
}

std::optional<HelperRuntimeCommand> parse_helper_runtime_command(
    const std::string_view line)
{
    if (line == "STOP TERM") {
        return HelperRuntimeCommand{StopTerminateCommand{}};
    }
    if (line == "STOP KILL") {
        return HelperRuntimeCommand{StopKillCommand{}};
    }
    if (line == "SHUTDOWN") {
        return HelperRuntimeCommand{CompatibilityShutdownCommand{}};
    }
    const auto tokens{split_wire_tokens(line)};
    if (!tokens.has_value()) {
        return std::nullopt;
    }
    const auto change{parse_profile_change(*tokens)};
    if (!change.has_value()) {
        return std::nullopt;
    }
    return HelperRuntimeCommand{*change};
}

std::string helper_runtime_command_message(const HelperRuntimeCommand& command)
{
    return std::visit(
        [](const auto& value) -> std::string {
            using Command = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Command, StopTerminateCommand>) {
                return "STOP TERM\n";
            } else if constexpr (std::is_same_v<Command, StopKillCommand>) {
                return "STOP KILL\n";
            } else if constexpr (
                std::is_same_v<Command, CompatibilityShutdownCommand>) {
                return "SHUTDOWN\n";
            } else {
                return serialize_profile_change(value);
            }
        },
        command);
}

std::string helper_conversation_event_message(const HelperConversationEvent& event)
{
    return std::visit(
        [](const auto& value) -> std::string {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, ReadyEvent>) {
                return "READY\n";
            } else if constexpr (std::is_same_v<Event, ActivatedEvent>) {
                return "ACTIVE " + std::to_string(value.workload_pid) + '\n';
            } else if constexpr (std::is_same_v<Event, ActivationFailedEvent>) {
                return "START_FAILED " + std::to_string(value.result) + '\n';
            } else if constexpr (std::is_same_v<Event, WorkloadExitedEvent>) {
                return "WORKLOAD_EXITED " + std::to_string(value.result) + '\n';
            } else if constexpr (std::is_same_v<Event, WorkloadSignaledEvent>) {
                return "WORKLOAD_SIGNALED " + std::to_string(value.signal) + '\n';
            } else if constexpr (std::is_same_v<Event, CleanupSucceededEvent>) {
                return "CLEANUP_OK\n";
            } else if constexpr (std::is_same_v<Event, CleanupFailedEvent>) {
                return "CLEANUP_FAILED\n";
            } else if constexpr (std::is_same_v<Event, ProfileChangeResultEvent>) {
                return value.result == ProfileChangeResult::applied
                    ? "PROFILE_OK\n" : "PROFILE_FAILED APPLY_FAILED\n";
            } else if constexpr (std::is_same_v<Event, ProfileStateUnknownEvent>) {
                return "ERROR PROFILE_STATE_UNKNOWN\n";
            } else {
                return {};
            }
        },
        event);
}

std::string helper_error_message(const std::string_view safe_message)
{
    std::string message{helper_error_prefix};
    message.append(safe_message);
    message.push_back('\n');
    return message;
}

std::optional<std::string> SupervisorHelperConversation::begin_profile_change(
    const ProfileChange& change)
{
    if (state_ != State::active || pending_profile_change_.has_value()) {
        return std::nullopt;
    }
    std::string message{serialize_profile_change(change)};
    if (message.empty()) {
        return std::nullopt;
    }
    pending_profile_change_ = change;
    return message;
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
                pending_profile_change_.reset();
                buffer_.clear();
                events.emplace_back(ConversationLostEvent{});
            }
            return events;
        }

        if (newline > maximum_helper_message_size) {
            state_ = State::failed;
            pending_profile_change_.reset();
            buffer_.clear();
            events.emplace_back(ConversationLostEvent{});
            return events;
        }

        const std::string line{buffer_.substr(0, newline)};
        buffer_.erase(0, newline + 1);
        std::optional<HelperConversationEvent> event;

        if (state_ == State::active && pending_profile_change_.has_value()
            && line == "PROFILE_OK") {
            event = ProfileChangeResultEvent{
                ProfileChangeResult::applied, *pending_profile_change_};
            pending_profile_change_.reset();
        } else if (state_ == State::active && pending_profile_change_.has_value()
                   && line == "PROFILE_FAILED APPLY_FAILED") {
            event = ProfileChangeResultEvent{
                ProfileChangeResult::restored_after_failure,
                *pending_profile_change_};
            pending_profile_change_.reset();
        } else if (state_ == State::active && pending_profile_change_.has_value()
                   && line == "ERROR PROFILE_STATE_UNKNOWN") {
            pending_profile_change_.reset();
            state_ = State::failed;
            event = ProfileStateUnknownEvent{};
        } else if (line.starts_with(helper_error_prefix)) {
            pending_profile_change_.reset();
            state_ = State::failed;
            event = ConversationLostEvent{};
        } else if (state_ == State::waiting_for_ready && line == "READY") {
            event = ReadyEvent{};
            state_ = State::waiting_for_activation;
        } else if (state_ == State::waiting_for_activation) {
            if (const auto pid{parse_value_after(line, "ACTIVE ", 1, 1'000'000'000)}) {
                event = ActivatedEvent{*pid};
                state_ = State::active;
            } else if (const auto result{
                           parse_value_after(line, "START_FAILED ", 125, 127)}) {
                event = ActivationFailedEvent{*result};
                state_ = State::waiting_for_cleanup;
            }
        } else if (state_ == State::active) {
            if (const auto result{
                    parse_value_after(line, "WORKLOAD_EXITED ", 0, 255)}) {
                event = WorkloadExitedEvent{*result};
                pending_profile_change_.reset();
                state_ = State::waiting_for_cleanup;
            } else if (const auto signal{
                           parse_value_after(line, "WORKLOAD_SIGNALED ", 1, 127)}) {
                event = WorkloadSignaledEvent{*signal};
                pending_profile_change_.reset();
                state_ = State::waiting_for_cleanup;
            }
        } else if (state_ == State::waiting_for_cleanup) {
            if (line == "CLEANUP_OK") {
                event = CleanupSucceededEvent{};
                state_ = State::finished;
            } else if (line == "CLEANUP_FAILED") {
                event = CleanupFailedEvent{};
                state_ = State::finished;
            }
        }

        if (!event.has_value()) {
            state_ = State::failed;
            pending_profile_change_.reset();
            buffer_.clear();
            events.emplace_back(ConversationLostEvent{});
            return events;
        }
        events.push_back(std::move(*event));
        if (state_ == State::failed) {
            buffer_.clear();
            return events;
        }
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
    pending_profile_change_.reset();
    buffer_.clear();
    return {ConversationLostEvent{}};
}

} // namespace netlaglab
