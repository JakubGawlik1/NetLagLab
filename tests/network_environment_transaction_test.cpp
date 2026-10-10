#include "network_environment/network_environment.hpp"
#include "network_environment/transaction_test_support.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace netlaglab::network_environment {
namespace {

using testing::Operation;
using testing::Outcome;
using testing::ScriptStep;

const std::vector<Operation> setup_operations{
    Operation::preflight,
    Operation::create_namespace,
    Operation::create_veth,
    Operation::move_peer,
    Operation::assign_host_address,
    Operation::bring_host_link_up,
    Operation::bring_loopback_up,
    Operation::assign_namespace_address,
    Operation::bring_namespace_link_up,
    Operation::add_default_route,
    Operation::configure_nat,
};

std::vector<ScriptStep> successful_setup()
{
    std::vector<ScriptStep> script;
    for (const Operation operation : setup_operations) {
        script.push_back({operation});
    }
    script.push_back({Operation::remove_nat, Outcome::removed});
    return script;
}

TEST(NetworkEnvironmentTransactionTest, PreparesAndExplicitlyCleansCompleteEnvironment)
{
    static_assert(!std::is_copy_constructible_v<PreparedNetworkEnvironment>);
    static_assert(std::is_move_constructible_v<PreparedNetworkEnvironment>);
    static_assert(!std::is_move_assignable_v<PreparedNetworkEnvironment>);
    static_assert(!std::is_move_assignable_v<ResidualCleanup>);

    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        {
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
            {Operation::configure_nat},
            {Operation::remove_nat, Outcome::removed},
            {Operation::remove_veth, Outcome::removed},
            {Operation::remove_namespace, Outcome::removed},
        },
        trace)};

    ASSERT_TRUE(std::holds_alternative<PreparedNetworkEnvironment>(result));
    CleanupResult cleanup{
        std::move(std::get<PreparedNetworkEnvironment>(result)).cleanup()};

    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_EQ(
        trace->operations,
        (std::vector<Operation>{
            Operation::preflight,
            Operation::create_namespace,
            Operation::create_veth,
            Operation::move_peer,
            Operation::assign_host_address,
            Operation::bring_host_link_up,
            Operation::bring_loopback_up,
            Operation::assign_namespace_address,
            Operation::bring_namespace_link_up,
            Operation::add_default_route,
            Operation::configure_nat,
            Operation::remove_nat,
            Operation::remove_veth,
            Operation::remove_namespace,
        }));
}

TEST(NetworkEnvironmentTransactionTest, PreparedOwnerRetainsHostLockUntilCleanup)
{
    auto trace{std::make_shared<testing::SharedTrace>()};
    auto script{successful_setup()};
    script.push_back({Operation::remove_veth, Outcome::removed});
    script.push_back({Operation::remove_namespace, Outcome::removed});
    PreparationResult result{testing::prepare_scripted_network_environment(
        std::move(script), trace, {}, true)};
    ASSERT_TRUE(std::holds_alternative<PreparedNetworkEnvironment>(result));
    EXPECT_TRUE(trace->host_lock_alive);

    CleanupResult cleanup{std::move(
        std::get<PreparedNetworkEnvironment>(result))
                              .cleanup()};

    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_FALSE(trace->host_lock_alive);
}

TEST(NetworkEnvironmentTransactionTest, ResidualOwnerRetainsHostLockUntilRetry)
{
    auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        {
            {Operation::preflight},
            {Operation::create_namespace, Outcome::fail_new_state},
            {Operation::remove_namespace, Outcome::cleanup_retained},
            {Operation::remove_namespace, Outcome::removed},
        },
        trace,
        {},
        true)};
    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    auto& failure{std::get<PreparationFailure>(result)};
    ASSERT_TRUE(failure.residual.has_value());
    EXPECT_TRUE(trace->host_lock_alive);

    CleanupResult retry{std::move(*failure.residual).retry()};

    EXPECT_TRUE(retry.failures.empty());
    EXPECT_FALSE(retry.residual.has_value());
    EXPECT_FALSE(trace->host_lock_alive);
}

struct SetupFailureCase {
    Operation failed_operation;
    Stage expected_stage;
    std::vector<Operation> rollback;
};

