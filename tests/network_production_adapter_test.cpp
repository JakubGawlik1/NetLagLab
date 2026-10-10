#include "network_environment/production_adapter.hpp"
#include "network_environment/production_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab::network_environment {
namespace {

using detail::AddressQuery;
using detail::HostLock;
using detail::HostLockResult;
using detail::InventoryStatus;
using detail::LinkQuery;
using detail::NamespaceHandle;
using detail::NamespaceIdentity;
using detail::NamespaceQuery;
using detail::PreflightPlatform;
using detail::ProductionPlatform;
using detail::QueryStatus;
using detail::RouteDumpStatus;
using detail::ToolQuery;

struct RecoveryOrderTrace {
    bool host_lock_held{};
    bool preflight_called{};
    bool inspected_under_lock{};
    std::size_t removals{};
};

class FakeHostLock final : public HostLock {
public:
    explicit FakeHostLock(std::shared_ptr<RecoveryOrderTrace> trace = {})
        : trace_{std::move(trace)}
    {
    }

    ~FakeHostLock() override
    {
        if (trace_) {
            trace_->host_lock_held = false;
        }
    }

private:
    std::shared_ptr<RecoveryOrderTrace> trace_;
};

class PassingPreflight final : public PreflightPlatform {
public:
    explicit PassingPreflight(std::shared_ptr<RecoveryOrderTrace> trace = {})
        : trace_{std::move(trace)}
    {
    }

    [[nodiscard]] HostLockResult acquire_host_lock() override
    {
        if (trace_) {
            trace_->host_lock_held = true;
        }
        return {std::make_unique<FakeHostLock>(trace_)};
    }
    [[nodiscard]] bool privileged() const override
    {
        if (trace_) {
            trace_->preflight_called = true;
        }
        return true;
    }
    [[nodiscard]] ToolQuery query_tool(std::string_view) override
    {
        return {QueryStatus::present, {true, 0, 0755}};
    }
    [[nodiscard]] QueryStatus query_namespace_name() override
    {
        return QueryStatus::absent;
    }
    [[nodiscard]] QueryStatus query_link_name(std::string_view) override
    {
        return QueryStatus::absent;
    }
    [[nodiscard]] AddressQuery query_addresses() override { return {true, {}}; }
    [[nodiscard]] RouteDumpStatus query_routes(
        std::chrono::steady_clock::time_point) override
    {
        return RouteDumpStatus::complete;
    }

private:
    std::shared_ptr<RecoveryOrderTrace> trace_;
};

class RecoveryOrderBackend final : public detail::PersistentFirewallBackend {
public:
    explicit RecoveryOrderBackend(std::shared_ptr<RecoveryOrderTrace> trace)
        : trace_{std::move(trace)}
    {
    }

    [[nodiscard]] detail::LiveRuleState inspect(std::string_view) override
    {
        trace_->inspected_under_lock = trace_->host_lock_held;
        return detail::LiveRuleState::mismatch;
    }

    [[nodiscard]] bool remove_exact(std::string_view) override
    {
        ++trace_->removals;
        return true;
    }

private:
    std::shared_ptr<RecoveryOrderTrace> trace_;
};

class FakeNamespaceHandle final : public NamespaceHandle {
public:
    explicit FakeNamespaceHandle(
        const NamespaceIdentity identity,
        std::shared_ptr<std::vector<std::string>> events = {},
        std::string label = {})
        : identity_{identity}
        , events_{std::move(events)}
        , label_{std::move(label)}
    {
    }

    ~FakeNamespaceHandle() override
    {
        if (events_) {
            events_->push_back("close:" + label_);
        }
    }

