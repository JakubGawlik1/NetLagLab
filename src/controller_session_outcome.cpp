#include "controller_session_outcome.hpp"

#include <array>
#include <charconv>
#include <optional>
#include <utility>

namespace netlaglab {
namespace {

constexpr std::string_view begin_marker{"SESSION_OUTCOME_BEGIN"};
constexpr std::string_view end_marker{"SESSION_OUTCOME_END"};
constexpr std::string_view workload_exit_prefix{"WORKLOAD EXIT "};
constexpr std::string_view workload_signal_prefix{"WORKLOAD SIGNAL "};
constexpr std::string_view workload_unknown{"WORKLOAD UNKNOWN"};
constexpr std::string_view infrastructure_ok{"INFRASTRUCTURE OK"};
constexpr std::string_view infrastructure_failed{"INFRASTRUCTURE FAILED"};
constexpr std::string_view failure_prefix{"FAILURE "};

struct FailureWireMapping {
    InfrastructureFailure failure;
    std::string_view code;
};

constexpr std::array<FailureWireMapping, 8> failure_wire_mappings{{
    {InfrastructureFailure::start, "STARTUP"},
    {InfrastructureFailure::conversation, "HELPER_CONVERSATION"},
    {InfrastructureFailure::profile_state, "PROFILE_STATE"},
    {InfrastructureFailure::stop_request, "STOP_REQUEST"},
    {InfrastructureFailure::cleanup, "PRIVILEGED_CLEANUP"},
    {InfrastructureFailure::launcher_reaping, "LAUNCHER_REAPING"},
    {InfrastructureFailure::supervisor_cleanup, "SUPERVISOR_CLEANUP"},
    {InfrastructureFailure::invalid_event, "INVALID_LIFECYCLE_EVENT"},
}};

struct ParsedWorkload {
    std::optional<WorkloadResult> value;
};

[[nodiscard]] std::optional<int> parse_bounded_decimal(
    const std::string_view token,
    const int minimum,
    const int maximum)
{
    int value{};
    const auto result{
        std::from_chars(token.data(), token.data() + token.size(), value)};
    if (token.empty() || result.ec != std::errc{}
        || result.ptr != token.data() + token.size() || value < minimum
        || value > maximum) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::variant<ParsedWorkload, ControllerSessionOutcomeProtocolError>
parse_workload_line(const std::string_view line)
{
    if (line == workload_unknown) {
        return ParsedWorkload{std::nullopt};
    }
    if (line.starts_with(workload_exit_prefix)) {
        const std::optional<int> value{parse_bounded_decimal(
            line.substr(workload_exit_prefix.size()), 0, 255)};
        if (!value.has_value()) {
            return ControllerSessionOutcomeProtocolError::invalid_value;
        }
        return ParsedWorkload{
            WorkloadResult{WorkloadResultKind::exited, *value}};
    }
    if (line.starts_with(workload_signal_prefix)) {
        const std::optional<int> value{parse_bounded_decimal(
            line.substr(workload_signal_prefix.size()), 1, 127)};
        if (!value.has_value()) {
            return ControllerSessionOutcomeProtocolError::invalid_value;
        }
        return ParsedWorkload{
            WorkloadResult{WorkloadResultKind::signaled, *value}};
    }
    return ControllerSessionOutcomeProtocolError::invalid_structure;
}

[[nodiscard]] std::optional<InfrastructureFailure> parse_failure_code(
    const std::string_view code)
{
    for (const FailureWireMapping& mapping : failure_wire_mappings) {
        if (mapping.code == code) {
            return mapping.failure;
        }
    }
    return std::nullopt;
}

[[nodiscard]] const FailureWireMapping* failure_mapping(
    const InfrastructureFailure failure) noexcept
{
    for (const FailureWireMapping& mapping : failure_wire_mappings) {
        if (mapping.failure == failure) {
            return &mapping;
        }
    }
    return nullptr;
}

class LineReader {
public:
    explicit LineReader(const std::string_view input) noexcept : input_{input}
    {
    }

    [[nodiscard]] std::optional<std::string_view> next()
    {
        const std::size_t newline{input_.find('\n', position_)};
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view line{
            input_.substr(position_, newline - position_)};
        position_ = newline + 1;
        return line;
    }

    [[nodiscard]] bool exhausted() const noexcept
    {
        return position_ == input_.size();
    }

private:
    std::string_view input_;
    std::size_t position_{};
};

[[nodiscard]] ControllerSessionOutcomeParseResult parse_block(
    const std::string_view input)
{
    LineReader lines{input};
    const std::optional<std::string_view> begin{lines.next()};
    if (!begin.has_value()) {
        return IncompleteControllerSessionOutcome{};
    }
    if (*begin != begin_marker) {
        return ControllerSessionOutcomeProtocolError::invalid_structure;
    }

    const std::optional<std::string_view> workload_line{lines.next()};
    if (!workload_line.has_value()) {
        return IncompleteControllerSessionOutcome{};
    }
    const auto workload{parse_workload_line(*workload_line)};
    const auto* parsed_workload{std::get_if<ParsedWorkload>(&workload)};
    if (parsed_workload == nullptr) {
        return std::get<ControllerSessionOutcomeProtocolError>(workload);
    }

    const std::optional<std::string_view> infrastructure_line{lines.next()};
    if (!infrastructure_line.has_value()) {
        return IncompleteControllerSessionOutcome{};
    }
    const bool succeeded{*infrastructure_line == infrastructure_ok};
    if (!succeeded && *infrastructure_line != infrastructure_failed) {
        return ControllerSessionOutcomeProtocolError::invalid_structure;
    }
    if (succeeded && !parsed_workload->value.has_value()) {
        return ControllerSessionOutcomeProtocolError::impossible_outcome;
    }

    std::vector<InfrastructureFailure> failures;
    std::array<bool, failure_wire_mappings.size()> seen_failures{};
    while (true) {
        const std::optional<std::string_view> line{lines.next()};
        if (!line.has_value()) {
            return IncompleteControllerSessionOutcome{};
        }
        if (*line == end_marker) {
            if (!lines.exhausted()) {
                return ControllerSessionOutcomeProtocolError::trailing_data;
            }
            if (succeeded && !failures.empty()) {
                return ControllerSessionOutcomeProtocolError::impossible_outcome;
            }
            if (!succeeded && failures.empty()) {
                return ControllerSessionOutcomeProtocolError::impossible_outcome;
            }
            return SessionOutcome{
                .workload = parsed_workload->value,
                .infrastructure_failures = std::move(failures),
            };
        }
        if (succeeded || !line->starts_with(failure_prefix)) {
            return ControllerSessionOutcomeProtocolError::invalid_structure;
        }

        const std::optional<InfrastructureFailure> failure{
            parse_failure_code(line->substr(failure_prefix.size()))};
        if (!failure.has_value()) {
            return ControllerSessionOutcomeProtocolError::invalid_structure;
        }
        const FailureWireMapping* const mapping{failure_mapping(*failure)};
        if (mapping == nullptr) {
            return ControllerSessionOutcomeProtocolError::invalid_structure;
        }
        const std::size_t index{
            static_cast<std::size_t>(mapping - failure_wire_mappings.data())};
        if (seen_failures[index]) {
            return ControllerSessionOutcomeProtocolError::invalid_structure;
        }
        seen_failures[index] = true;
        failures.push_back(*failure);
    }
}

[[nodiscard]] bool valid_workload_for_wire(
    const std::optional<WorkloadResult>& workload) noexcept
{
    if (!workload.has_value()) {
        return true;
    }
    if (workload->kind == WorkloadResultKind::exited) {
        return workload->value >= 0 && workload->value <= 255;
    }
    if (workload->kind == WorkloadResultKind::signaled) {
        return workload->value >= 1 && workload->value <= 127;
    }
    return false;
}

} // namespace

ControllerSessionOutcomeSerialization serialize_controller_session_outcome(
    const SessionOutcome& outcome)
{
    if (!valid_workload_for_wire(outcome.workload)
        || (outcome.infrastructure_succeeded() && !outcome.workload.has_value())) {
        return ControllerSessionOutcomeProtocolError::impossible_outcome;
    }

    std::array<bool, failure_wire_mappings.size()> seen_failures{};
    for (const InfrastructureFailure failure : outcome.infrastructure_failures) {
        const FailureWireMapping* const mapping{failure_mapping(failure)};
        if (mapping == nullptr) {
            return ControllerSessionOutcomeProtocolError::impossible_outcome;
        }
        const std::size_t index{
            static_cast<std::size_t>(mapping - failure_wire_mappings.data())};
        if (seen_failures[index]) {
            return ControllerSessionOutcomeProtocolError::impossible_outcome;
        }
        seen_failures[index] = true;
    }

    std::string block{"SESSION_OUTCOME_BEGIN\n"};
    if (!outcome.workload.has_value()) {
        block += "WORKLOAD UNKNOWN\n";
    } else if (outcome.workload->kind == WorkloadResultKind::exited) {
        block += "WORKLOAD EXIT " + std::to_string(outcome.workload->value) + '\n';
    } else {
        block += "WORKLOAD SIGNAL " + std::to_string(outcome.workload->value) + '\n';
    }

    block += outcome.infrastructure_succeeded() ? "INFRASTRUCTURE OK\n"
                                                : "INFRASTRUCTURE FAILED\n";
    for (const InfrastructureFailure failure : outcome.infrastructure_failures) {
        const FailureWireMapping* const mapping{failure_mapping(failure)};
        if (mapping == nullptr) {
            return ControllerSessionOutcomeProtocolError::impossible_outcome;
        }
        block += "FAILURE ";
        block += mapping->code;
        block += '\n';
    }
    block += "SESSION_OUTCOME_END\n";
    if (block.size() > maximum_controller_session_outcome_size) {
        return ControllerSessionOutcomeProtocolError::message_too_large;
    }
    return block;
}

ControllerSessionOutcomeParseResult ControllerSessionOutcomeParser::receive(
    const std::string_view bytes)
{
    if (completed_) {
        return ControllerSessionOutcomeProtocolError::trailing_data;
    }
    if (buffer_.size() + bytes.size() > maximum_controller_session_outcome_size) {
        return ControllerSessionOutcomeProtocolError::message_too_large;
    }
    buffer_.append(bytes);

    ControllerSessionOutcomeParseResult result{parse_block(buffer_)};
    if (std::holds_alternative<SessionOutcome>(result)) {
        completed_ = true;
    }
    return result;
}

ControllerSessionOutcomeParseResult ControllerSessionOutcomeParser::finish()
{
    if (completed_) {
        return ControllerSessionOutcomeProtocolError::trailing_data;
    }
    const ControllerSessionOutcomeParseResult result{parse_block(buffer_)};
    if (std::holds_alternative<ControllerSessionOutcomeProtocolError>(result)) {
        return result;
    }
    return ControllerSessionOutcomeProtocolError::incomplete_input;
}

} // namespace netlaglab