class NetworkEnvironmentSetupFailureTest
    : public ::testing::TestWithParam<SetupFailureCase> {
};

TEST_P(NetworkEnvironmentSetupFailureTest, StopsAndRollsBackOnlyTheProvenPrefix)
{
    const SetupFailureCase parameter{GetParam()};
    std::vector<ScriptStep> script;
    std::vector<Operation> expected_trace;
    for (const Operation operation : setup_operations) {
        expected_trace.push_back(operation);
        if (operation == parameter.failed_operation) {
            script.push_back({
                operation,
                Outcome::fail_unchanged,
                Cause::command_signal,
            });
            break;
        }
        script.push_back({operation});
    }
    for (const Operation operation : parameter.rollback) {
        script.push_back({operation, Outcome::removed});
        expected_trace.push_back(operation);
    }

    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        std::move(script),
        trace)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, parameter.expected_stage);
    EXPECT_EQ(failure.primary.cause, Cause::command_signal);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    EXPECT_EQ(trace->operations, expected_trace);
}

INSTANTIATE_TEST_SUITE_P(
    EverySemanticOperation,
    NetworkEnvironmentSetupFailureTest,
    ::testing::Values(
        SetupFailureCase{Operation::preflight, Stage::preflight, {}},
        SetupFailureCase{
            Operation::create_namespace,
            Stage::namespace_creation,
            {},
        },
        SetupFailureCase{
            Operation::create_veth,
            Stage::veth_creation,
            {Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::move_peer,
            Stage::veth_creation,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::assign_host_address,
            Stage::host_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::bring_host_link_up,
            Stage::host_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::bring_loopback_up,
            Stage::namespace_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::assign_namespace_address,
            Stage::namespace_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::bring_namespace_link_up,
            Stage::namespace_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::add_default_route,
            Stage::route_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        },
        SetupFailureCase{
            Operation::configure_nat,
            Stage::nat_configuration,
            {Operation::remove_veth, Operation::remove_namespace},
        }));

TEST(
    NetworkEnvironmentTransactionTest,
    PreservesRollbackFailureAndRechecksVethAfterNamespaceRemoval)
{
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        {
            {Operation::preflight},
            {Operation::create_namespace},
            {Operation::create_veth},
            {Operation::move_peer},
            {Operation::assign_host_address,
             Outcome::fail_unchanged,
             Cause::command_exit},
            {Operation::remove_veth,
             Outcome::cleanup_retained,
             Cause::system_failure},
            {Operation::remove_namespace, Outcome::removed},
            {Operation::remove_veth, Outcome::already_absent},
        },
        trace)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::host_configuration);
    EXPECT_EQ(failure.primary.cause, Cause::command_exit);
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures.front().stage, Stage::cleanup);
    EXPECT_EQ(failure.rollback_failures.front().cause, Cause::system_failure);
    EXPECT_FALSE(failure.residual.has_value());
    EXPECT_EQ(trace->operations.back(), Operation::remove_veth);
}

struct OwnershipTransitionCase {
    Operation failed_operation;
    Outcome outcome;
    std::vector<Operation> rollback;
    bool expects_residual;
};

class NetworkEnvironmentOwnershipTransitionTest
    : public ::testing::TestWithParam<OwnershipTransitionCase> {
};

TEST_P(
    NetworkEnvironmentOwnershipTransitionTest,
    CleanupAuthorityFollowsOnlyTheReconciledEvidence)
{
    const OwnershipTransitionCase parameter{GetParam()};
    std::vector<ScriptStep> script;
    std::vector<Operation> expected_trace;
    for (const Operation operation : setup_operations) {
        expected_trace.push_back(operation);
        if (operation == parameter.failed_operation) {
            script.push_back({
                operation,
                parameter.outcome,
                Cause::identity_unavailable,
            });
            break;
        }
        script.push_back({operation});
    }
    for (const Operation operation : parameter.rollback) {
        script.push_back({operation, Outcome::removed});
        expected_trace.push_back(operation);
    }

    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        std::move(script),
        trace)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.cause, Cause::identity_unavailable);
    EXPECT_EQ(failure.residual.has_value(), parameter.expects_residual);
    EXPECT_EQ(trace->operations, expected_trace);
}