    [[nodiscard]] NamespaceIdentity identity() const override
    {
        return identity_;
    }

private:
    NamespaceIdentity identity_;
    std::shared_ptr<std::vector<std::string>> events_;
    std::string label_;
};

struct RecordedCommand {
    std::string executable;
    std::vector<std::string> arguments;
    const NamespaceHandle* inherited_namespace;
    bool enters_namespace;
};

struct RecordedLinkQuery {
    std::string name;
    const NamespaceHandle* namespace_handle;
};

struct FakeProductionTrace {
    std::vector<RecordedCommand> recorded_commands;
    std::vector<RecordedLinkQuery> recorded_link_queries;
    std::shared_ptr<std::vector<std::string>> events{
        std::make_shared<std::vector<std::string>>()};
};

class FakeProductionPlatform final : public ProductionPlatform {
public:
    std::deque<CommandResult> commands;
    std::deque<NamespaceQuery> namespaces;
    std::deque<LinkQuery> host_links;
    std::deque<LinkQuery> namespace_links;
    std::shared_ptr<FakeProductionTrace> trace{
        std::make_shared<FakeProductionTrace>()};

    [[nodiscard]] CommandResult run_ip(
        const std::string_view executable_path,
        const std::span<const std::string> arguments,
        const NamespaceHandle* inherited_namespace,
        std::chrono::steady_clock::time_point) override
    {
        trace->recorded_commands.push_back({
            std::string{executable_path},
            {arguments.begin(), arguments.end()},
            inherited_namespace,
            false,
        });
        CommandResult result{std::move(commands.front())};
        commands.pop_front();
        return result;
    }

    [[nodiscard]] CommandResult run_ip_in_namespace(
        const std::string_view executable_path,
        const std::span<const std::string> arguments,
        const NamespaceHandle& namespace_handle,
        std::chrono::steady_clock::time_point) override
    {
        trace->recorded_commands.push_back({
            std::string{executable_path},
            {arguments.begin(), arguments.end()},
            &namespace_handle,
            true,
        });
        CommandResult result{std::move(commands.front())};
        commands.pop_front();
        return result;
    }

    [[nodiscard]] NamespaceQuery query_namespace() override
    {
        if (namespaces.empty()) {
            return {InventoryStatus::failure, nullptr};
        }
        NamespaceQuery result{std::move(namespaces.front())};
        namespaces.pop_front();
        return result;
    }

    [[nodiscard]] std::string namespace_file_argument(
        const NamespaceHandle&) const override
    {
        return "/proc/self/fd/42";
    }

    [[nodiscard]] LinkQuery query_host_link(
        const std::string_view name,
        std::chrono::steady_clock::time_point) override
    {
        trace->events->push_back("host-query:" + std::string{name});
        trace->recorded_link_queries.push_back({std::string{name}, nullptr});
        if (host_links.empty()) {
            return {InventoryStatus::failure, std::nullopt};
        }
        LinkQuery result{std::move(host_links.front())};
        host_links.pop_front();
        return result;
    }

