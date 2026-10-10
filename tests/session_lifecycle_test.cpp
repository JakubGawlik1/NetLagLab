#include "session_lifecycle.hpp"
#include "controller_session_outcome.hpp"
#include "session_presentation.hpp"
#include "socket_io.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace netlaglab {
namespace {

class ScriptedLifecycleAdapter final : public LifecycleAdapter {
public:
    ScriptedLifecycleAdapter(
        const std::initializer_list<LifecycleEvent> events,
        const bool launcher_reaping_succeeds = true,
        const bool supervisor_cleanup_succeeds = true,
        const bool terminal_delivery_succeeds = true)
        : events_{events},
          launcher_reaping_succeeds_{launcher_reaping_succeeds},
          supervisor_cleanup_succeeds_{supervisor_cleanup_succeeds},
          terminal_delivery_succeeds_{terminal_delivery_succeeds}
    {
    }

    bool begin() override
    {
        began = true;
        return true;
    }

    LifecycleEvent wait(const LifecycleWait wait) override
    {
        waits.push_back(wait);
        if (events_.empty()) {
            return {LifecycleEventKind::conversation_lost};
        }

        const LifecycleEvent event{events_.front()};
        events_.pop_front();
        return event;
    }

    bool request_stop(const StopRequest request) override
    {
        stop_requests.push_back(request);
        return true;
    }

    bool reap_launcher() override
    {
        launcher_reaped = true;
        return launcher_reaping_succeeds_;
    }

    bool finalize() override
    {
        finalization_order.push_back("cleanup");
        finalized = true;
        return supervisor_cleanup_succeeds_;
    }

    void publish_outcome(const SessionOutcome& outcome) override
    {
        finalization_order.push_back("publish");
        published_outcome = outcome;
        const ControllerSessionOutcomeSerialization serialized{
            serialize_controller_session_outcome(outcome)};
        if (const auto* block{std::get_if<std::string>(&serialized)}) {
            published_block = *block;
            terminal_delivery_succeeded = terminal_delivery_succeeds_
                || send_socket_text(-1, *block);
        } else {
            terminal_delivery_succeeded = false;
        }
    }

    bool began{false};
    bool launcher_reaped{false};
    bool finalized{false};
    bool terminal_delivery_succeeded{true};
    std::optional<SessionOutcome> published_outcome;
    std::string published_block;
    std::vector<std::string> finalization_order;
    std::vector<LifecycleWait> waits;
    std::vector<StopRequest> stop_requests;

private:
    std::deque<LifecycleEvent> events_;
    bool launcher_reaping_succeeds_;
    bool supervisor_cleanup_succeeds_;
    bool terminal_delivery_succeeds_;
};

TEST(SessionLifecycleTest, ReturnsWorkloadOutcomeAfterCleanupAndLauncherReaping)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 23},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->kind, WorkloadResultKind::exited);
    EXPECT_EQ(outcome.workload->value, 23);
    EXPECT_TRUE(outcome.infrastructure_succeeded());
    EXPECT_EQ(session_exit_status(outcome), 23);
    EXPECT_TRUE(adapter.began);
    EXPECT_TRUE(adapter.launcher_reaped);
}

TEST(SessionLifecycleTest, PreservesPreActivationStartFailureAfterCleanRollback)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activation_failed, 127},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->kind, WorkloadResultKind::start_failure);
    EXPECT_EQ(outcome.workload->value, 127);
    EXPECT_TRUE(outcome.infrastructure_succeeded());
    EXPECT_EQ(session_exit_status(outcome), 127);
    EXPECT_TRUE(adapter.launcher_reaped);
}