INSTANTIATE_TEST_SUITE_P(
    NamespaceVethAndPeerMove,
    NetworkEnvironmentOwnershipTransitionTest,
    ::testing::Values(
        OwnershipTransitionCase{
            Operation::create_namespace,
            Outcome::fail_unchanged,
            {},
            false,
        },
        OwnershipTransitionCase{
            Operation::create_namespace,
            Outcome::fail_new_state,
            {Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::create_namespace,
            Outcome::fail_absent,
            {},
            false,
        },
        OwnershipTransitionCase{
            Operation::create_namespace,
            Outcome::fail_identity_unconfirmed,
            {},
            true,
        },
        OwnershipTransitionCase{
            Operation::create_veth,
            Outcome::fail_unchanged,
            {Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::create_veth,
            Outcome::fail_new_state,
            {Operation::remove_veth, Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::create_veth,
            Outcome::fail_absent,
            {Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::create_veth,
            Outcome::fail_identity_unconfirmed,
            {Operation::remove_namespace},
            true,
        },
        OwnershipTransitionCase{
            Operation::move_peer,
            Outcome::fail_unchanged,
            {Operation::remove_veth, Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::move_peer,
            Outcome::fail_new_state,
            {Operation::remove_veth, Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::move_peer,
            Outcome::fail_absent,
            {Operation::remove_namespace},
            false,
        },
        OwnershipTransitionCase{
            Operation::move_peer,
            Outcome::fail_identity_unconfirmed,
            {Operation::remove_namespace},
            true,
        }));

TEST(
    NetworkEnvironmentTransactionTest,
    ExplicitRetryTouchesOnlyResourcesRetainedByThePreviousPass)
{
    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth,
             Outcome::cleanup_retained,
             Cause::command_exit},
            {Operation::remove_namespace,
             Outcome::cleanup_retained,
             Cause::system_failure},
            {Operation::remove_veth, Outcome::removed},
            {Operation::remove_namespace, Outcome::already_absent},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult preparation{
        testing::prepare_scripted_network_environment(std::move(script), trace)};
    ASSERT_TRUE(
        std::holds_alternative<PreparedNetworkEnvironment>(preparation));

    CleanupResult first{
        std::move(std::get<PreparedNetworkEnvironment>(preparation)).cleanup()};

    ASSERT_EQ(first.failures.size(), 2U);
    EXPECT_EQ(first.failures[0].cause, Cause::command_exit);
    EXPECT_EQ(first.failures[1].cause, Cause::system_failure);
    ASSERT_TRUE(first.residual.has_value());

    CleanupResult retry{std::move(*first.residual).retry()};

    EXPECT_TRUE(retry.failures.empty());
    EXPECT_FALSE(retry.residual.has_value());
    EXPECT_EQ(
        std::vector<Operation>(trace->operations.end() - 4, trace->operations.end()),
        (std::vector<Operation>{
            Operation::remove_veth,
            Operation::remove_namespace,
            Operation::remove_veth,
            Operation::remove_namespace,
        }));
}

TEST(
    NetworkEnvironmentTransactionTest,
    IdentityUnconfirmedResidualNeverRegainsDeletionAuthority)
{
    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth,
             Outcome::cleanup_identity_mismatch,
             Cause::identity_mismatch},
            {Operation::remove_namespace, Outcome::removed},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult preparation{
        testing::prepare_scripted_network_environment(std::move(script), trace)};
    ASSERT_TRUE(
        std::holds_alternative<PreparedNetworkEnvironment>(preparation));

    CleanupResult first{
        std::move(std::get<PreparedNetworkEnvironment>(preparation)).cleanup()};
    ASSERT_TRUE(first.residual.has_value());
    const std::size_t calls_before_retry{trace->operations.size()};

    CleanupResult retry{std::move(*first.residual).retry()};

    ASSERT_EQ(retry.failures.size(), 1U);
    EXPECT_EQ(retry.failures.front().cause, Cause::incomplete_cleanup);
    EXPECT_TRUE(retry.residual.has_value());
    EXPECT_EQ(trace->operations.size(), calls_before_retry);
}

TEST(
    NetworkEnvironmentTransactionTest,
    AlreadyAbsentRootsCompleteExplicitCleanup)
{
    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth, Outcome::already_absent},
            {Operation::remove_namespace, Outcome::already_absent},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult preparation{
        testing::prepare_scripted_network_environment(std::move(script), trace)};
    ASSERT_TRUE(
        std::holds_alternative<PreparedNetworkEnvironment>(preparation));

    CleanupResult cleanup{
        std::move(std::get<PreparedNetworkEnvironment>(preparation)).cleanup()};

    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
}

TEST(
    NetworkEnvironmentTransactionTest,
    NamespaceIdentityMismatchIsRetainedWithoutAnotherDeletionAttempt)
{
    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth, Outcome::removed},
            {Operation::remove_namespace,
             Outcome::cleanup_identity_mismatch,
             Cause::identity_mismatch},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult preparation{
        testing::prepare_scripted_network_environment(std::move(script), trace)};
    ASSERT_TRUE(
        std::holds_alternative<PreparedNetworkEnvironment>(preparation));

    CleanupResult cleanup{
        std::move(std::get<PreparedNetworkEnvironment>(preparation)).cleanup()};
    ASSERT_EQ(cleanup.failures.size(), 1U);
    EXPECT_EQ(cleanup.failures.front().cause, Cause::identity_mismatch);
    ASSERT_TRUE(cleanup.residual.has_value());
    const std::size_t calls_before_retry{trace->operations.size()};

    CleanupResult retry{std::move(*cleanup.residual).retry()};

    ASSERT_EQ(retry.failures.size(), 1U);
    EXPECT_EQ(retry.failures.front().cause, Cause::incomplete_cleanup);
    EXPECT_TRUE(retry.residual.has_value());
    EXPECT_EQ(trace->operations.size(), calls_before_retry);
}

TEST(
    NetworkEnvironmentTransactionTest,
    SetupBudgetDoesNotRenewAndRollbackReceivesAnIndependentBudget)
{
    const auto start{
        std::chrono::steady_clock::time_point{std::chrono::seconds{100}}};
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        {
            {Operation::preflight},
            {Operation::create_namespace,
             Outcome::success,
             Cause::system_failure,
             std::chrono::seconds{30}},
            {Operation::remove_namespace, Outcome::removed},
        },
        trace,
        start)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::veth_creation);
    EXPECT_EQ(failure.primary.cause, Cause::timeout);
    EXPECT_EQ(
        trace->operations,
        (std::vector<Operation>{
            Operation::preflight,
            Operation::create_namespace,
            Operation::remove_namespace,
        }));
    ASSERT_EQ(trace->deadlines.size(), 3U);
    EXPECT_EQ(trace->deadlines[0], start + std::chrono::seconds{5});
    EXPECT_EQ(trace->deadlines[1], start + std::chrono::seconds{5});
    EXPECT_EQ(trace->deadlines[2], start + std::chrono::seconds{35});
}