    [[nodiscard]] LinkQuery query_namespace_link(
        const NamespaceHandle& handle,
        const std::string_view name,
        std::chrono::steady_clock::time_point) override
    {
        trace->recorded_link_queries.push_back({std::string{name}, &handle});
        if (namespace_links.empty()) {
            return {InventoryStatus::failure, std::nullopt};
        }
        LinkQuery result{std::move(namespace_links.front())};
        namespace_links.pop_front();
        return result;
    }
};

LinkQuery present_link(
    const std::uint32_t index,
    const std::uint32_t peer_index,
    const std::optional<std::int32_t> peer_namespace_id = std::nullopt)
{
    return {
        InventoryStatus::present,
        detail::LinkIdentity{index, peer_index, peer_namespace_id},
    };
}

LinkQuery absent_link()
{
    return {InventoryStatus::absent, std::nullopt};
}

void queue_namespace_creation(FakeProductionPlatform& platform)
{
    platform.namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
}

void queue_namespace_cleanup(FakeProductionPlatform& platform)
{
    queue_namespace_creation(platform);
    platform.namespaces.push_back({InventoryStatus::absent, nullptr});
}

void queue_host_pair(FakeProductionPlatform& platform)
{
    platform.host_links.push_back(present_link(20, 21));
    platform.host_links.push_back(present_link(21, 20));
}

void queue_placed_pair(FakeProductionPlatform& platform)
{
    platform.host_links.push_back(present_link(20, 21, 4));
    platform.host_links.push_back(absent_link());
    platform.namespace_links.push_back(present_link(21, 20, 8));
}

void queue_absent_placed_pair(FakeProductionPlatform& platform)
{
    platform.host_links.push_back(absent_link());
    platform.host_links.push_back(absent_link());
    platform.namespace_links.push_back(absent_link());
}

TEST(NetworkProductionAdapterTest, ConfiguresTheCompleteFixedTopology)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    for (int index{}; index < 11; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparedNetworkEnvironment>(result));
    CleanupResult cleanup{
        std::move(std::get<PreparedNetworkEnvironment>(result)).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 11U);
    EXPECT_EQ(
        observed->recorded_commands[3].arguments,
        (std::vector<std::string>{
            "address", "add", "10.200.0.1/30", "dev", "nll-host"}));
    EXPECT_EQ(
        observed->recorded_commands[4].arguments,
        (std::vector<std::string>{"link", "set", "dev", "nll-host", "up"}));
    EXPECT_EQ(
        observed->recorded_commands[5].arguments,
        (std::vector<std::string>{"link", "set", "dev", "lo", "up"}));
    EXPECT_EQ(
        observed->recorded_commands[6].arguments,
        (std::vector<std::string>{
            "address", "add", "10.200.0.2/30", "dev", "nll-app"}));
    EXPECT_EQ(
        observed->recorded_commands[7].arguments,
        (std::vector<std::string>{"link", "set", "dev", "nll-app", "up"}));
    EXPECT_EQ(
        observed->recorded_commands[8].arguments,
        (std::vector<std::string>{
            "route", "add", "default", "via", "10.200.0.1", "dev", "nll-app"}));
    EXPECT_FALSE(observed->recorded_commands[3].enters_namespace);
    EXPECT_FALSE(observed->recorded_commands[4].enters_namespace);
    const NamespaceHandle* exact_namespace{
        observed->recorded_commands[2].inherited_namespace};
    ASSERT_NE(exact_namespace, nullptr);
    for (std::size_t index{5}; index <= 8; ++index) {
        EXPECT_TRUE(observed->recorded_commands[index].enters_namespace);
        EXPECT_EQ(
            observed->recorded_commands[index].inherited_namespace,
            exact_namespace);
    }
}

struct ConfigurationFailureCase {
    std::size_t operation_index;
    Stage expected_stage;
    CommandResultKind result_kind;
    Cause expected_cause;
};

class NetworkProductionConfigurationFailureTest
    : public ::testing::TestWithParam<ConfigurationFailureCase> {
};

