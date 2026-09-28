#include "attach_session_presentation.hpp"

#include "session_presentation.hpp"

#include <ostream>

namespace netlaglab {

int report_attached_session_outcome(
    const SessionOutcome& outcome,
    std::ostream& output,
    std::ostream& error)
{
    if (outcome.infrastructure_succeeded()) {
        if (!outcome.workload.has_value()) {
            error << "NetLagLab: invalid Session Outcome.\n";
            return 1;
        }
        if (outcome.workload->kind == WorkloadResultKind::exited) {
            output << "Session ended; Workload exit code: "
                   << outcome.workload->value << ".\n";
        } else {
            output << "Session ended; Workload terminated by signal "
                   << outcome.workload->value << ".\n";
        }
        return 0;
    }

    error << "NetLagLab: Session infrastructure failed: ";
    for (std::size_t index{}; index < outcome.infrastructure_failures.size();
         ++index) {
        if (index != 0) {
            error << ", ";
        }
        error << infrastructure_failure_name(
            outcome.infrastructure_failures[index]);
    }
    if (!outcome.workload.has_value()) {
        error << "; Workload result is unknown.\n";
    } else if (outcome.workload->kind == WorkloadResultKind::signaled) {
        error << "; Workload terminated by signal " << outcome.workload->value
              << ".\n";
    } else {
        error << "; Workload exit code: " << outcome.workload->value << ".\n";
    }
    return 1;
}

std::optional<int> report_legacy_attached_session_outcome(
    const std::string_view line,
    std::ostream& output,
    std::ostream& error)
{
    if (line == "SESSION_ENDED") {
        output << "Session ended.\n";
        return 0;
    }
    if (line == "SESSION_FAILED") {
        error << "NetLagLab: session failed\n";
        return 1;
    }
    return std::nullopt;
}

} // namespace netlaglab