TEST(
    NetworkEnvironmentTransactionTest,
    OperationDeadlineIsCappedByTheRemainingSetupBudget)
{
    const auto start{
        std::chrono::steady_clock::time_point{std::chrono::seconds{100}}};
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        {
            {Operation::preflight,
             Outcome::success,
             Cause::system_failure,
             std::chrono::seconds{27}},
            {Operation::create_namespace,
             Outcome::fail_unchanged,
             Cause::system_failure},
        },
        trace,
        start)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    ASSERT_EQ(trace->deadlines.size(), 2U);
    EXPECT_EQ(trace->deadlines[0], start + std::chrono::seconds{5});
    EXPECT_EQ(trace->deadlines[1], start + std::chrono::seconds{30});
}

TEST(
    NetworkEnvironmentTransactionTest,
    CleanupExpiryStartsNoLaterMutationAndExplicitRetryGetsAFreshBudget)
{
    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth,
             Outcome::cleanup_retained,
             Cause::command_exit,
             std::chrono::seconds{30}},
            {Operation::remove_veth, Outcome::removed},
            {Operation::remove_namespace, Outcome::removed},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult preparation{
        testing::prepare_scripted_network_environment(std::move(script), trace)};
    ASSERT_TRUE(
        std::holds_alternative<PreparedNetworkEnvironment>(preparation));

    CleanupResult first{
        std::move(std::get<PreparedNetworkEnvironment>(preparation)).cleanup()};

    ASSERT_EQ(first.failures.size(), 2U);
    EXPECT_EQ(first.failures[0].cause, Cause::command_exit);
    EXPECT_EQ(first.failures[1].cause, Cause::timeout);
    ASSERT_TRUE(first.residual.has_value());
    EXPECT_EQ(trace->operations.back(), Operation::remove_veth);
    const auto first_cleanup_deadline{trace->deadlines.back()};

    CleanupResult retry{std::move(*first.residual).retry()};

    EXPECT_TRUE(retry.failures.empty());
    EXPECT_FALSE(retry.residual.has_value());
    ASSERT_GE(trace->deadlines.size(), 3U);
    EXPECT_EQ(
        trace->deadlines[trace->deadlines.size() - 2],
        first_cleanup_deadline + std::chrono::seconds{30});
}