TEST_P(
    NetworkProductionConfigurationFailureTest,
    StopsBeforeTheNextMutationAndRollsBackTheProvenRoots)
{
    const ConfigurationFailureCase parameter{GetParam()};
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    for (std::size_t index{}; index < 3 + parameter.operation_index; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    production->commands.push_back({parameter.result_kind, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, parameter.expected_stage);
    EXPECT_EQ(failure.primary.cause, parameter.expected_cause);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(
        observed->recorded_commands.size(),
        3 + parameter.operation_index + 1 + 2);
    EXPECT_EQ(
        observed->recorded_commands[3 + parameter.operation_index + 1].arguments,
        (std::vector<std::string>{"link", "delete", "dev", "nll-host"}));
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

INSTANTIATE_TEST_SUITE_P(
    EveryFixedConfigurationMutation,
    NetworkProductionConfigurationFailureTest,
    ::testing::Values(
        ConfigurationFailureCase{
            0,
            Stage::host_configuration,
            CommandResultKind::exec_failure,
            Cause::unavailable_or_invalid_tool,
        },
        ConfigurationFailureCase{
            1,
            Stage::host_configuration,
            CommandResultKind::signal,
            Cause::command_signal,
        },
        ConfigurationFailureCase{
            2,
            Stage::namespace_configuration,
            CommandResultKind::timeout,
            Cause::timeout,
        },
        ConfigurationFailureCase{
            3,
            Stage::namespace_configuration,
            CommandResultKind::system_failure,
            Cause::system_failure,
        },
        ConfigurationFailureCase{
            4,
            Stage::namespace_configuration,
            CommandResultKind::nonzero_exit,
            Cause::command_exit,
        },
        ConfigurationFailureCase{
            5,
            Stage::route_configuration,
            CommandResultKind::timeout,
            Cause::timeout,
        }));

TEST(NetworkProductionAdapterTest, CreatesNamespaceAndRetainsItsExactIdentity)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::veth_creation);
    ASSERT_EQ(observed->recorded_commands.size(), 2U);
    EXPECT_EQ(observed->recorded_commands[0].executable, "/usr/sbin/ip");
    EXPECT_EQ(
        observed->recorded_commands[0].arguments,
        (std::vector<std::string>{"netns", "add", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, MovesPeerThroughTheExactOwnedNamespaceHandle)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.stage,
        Stage::host_configuration);
    ASSERT_EQ(observed->recorded_commands.size(), 6U);
    EXPECT_EQ(
        observed->recorded_commands[1].arguments,
        (std::vector<std::string>{
            "link", "add", "nll-host", "type", "veth", "peer", "name", "nll-app"}));
    EXPECT_EQ(
        observed->recorded_commands[2].arguments,
        (std::vector<std::string>{
            "link", "set", "dev", "nll-app", "netns", "/proc/self/fd/42"}));
    ASSERT_NE(observed->recorded_commands[2].inherited_namespace, nullptr);
    ASSERT_GE(observed->recorded_link_queries.size(), 5U);
    EXPECT_EQ(
        observed->recorded_commands[2].inherited_namespace,
        observed->recorded_link_queries[4].namespace_handle);
}

TEST(NetworkProductionAdapterTest, ReconcilesAmbiguousNamespaceCreationBeforeCleanup)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({InventoryStatus::absent, nullptr});

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::namespace_creation);
    EXPECT_EQ(failure.primary.cause, Cause::timeout);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 2U);
    EXPECT_EQ(
        observed->recorded_commands[1].arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, LeavesUnconfirmedNamespaceUntouched)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->namespaces.push_back({InventoryStatus::failure, nullptr});

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.cause, Cause::identity_unavailable);
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::incomplete_cleanup);
    EXPECT_TRUE(failure.residual.has_value());
    EXPECT_EQ(observed->recorded_commands.size(), 1U);
}

TEST(NetworkProductionAdapterTest, DoesNotDeleteNamespaceWhoseIdentityChanged)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 12}),
    });

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::identity_mismatch);
    EXPECT_TRUE(failure.residual.has_value());
    EXPECT_EQ(observed->recorded_commands.size(), 1U);
}

TEST(NetworkProductionAdapterTest, LeavesNamespaceUntouchedWhenCleanupQueryFails)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    queue_namespace_creation(*production);
    production->namespaces.push_back({InventoryStatus::failure, nullptr});

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::identity_unavailable);
    EXPECT_TRUE(failure.residual.has_value());
    EXPECT_EQ(observed->recorded_commands.size(), 1U);
}

TEST(NetworkProductionAdapterTest, RetainsNamespaceAfterAmbiguousDelete)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_creation(*production);
    queue_namespace_creation(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::timeout);
    EXPECT_TRUE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 2U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, TreatsNamespaceAlreadyAbsentAsClean)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    queue_namespace_creation(*production);
    production->namespaces.push_back({InventoryStatus::absent, nullptr});

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    EXPECT_EQ(observed->recorded_commands.size(), 1U);
}

TEST(NetworkProductionAdapterTest, RollsBackPlacedVethBeforeNamespace)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    for (int index{}; index < 3; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::host_configuration);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 6U);
    EXPECT_EQ(
        observed->recorded_commands[4].arguments,
        (std::vector<std::string>{"link", "delete", "dev", "nll-host"}));
    EXPECT_EQ(
        observed->recorded_commands[5].arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, ReconcilesAmbiguousPeerMoveToPlacedPair)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::signal, SIGKILL, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::veth_creation);
    EXPECT_EQ(failure.primary.cause, Cause::command_signal);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 5U);
    EXPECT_EQ(
        observed->recorded_commands[3].arguments,
        (std::vector<std::string>{"link", "delete", "dev", "nll-host"}));
}

