#include "helper_session.hpp"

#include "network_environment/transaction_test_support.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <grp.h>
#include <unistd.h>

namespace netlaglab {
namespace {

using namespace network_environment;
using namespace network_environment::testing;

std::vector<ScriptStep> successful_preparation()
{
    return {
        {Operation::preflight},
        {Operation::create_namespace},
        {Operation::create_veth},
        {Operation::move_peer},
        {Operation::assign_host_address},
        {Operation::bring_host_link_up},
        {Operation::bring_loopback_up},
        {Operation::assign_namespace_address},
        {Operation::bring_namespace_link_up},
        {Operation::add_default_route},
        {Operation::remove_veth, Outcome::removed},
        {Operation::remove_namespace, Outcome::removed},
    };
}

WorkloadIdentity current_identity()
{
    const uid_t uid{geteuid()};
    const int count{getgroups(0, nullptr)};
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (count > 0) {
        (void)getgroups(count, groups.data());
    }
    return {uid, getegid(), std::move(groups)};
}

class ScriptedHelperSession final : public HelperSessionOperations {
public:
    ScriptedHelperSession(
        std::vector<ScriptStep> script,
        std::shared_ptr<SharedTrace> trace)
        : script_{std::move(script)}
        , trace_{std::move(trace)}
    {
    }

    PreparationResult prepare() override
    {
        actions.push_back("prepare");
        return prepare_scripted_network_environment(script_, trace_, {}, true);
    }

    WorkloadLaunchResult launch(
        const WorkloadContext& context,
        const WorkloadIdentity& identity,
        const WorkloadStandardDescriptors& standard_descriptors,
        const WorkloadNamespaceEntry& namespace_entry) override
    {
        actions.push_back("launch");
        EXPECT_TRUE(trace_->host_lock_alive);
        (void)namespace_entry;
        if (launch_failure.has_value()) {
            return {
                .process = std::nullopt,
                .failure_exit_code = *launch_failure,
                .diagnostic = launch_diagnostic,
            };
        }
        return launch_workload(context, identity, standard_descriptors);
    }

    bool send(const HelperConversationEvent& event) override
    {
        if (const auto* active{std::get_if<ActivatedEvent>(&event)}) {
            actions.push_back("active " + std::to_string(active->workload_pid));
            EXPECT_TRUE(trace_->host_lock_alive);
            return active_delivery_succeeds;
        }
        if (std::holds_alternative<ActivationFailedEvent>(event)) {
            actions.push_back("start failed");
            operations_at_start_failure = trace_->operations;
        } else if (std::holds_alternative<WorkloadExitedEvent>(event)) {
            actions.push_back("workload exited");
            EXPECT_TRUE(trace_->host_lock_alive);
        } else if (std::holds_alternative<CleanupSucceededEvent>(event)) {
            actions.push_back("cleanup succeeded");
            EXPECT_FALSE(trace_->host_lock_alive);
        } else if (std::holds_alternative<CleanupFailedEvent>(event)) {
            actions.push_back("cleanup failed");
            EXPECT_FALSE(trace_->host_lock_alive);
        }
        return true;
    }

    int supervise(WorkloadProcess workload) override
    {
        actions.push_back("supervise");
        if (simulate_supervisor_loss) {
            actions.push_back("supervisor lost");
            (void)stop_and_reap(workload);
            return 125;
        }
        const auto status{workload.wait()};
        EXPECT_TRUE(status.has_value());
        if (!status.has_value()) {
            return 125;
        }
        actions.push_back("reaped");
        lock_was_held_after_reap = trace_->host_lock_alive;
        if (status->exited) {
            workload_exit_codes.push_back(status->value);
            (void)send(WorkloadExitedEvent{status->value});
            return status->value;
        }
        (void)send(WorkloadSignaledEvent{status->value});
        return 128 + status->value;
    }