TEST(
    NetworkEnvironmentTransactionTest,
    DestructionPerformsOneNoThrowBestEffortPass)
{
    static_assert(std::is_nothrow_destructible_v<PreparedNetworkEnvironment>);
    static_assert(std::is_nothrow_destructible_v<ResidualCleanup>);

    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth, Outcome::removed},
            {Operation::remove_namespace, Outcome::removed},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    {
        PreparationResult preparation{
            testing::prepare_scripted_network_environment(
                std::move(script),
                trace)};
        ASSERT_TRUE(
            std::holds_alternative<PreparedNetworkEnvironment>(preparation));
    }

    EXPECT_EQ(
        std::vector<Operation>(trace->operations.end() - 2, trace->operations.end()),
        (std::vector<Operation>{
            Operation::remove_veth,
            Operation::remove_namespace,
        }));
}

TEST(
    NetworkEnvironmentTransactionTest,
    ResidualDestructionMakesOnlyOneAdditionalPass)
{
    std::vector<ScriptStep> script{successful_setup()};
    script.insert(
        script.end(),
        {
            {Operation::remove_veth,
             Outcome::cleanup_retained,
             Cause::command_exit},
            {Operation::remove_namespace,
             Outcome::cleanup_retained,
             Cause::system_failure},
            {Operation::remove_veth, Outcome::removed},
            {Operation::remove_namespace, Outcome::removed},
        });
    const auto trace{std::make_shared<testing::SharedTrace>()};
    {
        PreparationResult preparation{
            testing::prepare_scripted_network_environment(
                std::move(script),
                trace)};
        ASSERT_TRUE(
            std::holds_alternative<PreparedNetworkEnvironment>(preparation));
        CleanupResult cleanup{
            std::move(std::get<PreparedNetworkEnvironment>(preparation))
                .cleanup()};
        ASSERT_TRUE(cleanup.residual.has_value());
    }

    EXPECT_EQ(
        std::vector<Operation>(trace->operations.end() - 4, trace->operations.end()),
        (std::vector<Operation>{
            Operation::remove_veth,
            Operation::remove_namespace,
            Operation::remove_veth,
            Operation::remove_namespace,
        }));
}

class NetworkEnvironmentCauseTest : public ::testing::TestWithParam<Cause> {
};

TEST_P(NetworkEnvironmentCauseTest, PreservesTheTypedCause)
{
    const Cause cause{GetParam()};
    const auto trace{std::make_shared<testing::SharedTrace>()};
    PreparationResult result{testing::prepare_scripted_network_environment(
        {
            {Operation::preflight, Outcome::fail_unchanged, cause},
        },
        trace)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(std::get<PreparationFailure>(result).primary.cause, cause);
}

INSTANTIATE_TEST_SUITE_P(
    StableCauses,
    NetworkEnvironmentCauseTest,
    ::testing::Values(
        Cause::collision,
        Cause::unavailable_or_invalid_tool,
        Cause::unsupported_host_configuration,
        Cause::system_failure,
        Cause::command_exit,
        Cause::command_signal,
        Cause::timeout,
        Cause::identity_unavailable,
        Cause::identity_mismatch,
        Cause::incomplete_cleanup));

} // namespace
} // namespace netlaglab::network_environment