TEST(NetworkProductionAdapterTest, ReconcilesFailedPeerMoveToOldHostPair)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_host_pair(*production);
    production->namespace_links.push_back(absent_link());
    queue_host_pair(*production);
    production->host_links.push_back(absent_link());
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::veth_creation);
    EXPECT_EQ(failure.primary.cause, Cause::command_exit);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 5U);
    EXPECT_EQ(
        observed->recorded_commands[3].arguments,
        (std::vector<std::string>{"link", "delete", "dev", "nll-host"}));
}

TEST(NetworkProductionAdapterTest, ReconcilesFailedPeerMoveToProvenAbsence)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_absent_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.cause, Cause::timeout);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 4U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, RetainsUnconfirmedPeerMoveWithoutVethDeletion)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    production->host_links.push_back(
        {InventoryStatus::failure, std::nullopt});
    production->host_links.push_back(absent_link());
    production->namespace_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.cause, Cause::identity_unavailable);
    ASSERT_TRUE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 4U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, DoesNotDeleteVethWhoseIdentityChanged)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    for (int index{}; index < 3; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({InventoryStatus::absent, nullptr});
    production->host_links.push_back(present_link(20, 21));
    production->host_links.push_back(present_link(21, 20));
    production->host_links.push_back(present_link(20, 21, 4));
    production->host_links.push_back(absent_link());
    production->namespace_links.push_back(present_link(21, 20, 8));
    production->host_links.push_back(present_link(30, 31, 4));
    production->host_links.push_back(absent_link());
    production->namespace_links.push_back(present_link(31, 30, 8));

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::identity_mismatch);
    ASSERT_TRUE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 5U);
    EXPECT_EQ(
        observed->recorded_commands[4].arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, RechecksRetainedVethAfterNamespaceRemoval)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->commands.push_back({CommandResultKind::nonzero_exit, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(
            NamespaceIdentity{7, 11}, observed->events, "owned"),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({InventoryStatus::absent, nullptr});

    production->host_links.push_back(present_link(20, 21));
    production->host_links.push_back(present_link(21, 20));
    production->host_links.push_back(present_link(20, 21, 4));
    production->host_links.push_back(absent_link());
    production->namespace_links.push_back(present_link(21, 20, 8));
    for (int index{}; index < 2; ++index) {
        production->host_links.push_back(present_link(20, 21, 4));
        production->host_links.push_back(absent_link());
        production->namespace_links.push_back(present_link(21, 20, 8));
    }
    production->host_links.push_back(absent_link());
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::command_exit);
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 6U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
    const auto closed{std::find(
        observed->events->begin(), observed->events->end(), "close:owned")};
    ASSERT_NE(closed, observed->events->end());
    const auto final_recheck{std::find(
        closed, observed->events->end(), "host-query:nll-host")};
    EXPECT_NE(final_recheck, observed->events->end());
}

TEST(NetworkProductionAdapterTest, ReconcilesAmbiguousVethCreationToOwnedPair)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({InventoryStatus::absent, nullptr});
    production->host_links.push_back(present_link(20, 21));
    production->host_links.push_back(present_link(21, 20));
    production->host_links.push_back(present_link(20, 21));
    production->host_links.push_back(present_link(21, 20));
    production->host_links.push_back(absent_link());
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::veth_creation);
    EXPECT_EQ(failure.primary.cause, Cause::timeout);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 4U);
    EXPECT_EQ(
        observed->recorded_commands[2].arguments,
        (std::vector<std::string>{"link", "delete", "dev", "nll-host"}));
}

