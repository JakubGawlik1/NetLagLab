#include "session_lifecycle.hpp"

namespace netlaglab {

int WorkloadResult::exit_code() const noexcept
{
    if (kind == WorkloadResultKind::signaled) {
        return 128 + value;
    }

    return value;
}

bool SessionOutcome::infrastructure_succeeded() const noexcept
{
    return infrastructure_failures.empty();
}

int SessionOutcome::exit_code() const noexcept
{
    if (!infrastructure_succeeded() || !workload.has_value()) {
        return 125;
    }

    return workload->exit_code();
}

SessionOutcome run_session_lifecycle(LifecycleAdapter& adapter)
{
    SessionOutcome outcome;
    if (!adapter.begin()) {
        outcome.infrastructure_failures.push_back(InfrastructureFailure::start);
    } else {
        enum class State {
            waiting_for_activation,
            active,
            waiting_for_cleanup,
        };
        enum class StopStage {
            none,
            interrupt_grace,
            terminate_grace,
            kill_requested,
        };
        State state{State::waiting_for_activation};
        StopStage stop_stage{StopStage::none};

        while (true) {
            const bool deadline_active{
                stop_stage == StopStage::interrupt_grace
                || stop_stage == StopStage::terminate_grace};
            const LifecycleEvent event{adapter.wait(
                deadline_active ? LifecycleWait::five_seconds
                                : LifecycleWait::indefinitely)};

            if (event.kind == LifecycleEventKind::controller_lost) {
                continue;
            }

            if (event.kind == LifecycleEventKind::conversation_lost) {
                outcome.infrastructure_failures.push_back(
                    InfrastructureFailure::conversation);
                break;
            }

            if (event.kind == LifecycleEventKind::cleanup_failed) {
                outcome.infrastructure_failures.push_back(
                    InfrastructureFailure::cleanup);
                break;
            }

            if (state == State::waiting_for_activation) {
                if (event.kind == LifecycleEventKind::activated) {
                    state = State::active;
                    continue;
                }

                if (event.kind == LifecycleEventKind::activation_failed) {
                    if (event.value == 126 || event.value == 127) {
                        outcome.workload = WorkloadResult{
                            WorkloadResultKind::start_failure, event.value};
                    } else {
                        outcome.infrastructure_failures.push_back(
                            InfrastructureFailure::start);
                    }
                    state = State::waiting_for_cleanup;
                    continue;
                }
            } else if (state == State::active) {
                if (event.kind == LifecycleEventKind::terminal_interrupt) {
                    if (stop_stage == StopStage::none) {
                        stop_stage = StopStage::interrupt_grace;
                        continue;
                    }

                    if (stop_stage == StopStage::kill_requested) {
                        continue;
                    }

                    if (!adapter.request_stop(StopRequest::kill)) {
                        outcome.infrastructure_failures.push_back(
                            InfrastructureFailure::stop_request);
                        break;
                    }
                    stop_stage = StopStage::kill_requested;
                    continue;
                }

                if (event.kind == LifecycleEventKind::stop_requested
                    && stop_stage == StopStage::none) {
                    if (!adapter.request_stop(StopRequest::terminate)) {
                        outcome.infrastructure_failures.push_back(
                            InfrastructureFailure::stop_request);
                        break;
                    }
                    stop_stage = StopStage::terminate_grace;
                    continue;
                }

                if (event.kind == LifecycleEventKind::stop_requested) {
                    continue;
                }

                if (event.kind == LifecycleEventKind::deadline_expired
                    && stop_stage == StopStage::interrupt_grace) {
                    if (!adapter.request_stop(StopRequest::terminate)) {
                        outcome.infrastructure_failures.push_back(
                            InfrastructureFailure::stop_request);
                        break;
                    }
                    stop_stage = StopStage::terminate_grace;
                    continue;
                }

                if (event.kind == LifecycleEventKind::deadline_expired
                    && stop_stage == StopStage::terminate_grace) {
                    if (!adapter.request_stop(StopRequest::kill)) {
                        outcome.infrastructure_failures.push_back(
                            InfrastructureFailure::stop_request);
                        break;
                    }
                    stop_stage = StopStage::kill_requested;
                    continue;
                }

                if (event.kind == LifecycleEventKind::workload_exited) {
                    outcome.workload = WorkloadResult{
                        WorkloadResultKind::exited, event.value};
                    state = State::waiting_for_cleanup;
                    continue;
                }

                if (event.kind == LifecycleEventKind::workload_signaled) {
                    outcome.workload = WorkloadResult{
                        WorkloadResultKind::signaled, event.value};
                    state = State::waiting_for_cleanup;
                    continue;
                }
            } else if (event.kind == LifecycleEventKind::cleanup_succeeded) {
                break;
            }

            outcome.infrastructure_failures.push_back(InfrastructureFailure::invalid_event);
            break;
        }
    }

    if (!adapter.reap_launcher()) {
        outcome.infrastructure_failures.push_back(
            InfrastructureFailure::launcher_reaping);
    }
    if (!adapter.finalize(outcome.infrastructure_succeeded())) {
        outcome.infrastructure_failures.push_back(
            InfrastructureFailure::supervisor_cleanup);
    }

    return outcome;
}

} // namespace netlaglab
