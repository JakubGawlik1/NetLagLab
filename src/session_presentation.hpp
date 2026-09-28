#pragma once

#include "session_lifecycle.hpp"

#include <iosfwd>

namespace netlaglab {

[[nodiscard]] int session_exit_status(const SessionOutcome& outcome) noexcept;
void report_session_outcome(const SessionOutcome& outcome, std::ostream& error);

} // namespace netlaglab