TEST(SessionLifecycleTest, DistinguishesPermissionAndInfrastructureStartFailures)
{
    ScriptedLifecycleAdapter permission_adapter{
        {{LifecycleEventKind::activation_failed, 126},
         {LifecycleEventKind::cleanup_succeeded}}};
    ScriptedLifecycleAdapter infrastructure_adapter{
        {{LifecycleEventKind::activation_failed, 125},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome permission{run_session_lifecycle(permission_adapter)};
    const SessionOutcome infrastructure{
        run_session_lifecycle(infrastructure_adapter)};

    ASSERT_TRUE(permission.workload.has_value());
    EXPECT_EQ(permission.workload->kind, WorkloadResultKind::start_failure);
    EXPECT_EQ(session_exit_status(permission), 126);
    EXPECT_FALSE(infrastructure.workload.has_value());
    EXPECT_FALSE(infrastructure.infrastructure_succeeded());
    EXPECT_EQ(session_exit_status(infrastructure), 125);
}

TEST(SessionLifecycleTest, ClassifiesImmediateExitAsActivatedWorkloadOutcome)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 0},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->kind, WorkloadResultKind::exited);
    EXPECT_EQ(session_exit_status(outcome), 0);
}

TEST(SessionLifecycleTest, CleanupFailureOverridesButPreservesWorkloadOutcome)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 7},
         {LifecycleEventKind::cleanup_failed}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->value, 7);
    EXPECT_FALSE(outcome.infrastructure_succeeded());
    EXPECT_EQ(session_exit_status(outcome), 125);
}

TEST(SessionLifecycleTest, LauncherReapingFailureOverridesButPreservesWorkloadOutcome)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 9},
         {LifecycleEventKind::cleanup_succeeded}},
        false};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->value, 9);
    EXPECT_EQ(session_exit_status(outcome), 125);
}

TEST(SessionLifecycleTest, PublishesOutcomeAfterSupervisorCleanupIsKnown)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 9},
         {LifecycleEventKind::cleanup_succeeded}},
        true,
        false};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_EQ(
        adapter.finalization_order,
        (std::vector<std::string>{"cleanup", "publish"}));
    ASSERT_TRUE(adapter.published_outcome.has_value());
    EXPECT_EQ(
        adapter.published_outcome->infrastructure_failures,
        (std::vector{InfrastructureFailure::supervisor_cleanup}));
    EXPECT_EQ(
        outcome.infrastructure_failures,
        adapter.published_outcome->infrastructure_failures);
    EXPECT_NE(
        adapter.published_block.find("FAILURE SUPERVISOR_CLEANUP\n"),
        std::string::npos);
}

TEST(SessionLifecycleTest, TerminalDeliveryFailureDoesNotChangeSessionOutcome)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 0},
         {LifecycleEventKind::cleanup_succeeded}},
        true,
        true,
        false};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_TRUE(outcome.infrastructure_succeeded());
    EXPECT_FALSE(adapter.terminal_delivery_succeeded);
    ASSERT_TRUE(adapter.published_outcome.has_value());
    EXPECT_TRUE(adapter.published_outcome->infrastructure_succeeded());
    EXPECT_NE(
        adapter.published_block.find("INFRASTRUCTURE OK\n"),
        std::string::npos);
}

TEST(SessionLifecycleTest, HelperLossIsInfrastructureFailure)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::conversation_lost}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_FALSE(outcome.infrastructure_succeeded());
    EXPECT_EQ(session_exit_status(outcome), 125);
    EXPECT_TRUE(adapter.launcher_reaped);
}

TEST(SessionLifecycleTest, HelperLossPreservesKnownWorkloadOutcome)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 12},
         {LifecycleEventKind::conversation_lost}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->value, 12);
    EXPECT_FALSE(outcome.infrastructure_succeeded());
    EXPECT_EQ(session_exit_status(outcome), 125);
}

TEST(SessionLifecycleTest, ControllerLossDoesNotStopSession)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::controller_lost},
         {LifecycleEventKind::workload_exited, 4},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_EQ(session_exit_status(outcome), 4);
    EXPECT_TRUE(adapter.stop_requests.empty());
}

TEST(SessionLifecycleTest, FirstInterruptUsesGracePeriodsBeforeTermAndKill)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::terminal_interrupt},
         {LifecycleEventKind::deadline_expired},
         {LifecycleEventKind::deadline_expired},
         {LifecycleEventKind::workload_signaled, 9},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_EQ(adapter.stop_requests.size(), 2U);
    EXPECT_EQ(adapter.stop_requests[0], StopRequest::terminate);
    EXPECT_EQ(adapter.stop_requests[1], StopRequest::kill);
    EXPECT_EQ(session_exit_status(outcome), 137);
}

