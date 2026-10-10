#include "helper_session.hpp"

#include <ostream>
#include <string_view>
#include <variant>

namespace netlaglab {
namespace {

using network_environment::Cause;
using network_environment::Failure;
using network_environment::Stage;

[[nodiscard]] std::string_view stage_name(const Stage stage)
{
    switch (stage) {
    case Stage::preflight: return "preflight";
    case Stage::namespace_creation: return "namespace creation";
    case Stage::veth_creation: return "veth creation";
    case Stage::host_configuration: return "host configuration";
    case Stage::namespace_configuration: return "namespace configuration";
    case Stage::route_configuration: return "route configuration";
    case Stage::cleanup: return "cleanup";
    }
    return "unknown";
}

[[nodiscard]] std::string_view cause_name(const Cause cause)
{
    switch (cause) {
    case Cause::collision: return "collision";
    case Cause::unavailable_or_invalid_tool: return "unavailable or invalid tool";
    case Cause::system_failure: return "system failure";
    case Cause::command_exit: return "command exited unsuccessfully";
    case Cause::command_signal: return "command terminated by signal";
    case Cause::timeout: return "timed out";
    case Cause::identity_unavailable: return "identity unavailable";
    case Cause::identity_mismatch: return "identity mismatch";
    case Cause::incomplete_cleanup: return "incomplete cleanup";
    }
    return "unknown";
}

void report_failure(
    const Failure failure,
    const std::string_view context,
    std::ostream& error)
{
    error << "NetLagLab helper: Network Environment " << context << " at "
          << stage_name(failure.stage) << ": " << cause_name(failure.cause) << '\n';
}

[[nodiscard]] bool report_cleanup(
    const bool succeeded,
    HelperSessionOperations& operations)
{
    return operations.send(
        succeeded ? HelperConversationEvent{CleanupSucceededEvent{}}
                  : HelperConversationEvent{CleanupFailedEvent{}});
}

[[nodiscard]] bool cleanup_prepared(
    network_environment::PreparedNetworkEnvironment& environment,
    std::ostream& error)
{
    using namespace network_environment;
    CleanupResult result{std::move(environment).cleanup()};
    bool succeeded{result.failures.empty() && !result.residual.has_value()};
    for (const Failure failure : result.failures) {
        report_failure(failure, "cleanup failed", error);
    }
    if (result.residual.has_value()) {
        CleanupResult retry{std::move(*result.residual).retry()};
        succeeded = false;
        for (const Failure failure : retry.failures) {
            report_failure(failure, "cleanup retry failed", error);
        }
        if (retry.residual.has_value()) {
            report_failure(
                {Stage::cleanup, Cause::incomplete_cleanup},
                "cleanup left residual resources",
                error);
        }
    }
    return succeeded;
}

[[nodiscard]] bool cleanup_failed_preparation(
    network_environment::PreparationFailure& failure,
    std::ostream& error)
{
    using namespace network_environment;
    report_failure(failure.primary, "preparation failed", error);
    bool succeeded{failure.rollback_failures.empty()};
    for (const Failure rollback_failure : failure.rollback_failures) {
        succeeded = false;
        report_failure(rollback_failure, "rollback failed", error);
    }
    if (failure.residual.has_value()) {
        CleanupResult retry{std::move(*failure.residual).retry()};
        succeeded = false;
        for (const Failure retry_failure : retry.failures) {
            report_failure(retry_failure, "rollback retry failed", error);
        }
        if (retry.residual.has_value()) {
            report_failure(
                {Stage::cleanup, Cause::incomplete_cleanup},
                "rollback left residual resources",
                error);
        }
    }
    return succeeded;
}

} // namespace

int run_helper_session(
    HelperSessionOperations& operations,
    const WorkloadContext& context,
    const WorkloadIdentity& identity,
    const WorkloadStandardDescriptors& standard_descriptors,
    std::ostream& error)
{
    using namespace network_environment;
    PreparationResult preparation{operations.prepare()};
    auto* prepared{std::get_if<PreparedNetworkEnvironment>(&preparation)};
    if (prepared == nullptr) {
        auto& failure{std::get<PreparationFailure>(preparation)};
        const bool cleanup_succeeded{cleanup_failed_preparation(failure, error)};
        const bool failure_reported{operations.send(ActivationFailedEvent{125})};
        const bool cleanup_reported{report_cleanup(cleanup_succeeded, operations)};
        return failure_reported && cleanup_reported && cleanup_succeeded ? 0 : 125;
    }

    const WorkloadNamespaceEntry namespace_entry{prepared->workload_namespace()};
    WorkloadLaunchResult launch{operations.launch(
        context, identity, standard_descriptors, namespace_entry)};
    if (!launch.process.has_value()) {
        if (!launch.diagnostic.empty()) {
            error << launch.diagnostic << '\n';
        }
        const bool failure_reported{
            operations.send(ActivationFailedEvent{launch.failure_exit_code})};
        const bool cleanup_succeeded{cleanup_prepared(*prepared, error)};
        const bool cleanup_reported{report_cleanup(cleanup_succeeded, operations)};
        return failure_reported && cleanup_reported && cleanup_succeeded ? 0 : 125;
    }

    int supervision_status{125};
    if (!operations.send(ActivatedEvent{launch.process->pid()})) {
        (void)operations.stop_and_reap(*launch.process);
        launch.process.reset();
    } else {
        supervision_status = operations.supervise(std::move(*launch.process));
    }

    const bool cleanup_succeeded{cleanup_prepared(*prepared, error)};
    const bool cleanup_reported{report_cleanup(cleanup_succeeded, operations)};
    return cleanup_succeeded && cleanup_reported ? supervision_status : 125;
}

} // namespace netlaglab
