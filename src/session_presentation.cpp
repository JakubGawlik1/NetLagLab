#include "session_presentation.hpp"

#include <ostream>
#include <string_view>
#include <unistd.h>

namespace netlaglab {
namespace {

[[nodiscard]] int workload_exit_status(const WorkloadResult& result) noexcept
{
    return result.kind == WorkloadResultKind::signaled ? 128 + result.value
                                                       : result.value;
}

} // namespace

std::string_view infrastructure_failure_name(
    const InfrastructureFailure failure) noexcept
{
    switch (failure) {
    case InfrastructureFailure::start:
        return "startup";
    case InfrastructureFailure::conversation:
        return "helper conversation";
    case InfrastructureFailure::connectivity:
        return "Session connectivity";
    case InfrastructureFailure::profile_state:
        return "unknown network profile state";
    case InfrastructureFailure::stop_request:
        return "Workload stop request";
    case InfrastructureFailure::cleanup:
        return "privileged cleanup";
    case InfrastructureFailure::launcher_reaping:
        return "helper launcher reaping";
    case InfrastructureFailure::supervisor_cleanup:
        return "Supervisor cleanup";
    case InfrastructureFailure::invalid_event:
        return "invalid lifecycle event";
    }
    return "unknown infrastructure stage";
}

int session_exit_status(const SessionOutcome& outcome) noexcept
{
    if (!outcome.infrastructure_succeeded() || !outcome.workload.has_value()) {
        return 125;
    }
    return workload_exit_status(*outcome.workload);
}

void report_session_outcome(
    const SessionOutcome& outcome,
    std::ostream& error)
{
    if (!outcome.infrastructure_succeeded()) {
        error << "NetLagLab: Session infrastructure failed: ";
        for (std::size_t index{}; index < outcome.infrastructure_failures.size();
             ++index) {
            if (index != 0) {
                error << ", ";
            }
            error << infrastructure_failure_name(
                outcome.infrastructure_failures[index]);
        }
        if (outcome.workload.has_value()) {
            error << "; Workload result was "
                  << workload_exit_status(*outcome.workload);
        }
        error << '\n';
        return;
    }

    if (isatty(STDERR_FILENO) != 1 || !outcome.workload.has_value()) {
        return;
    }
    if (outcome.workload->kind == WorkloadResultKind::signaled) {
        error << "NetLagLab: Session ended; Workload terminated by signal "
              << outcome.workload->value << '\n';
    } else if (outcome.workload->kind == WorkloadResultKind::exited) {
        error << "NetLagLab: Session ended; Workload exit code: "
              << outcome.workload->value << '\n';
    }
}

} // namespace netlaglab