TEST(NetworkProductionAdapterTest, TreatsOwnedVethAlreadyAbsentAsClean)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    production->host_links.push_back(absent_link());
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 3U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, LeavesVethUntouchedWhenCleanupQueryFails)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::timeout, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    production->host_links.push_back(
        {InventoryStatus::failure, std::nullopt});
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    ASSERT_EQ(failure.rollback_failures.size(), 1U);
    EXPECT_EQ(failure.rollback_failures[0].cause, Cause::identity_unavailable);
    EXPECT_TRUE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 3U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

TEST(NetworkProductionAdapterTest, NeverAdoptsPartialVethIdentity)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({InventoryStatus::absent, nullptr});
    production->host_links.push_back(present_link(20, 21));
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.cause, Cause::identity_unavailable);
    ASSERT_TRUE(failure.residual.has_value());
    ASSERT_EQ(observed->recorded_commands.size(), 3U);
    EXPECT_EQ(
        observed->recorded_commands.back().arguments,
        (std::vector<std::string>{"netns", "delete", "netlaglab"}));
}

struct CommandMappingCase {
    CommandResultKind kind;
    Cause expected;
};

class NetworkProductionCommandMappingTest
    : public ::testing::TestWithParam<CommandMappingCase> {
};

TEST_P(NetworkProductionCommandMappingTest, MapsRunnerResultAfterProvenAbsence)
{
    const CommandMappingCase parameter{GetParam()};
    auto production{std::make_unique<FakeProductionPlatform>()};
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->commands.push_back({parameter.kind, 1, {}});
    production->commands.push_back({CommandResultKind::success, 0, {}});
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({
        InventoryStatus::present,
        std::make_unique<FakeNamespaceHandle>(NamespaceIdentity{7, 11}),
    });
    production->namespaces.push_back({InventoryStatus::absent, nullptr});
    production->host_links.push_back(absent_link());
    production->host_links.push_back(absent_link());

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::veth_creation);
    EXPECT_EQ(failure.primary.cause, parameter.expected);
    EXPECT_FALSE(failure.residual.has_value());
}

INSTANTIATE_TEST_SUITE_P(
    RunnerResults,
    NetworkProductionCommandMappingTest,
    ::testing::Values(
        CommandMappingCase{CommandResultKind::success, Cause::identity_unavailable},
        CommandMappingCase{CommandResultKind::nonzero_exit, Cause::command_exit},
        CommandMappingCase{CommandResultKind::signal, Cause::command_signal},
        CommandMappingCase{CommandResultKind::timeout, Cause::timeout},
        CommandMappingCase{
            CommandResultKind::exec_failure,
            Cause::unavailable_or_invalid_tool},
        CommandMappingCase{CommandResultKind::system_failure, Cause::system_failure}));

TEST(NetworkProductionRecoveryTest, ReconcilesUnderHostLockBeforeNetworkPreflight)
{
    std::array<char, 40> directory_template{};
    constexpr std::string_view pattern{"/tmp/nll-recovery-order-XXXXXX"};
    std::copy(pattern.begin(), pattern.end(), directory_template.begin());
    char* created{mkdtemp(directory_template.data())};
    ASSERT_NE(created, nullptr);
    const std::string directory{created};
    const auto remove_directory = [&]() {
        std::filesystem::remove_all(directory);
    };
    auto journal{std::make_unique<detail::RecoveryJournalStore>(
        directory, static_cast<std::uint32_t>(getuid()))};
    ASSERT_TRUE(journal->write(
        {detail::RecoveryPhase::intent, "00112233445566778899aabbccddeeff"}));
    auto trace{std::make_shared<RecoveryOrderTrace>()};

    PreparationResult result{testing::prepare_with_recovery(
        std::make_unique<PassingPreflight>(trace),
        std::make_unique<FakeProductionPlatform>(),
        std::move(journal),
        std::make_unique<RecoveryOrderBackend>(trace))};

    remove_directory();
    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::identity_mismatch);
    EXPECT_TRUE(trace->inspected_under_lock);
    EXPECT_FALSE(trace->preflight_called);
    EXPECT_EQ(trace->removals, 0U);
    EXPECT_FALSE(trace->host_lock_held);
}

} // namespace
} // namespace netlaglab::network_environment
