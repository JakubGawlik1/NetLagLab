#pragma once

#include "session_lifecycle.hpp"

#include <iosfwd>
#include <optional>
#include <string_view>

namespace netlaglab {

[[nodiscard]] int report_attached_session_outcome(
    const SessionOutcome& outcome,
    std::ostream& output,
    std::ostream& error);

[[nodiscard]] std::optional<int> report_legacy_attached_session_outcome(
    std::string_view line,
    std::ostream& output,
    std::ostream& error);

} // namespace netlaglab
