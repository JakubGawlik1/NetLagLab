#pragma once

#include "helper_protocol.hpp"
#include "network_environment/network_environment.hpp"
#include "workload_process.hpp"

#include <iosfwd>

namespace netlaglab {

class HelperSessionOperations {
public:
    virtual ~HelperSessionOperations() = default;

    [[nodiscard]] virtual network_environment::PreparationResult prepare() = 0;
    [[nodiscard]] virtual WorkloadLaunchResult launch(
        const WorkloadContext& context,
        const WorkloadIdentity& identity,
        const WorkloadStandardDescriptors& standard_descriptors,
        const network_environment::WorkloadNamespaceEntry& namespace_entry) = 0;
    [[nodiscard]] virtual bool send(const HelperConversationEvent& event) = 0;
    [[nodiscard]] virtual int supervise(WorkloadProcess workload) = 0;
    [[nodiscard]] virtual bool stop_and_reap(WorkloadProcess& workload) = 0;
};

[[nodiscard]] int run_helper_session(
    HelperSessionOperations& operations,
    const WorkloadContext& context,
    const WorkloadIdentity& identity,
    const WorkloadStandardDescriptors& standard_descriptors,
    std::ostream& error);

} // namespace netlaglab