    bool stop_and_reap(WorkloadProcess& workload) override
    {
        actions.push_back("stop and reap");
        (void)workload.send_signal(SIGKILL);
        const bool reaped{workload.wait().has_value()};
        actions.push_back(reaped ? "reaped" : "reap failed");
        lock_was_held_after_reap = trace_->host_lock_alive;
        return reaped;
    }

    std::vector<ScriptStep> script_;
    std::shared_ptr<SharedTrace> trace_;
    std::vector<std::string> actions;
    std::optional<int> launch_failure;
    std::string launch_diagnostic;
    bool active_delivery_succeeds{true};
    bool simulate_supervisor_loss{};
    bool lock_was_held_after_reap{};
    std::vector<Operation> operations_at_start_failure;
    std::vector<int> workload_exit_codes;
};

WorkloadContext true_context()
{
    return {"/", {"/bin/true"}, {"PATH=/bin"}};
}

TEST(HelperSessionTest, PreparesLaunchesReapsAndCleansInOrder)
{
    auto trace{std::make_shared<SharedTrace>()};
    ScriptedHelperSession operations{successful_preparation(), trace};
    std::ostringstream error;

    const int result{run_helper_session(
        operations, true_context(), current_identity(), {}, error)};

    EXPECT_EQ(result, 0);
    ASSERT_EQ(operations.actions.size(), 7U);
    EXPECT_EQ(operations.actions[0], "prepare");
    EXPECT_EQ(operations.actions[1], "launch");
    EXPECT_TRUE(operations.actions[2].starts_with("active "));
    EXPECT_EQ(operations.actions[3], "supervise");
    EXPECT_EQ(operations.actions[4], "reaped");
    EXPECT_EQ(operations.actions[5], "workload exited");
    EXPECT_EQ(operations.actions[6], "cleanup succeeded");
    EXPECT_TRUE(operations.lock_was_held_after_reap);
    EXPECT_FALSE(trace->host_lock_alive);
    EXPECT_EQ(trace->operations[10], Operation::remove_veth);
    EXPECT_EQ(trace->operations[11], Operation::remove_namespace);
}

TEST(HelperSessionTest, PreparationFailureRetriesResidualWithoutLaunching)
{
    auto trace{std::make_shared<SharedTrace>()};
    std::vector<ScriptStep> script{
        {Operation::preflight},
        {Operation::create_namespace, Outcome::fail_new_state, Cause::system_failure},
        {Operation::remove_namespace, Outcome::cleanup_retained, Cause::command_exit},
        {Operation::remove_namespace, Outcome::removed},
    };
    ScriptedHelperSession operations{std::move(script), trace};
    std::ostringstream error;

    const int result{run_helper_session(
        operations, true_context(), current_identity(), {}, error)};

    EXPECT_EQ(result, 125);
    EXPECT_EQ(operations.actions,
              (std::vector<std::string>{"prepare", "start failed", "cleanup failed"}));
    EXPECT_EQ(trace->operations,
              (std::vector<Operation>{Operation::preflight,
                                      Operation::create_namespace,
                                      Operation::remove_namespace,
                                      Operation::remove_namespace}));
    EXPECT_EQ(operations.operations_at_start_failure, trace->operations);
    EXPECT_FALSE(trace->host_lock_alive);
    EXPECT_NE(error.str().find("namespace creation: system failure"), std::string::npos);
}

TEST(HelperSessionTest, ExecFailureCleansBeforeReportingAndPreservesConventionalCode)
{
    auto trace{std::make_shared<SharedTrace>()};
    ScriptedHelperSession operations{successful_preparation(), trace};
    operations.launch_failure = 127;
    std::ostringstream error;

    const int result{run_helper_session(
        operations, true_context(), current_identity(), {}, error)};

    EXPECT_EQ(result, 0);
    EXPECT_EQ(operations.actions,
              (std::vector<std::string>{"prepare", "launch", "start failed",
                                        "cleanup succeeded"}));
    EXPECT_FALSE(trace->host_lock_alive);
}

TEST(HelperSessionTest, DnsMountFailureReportsDiagnosticAndCleansBeforeActivation)
{
    auto trace{std::make_shared<SharedTrace>()};
    ScriptedHelperSession operations{successful_preparation(), trace};
    operations.launch_failure = 125;
    operations.launch_diagnostic =
        "NetLagLab helper: failed to install the private read-only DNS mount view.";
    std::ostringstream error;

    const int result{run_helper_session(
        operations, true_context(), current_identity(), {}, error)};

    EXPECT_EQ(result, 0);
    EXPECT_EQ(operations.actions,
              (std::vector<std::string>{"prepare", "launch", "start failed",
                                        "cleanup succeeded"}));
    EXPECT_EQ(operations.operations_at_start_failure.size(), 10U);
    EXPECT_EQ(error.str(), operations.launch_diagnostic + '\n');
    EXPECT_FALSE(trace->host_lock_alive);
}

TEST(HelperSessionTest, CleanupFailureKeepsWorkloadResultAndFailsSession)
{
    auto script{successful_preparation()};
    script[10] = {Operation::remove_veth, Outcome::cleanup_retained, Cause::command_exit};
    script.push_back({Operation::remove_veth, Outcome::removed});
    auto trace{std::make_shared<SharedTrace>()};
    ScriptedHelperSession operations{std::move(script), trace};
    std::ostringstream error;

    const int result{run_helper_session(
        operations, true_context(), current_identity(), {}, error)};

    EXPECT_EQ(result, 125);
    EXPECT_EQ(operations.actions[4], "reaped");
    EXPECT_EQ(operations.actions[5], "workload exited");
    EXPECT_EQ(operations.actions[6], "cleanup failed");
    EXPECT_EQ(operations.workload_exit_codes, (std::vector<int>{0}));
    EXPECT_TRUE(operations.lock_was_held_after_reap);
    EXPECT_FALSE(trace->host_lock_alive);
}

TEST(HelperSessionTest, ActivationDeliveryFailureStopsAndReapsBeforeCleanup)
{
    auto trace{std::make_shared<SharedTrace>()};
    ScriptedHelperSession operations{successful_preparation(), trace};
    operations.active_delivery_succeeds = false;
    const WorkloadContext context{"/", {"/bin/sleep", "5"}, {"PATH=/bin"}};
    std::ostringstream error;

    const int result{run_helper_session(
        operations, context, current_identity(), {}, error)};

    EXPECT_EQ(result, 125);
    EXPECT_EQ(operations.actions[2].substr(0, 6), "active");
    EXPECT_EQ(operations.actions[3], "stop and reap");
    EXPECT_EQ(operations.actions[4], "reaped");
    EXPECT_EQ(operations.actions[5], "cleanup succeeded");
    EXPECT_TRUE(operations.lock_was_held_after_reap);
    EXPECT_FALSE(trace->host_lock_alive);
}

TEST(HelperSessionTest, SupervisorLossStillReapsBeforeExplicitCleanup)
{
    auto trace{std::make_shared<SharedTrace>()};
    ScriptedHelperSession operations{successful_preparation(), trace};
    const WorkloadContext context{"/", {"/bin/sleep", "5"}, {"PATH=/bin"}};
    std::ostringstream error;
    operations.simulate_supervisor_loss = true;

    const int result{run_helper_session(
        operations, context, current_identity(), {}, error)};

    EXPECT_EQ(result, 125);
    EXPECT_EQ(operations.actions[3], "supervise");
    EXPECT_EQ(operations.actions[4], "supervisor lost");
    EXPECT_EQ(operations.actions[5], "stop and reap");
    EXPECT_EQ(operations.actions[6], "reaped");
    EXPECT_EQ(operations.actions[7], "cleanup succeeded");
    EXPECT_TRUE(operations.lock_was_held_after_reap);
    EXPECT_FALSE(trace->host_lock_alive);
}

} // namespace
} // namespace netlaglab
