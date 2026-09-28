#pragma once

#include <optional>
#include <vector>

namespace netlaglab {

enum class WorkloadResultKind {
    start_failure,
    exited,
    signaled,
};

struct WorkloadResult {
    WorkloadResultKind kind;
    int value;
};

enum class InfrastructureFailure {
    start,
    conversation,
    stop_request,
    cleanup,
    launcher_reaping,
    supervisor_cleanup,
    invalid_event,
};

struct SessionOutcome {
    std::optional<WorkloadResult> workload;
    std::vector<InfrastructureFailure> infrastructure_failures;

    [[nodiscard]] bool infrastructure_succeeded() const noexcept;
};

enum class LifecycleEventKind {
    activated,
    activation_failed,
    workload_exited,
    workload_signaled,
    cleanup_succeeded,
    cleanup_failed,
    conversation_lost,
    controller_lost,
    terminal_interrupt,
    stop_requested,
    deadline_expired,
};

struct LifecycleEvent {
    LifecycleEventKind kind;
    int value{};
};

enum class LifecycleWait {
    indefinitely,
    five_seconds,
};

enum class StopRequest {
    terminate,
    kill,
};

class LifecycleAdapter {
public:
    virtual ~LifecycleAdapter() = default;

    [[nodiscard]] virtual bool begin() = 0;
    [[nodiscard]] virtual LifecycleEvent wait(LifecycleWait wait) = 0;
    [[nodiscard]] virtual bool request_stop(StopRequest request) = 0;
    [[nodiscard]] virtual bool reap_launcher() = 0;
    [[nodiscard]] virtual bool finalize(bool session_succeeded) = 0;
};

[[nodiscard]] SessionOutcome run_session_lifecycle(LifecycleAdapter& adapter);

} // namespace netlaglab
