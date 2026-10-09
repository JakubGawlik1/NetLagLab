#pragma once

#include "workload_process.hpp"

namespace netlaglab::workload_process_testing {

struct ChildSetupHooks {
    bool (*enter_namespace)(void*) noexcept;
    void (*before_identity_drop)(void*) noexcept;
    void* context;
};

[[nodiscard]] WorkloadLaunchResult launch_with_child_setup_hooks(
    const WorkloadContext& context,
    const WorkloadIdentity& identity,
    const WorkloadStandardDescriptors& standard_descriptors,
    const ChildSetupHooks& hooks);

} // namespace netlaglab::workload_process_testing
