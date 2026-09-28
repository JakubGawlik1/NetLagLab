#pragma once

#include "session_lifecycle.hpp"

#include <iosfwd>
#include <string_view>

namespace netlaglab {

[[nodiscard]] int session_exit_status(const SessionOutcome& outcome) noexcept;
[[nodiscard]] std::string_view infrastructure_failure_name(
    InfrastructureFailure failure) noexcept;
void report_session_outcome(const SessionOutcome& outcome, std::ostream& error);

} // namespace netlaglab
