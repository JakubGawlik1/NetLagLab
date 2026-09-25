#pragma once

#include "workload_context.hpp"

#include <array>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace netlaglab {

struct WorkloadIdentity {
    uid_t uid;
    gid_t gid;
    std::vector<gid_t> supplementary_groups;
};

inline constexpr int inherit_standard_descriptor{-1};
inline constexpr int close_standard_descriptor{-2};

struct WorkloadStandardDescriptors {
    std::array<int, 3> sources{
        inherit_standard_descriptor,
        inherit_standard_descriptor,
        inherit_standard_descriptor,
    };
};

struct WorkloadStatus {
    bool exited;
    int value;
};

enum class WorkloadPollState {
    running,
    finished,
    error,
};

struct WorkloadPollResult {
    WorkloadPollState state;
    std::optional<WorkloadStatus> status;
};

class WorkloadProcess {
public:
    explicit WorkloadProcess(pid_t pid) noexcept;
    ~WorkloadProcess();

    WorkloadProcess(const WorkloadProcess&) = delete;
    WorkloadProcess& operator=(const WorkloadProcess&) = delete;
    WorkloadProcess(WorkloadProcess&& other) noexcept;
    WorkloadProcess& operator=(WorkloadProcess&& other) noexcept;

    [[nodiscard]] pid_t pid() const noexcept;
    [[nodiscard]] bool send_signal(int signal_number) const noexcept;
    [[nodiscard]] std::optional<WorkloadStatus> wait();
    [[nodiscard]] WorkloadPollResult poll();

private:
    void terminate_and_reap() noexcept;

    pid_t pid_{-1};
};

struct WorkloadLaunchResult {
    std::optional<WorkloadProcess> process;
    int failure_exit_code{};
};

[[nodiscard]] WorkloadLaunchResult launch_workload(
    const WorkloadContext& context,
    const WorkloadIdentity& identity,
    const WorkloadStandardDescriptors& standard_descriptors = {});

} // namespace netlaglab
