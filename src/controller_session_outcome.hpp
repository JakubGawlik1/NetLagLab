#pragma once

#include "session_lifecycle.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace netlaglab {

inline constexpr std::size_t maximum_controller_session_outcome_size{1024};

struct IncompleteControllerSessionOutcome {
};

enum class ControllerSessionOutcomeProtocolError {
    invalid_structure,
    invalid_value,
    impossible_outcome,
    message_too_large,
    incomplete_input,
    trailing_data,
};

using ControllerSessionOutcomeSerialization = std::variant<
    std::string,
    ControllerSessionOutcomeProtocolError>;

using ControllerSessionOutcomeParseResult = std::variant<
    IncompleteControllerSessionOutcome,
    SessionOutcome,
    ControllerSessionOutcomeProtocolError>;

[[nodiscard]] ControllerSessionOutcomeSerialization
serialize_controller_session_outcome(const SessionOutcome& outcome);

class ControllerSessionOutcomeParser {
public:
    [[nodiscard]] ControllerSessionOutcomeParseResult receive(
        std::string_view bytes);
    [[nodiscard]] ControllerSessionOutcomeParseResult finish();

private:
    std::string buffer_;
    bool completed_{false};
};

} // namespace netlaglab
