#include "session_lifecycle.hpp"
#include "session_presentation.hpp"

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
        const bool launcher_reaping_succeeds = true)
        : events_{events}, launcher_reaping_succeeds_{launcher_reaping_succeeds}
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

    bool finalize(const bool session_succeeded) override
    {
        finalized = true;
        final_session_success = session_succeeded;
        return true;
    }

    bool began{false};
    bool launcher_reaped{false};
    bool finalized{false};
    bool final_session_success{false};
    std::vector<LifecycleWait> waits;
    std::vector<StopRequest> stop_requests;

private:
    std::deque<LifecycleEvent> events_;
    bool launcher_reaping_succeeds_;
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

TEST(SessionPresentationTest, ReportsEveryInfrastructureFailureAndWorkloadResult)
{
    const SessionOutcome outcome{
        .workload = WorkloadResult{WorkloadResultKind::exited, 7},
        .infrastructure_failures = {
            InfrastructureFailure::conversation,
            InfrastructureFailure::cleanup,
        },
    };
    std::ostringstream error;

    report_session_outcome(outcome, error);

    EXPECT_EQ(session_exit_status(outcome), 125);
    EXPECT_NE(error.str().find("helper conversation"), std::string::npos);
    EXPECT_NE(error.str().find("privileged cleanup"), std::string::npos);
    EXPECT_NE(error.str().find("Workload result was 7"), std::string::npos);
}

} // namespace
} // namespace netlaglab