TEST(SessionLifecycleTest, SecondInterruptRequestsImmediateKill)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::terminal_interrupt},
         {LifecycleEventKind::terminal_interrupt},
         {LifecycleEventKind::workload_signaled, 9},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_EQ(adapter.stop_requests.size(), 1U);
    EXPECT_EQ(adapter.stop_requests.front(), StopRequest::kill);
    EXPECT_EQ(session_exit_status(outcome), 137);
}

TEST(SessionLifecycleTest, TerminalEventCancelsQueuedControllerAndSignalEvents)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::workload_exited, 6},
         {LifecycleEventKind::stop_requested},
         {LifecycleEventKind::terminal_interrupt},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->value, 6);
    EXPECT_TRUE(outcome.infrastructure_succeeded());
    EXPECT_TRUE(adapter.stop_requests.empty());
}

TEST(SessionLifecycleTest, WorkloadResultEndsAnActiveStopDeadline)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::terminal_interrupt},
         {LifecycleEventKind::workload_signaled, 2},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    ASSERT_TRUE(outcome.infrastructure_succeeded());
    ASSERT_GE(adapter.waits.size(), 4U);
    EXPECT_EQ(adapter.waits.back(), LifecycleWait::indefinitely);
}

TEST(SessionLifecycleTest, ProfileStateFailureStopsAndPreservesLaterOutcomes)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::profile_state_unknown},
         {LifecycleEventKind::workload_exited, 17},
         {LifecycleEventKind::cleanup_failed}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_EQ(adapter.stop_requests, (std::vector{StopRequest::terminate}));
    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->value, 17);
    EXPECT_EQ(
        outcome.infrastructure_failures,
        (std::vector{
            InfrastructureFailure::profile_state,
            InfrastructureFailure::cleanup}));
}

TEST(SessionLifecycleTest, ConnectivityFailureStopsWorkloadAndKeepsBothOutcomes)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::connectivity_failed},
         {LifecycleEventKind::workload_signaled, 15},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_EQ(adapter.stop_requests, (std::vector{StopRequest::terminate}));
    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->kind, WorkloadResultKind::signaled);
    EXPECT_EQ(outcome.workload->value, 15);
    EXPECT_EQ(
        outcome.infrastructure_failures,
        (std::vector{InfrastructureFailure::connectivity}));
}

TEST(SessionLifecycleTest, ProfileStateFailurePreservesExistingStopDeadline)
{
    ScriptedLifecycleAdapter adapter{
        {{LifecycleEventKind::activated, 4321},
         {LifecycleEventKind::stop_requested},
         {LifecycleEventKind::profile_state_unknown},
         {LifecycleEventKind::deadline_expired},
         {LifecycleEventKind::workload_signaled, 9},
         {LifecycleEventKind::cleanup_succeeded}}};

    const SessionOutcome outcome{run_session_lifecycle(adapter)};

    EXPECT_EQ(
        adapter.stop_requests,
        (std::vector{StopRequest::terminate, StopRequest::kill}));
    ASSERT_TRUE(outcome.workload.has_value());
    EXPECT_EQ(outcome.workload->value, 9);
    EXPECT_EQ(
        outcome.infrastructure_failures,
        (std::vector{InfrastructureFailure::profile_state}));
}

TEST(SessionPresentationTest, ReportsEveryInfrastructureFailureAndWorkloadResult)
{
    const SessionOutcome outcome{
        .workload = WorkloadResult{WorkloadResultKind::exited, 7},
        .infrastructure_failures = {
            InfrastructureFailure::conversation,
            InfrastructureFailure::connectivity,
            InfrastructureFailure::profile_state,
            InfrastructureFailure::cleanup,
        },
    };
    std::ostringstream error;

    report_session_outcome(outcome, error);

    EXPECT_EQ(session_exit_status(outcome), 125);
    EXPECT_NE(error.str().find("helper conversation"), std::string::npos);
    EXPECT_NE(error.str().find("Session connectivity"), std::string::npos);
    EXPECT_NE(error.str().find("unknown network profile state"), std::string::npos);
    EXPECT_NE(error.str().find("privileged cleanup"), std::string::npos);
    EXPECT_NE(error.str().find("Workload result was 7"), std::string::npos);
}

} // namespace
} // namespace netlaglab
