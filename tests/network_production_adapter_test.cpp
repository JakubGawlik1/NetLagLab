#include "network_environment/production_adapter.hpp"
#include "network_environment/production_test_support.hpp"
#include "network_environment/recovery_journal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <deque>
#include <memory>
#include <optional>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

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

class FakeHostLock final : public HostLock {
};

class PassingPreflight final : public PreflightPlatform {
public:
    explicit PassingPreflight(const bool namespace_conflict = false)
        : namespace_conflict_{namespace_conflict}
    {
    }

    [[nodiscard]] HostLockResult acquire_host_lock() override
    {
        return {std::make_unique<FakeHostLock>()};
    }
    [[nodiscard]] bool privileged() const override { return true; }
    [[nodiscard]] ToolQuery query_tool(std::string_view) override
    {
        return {QueryStatus::present, {true, 0, 0755}};
    }
    [[nodiscard]] QueryStatus query_namespace_name() override
    {
        return namespace_conflict_ ? QueryStatus::present : QueryStatus::absent;
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
    bool namespace_conflict_;
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
    std::vector<RecordedCommand> recorded_tools;
    std::vector<RecordedLinkQuery> recorded_link_queries;
    std::vector<std::string> consent_requests;
    std::shared_ptr<std::vector<std::string>> events{
        std::make_shared<std::vector<std::string>>()};
};

class FakeProductionPlatform final : public ProductionPlatform {
public:
    FakeProductionPlatform()
    {
        std::array<char, 64> path{};
        constexpr std::string_view prefix{"/tmp/netlaglab-production-XXXXXX"};
        std::copy(prefix.begin(), prefix.end(), path.begin());
        const char* const directory{mkdtemp(path.data())};
        if (directory != nullptr) {
            recovery_directory_path_ = directory;
            recovery_directory_descriptor_ = open(
                directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }
    }

    ~FakeProductionPlatform() override
    {
        if (recovery_directory_descriptor_ != -1) {
            close(recovery_directory_descriptor_);
        }
        if (!recovery_directory_path_.empty()) {
            std::filesystem::remove_all(recovery_directory_path_);
        }
    }

    std::deque<CommandResult> commands;
    std::deque<CommandResult> tc_commands;
    std::deque<CommandResult> tool_commands;
    std::vector<RecordedCommand> recorded_tools;
    std::optional<bool> forwarding_enabled{true};
    std::optional<bool> legacy_rules{false};
    bool consent_granted{true};
    std::string ufw_status_output{"Status: inactive\n"};
    std::string ufw_rules_output;
    std::string firewalld_state_output{"not running\n"};
    std::optional<CommandResultKind> firewalld_state_kind;
    std::string firewalld_failure_argument;
    std::string ipv4_rules_output{
        "0:\tfrom all lookup local\n"
        "32766:\tfrom all lookup main\n"
        "32767:\tfrom all lookup default\n"};
    std::string ipv4_default_route_output{
        "default via 192.0.2.1 dev eth0 proto static\n"};
    std::string default_route_link_output{
        "2: eth0: <BROADCAST,UP>\n    link/ether 02:00:00:00:00:01\n"};
    std::string nft_ruleset_output;
    std::string firewalld_zone{"public"};
    bool firewalld_policy_exists{};
    bool firewalld_ingress_configured{};
    bool firewalld_egress_configured{};
    bool firewalld_rule_configured{};
    std::string nft_table_name;
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
        if (executable_path.ends_with("/tc")) {
            CommandResult result{std::move(tc_commands.front())};
            tc_commands.pop_front();
            return result;
        }
        CommandResult result{std::move(commands.front())};
        commands.pop_front();
        return result;
    }

    [[nodiscard]] CommandResult run_tool(
        const std::string_view executable_path,
        const std::span<const std::string> arguments,
        std::chrono::steady_clock::time_point) override
    {
        if (arguments.size() >= 2U && arguments[0] == "qdisc") {
            CommandResult result{std::move(tc_commands.front())};
            tc_commands.pop_front();
            trace->recorded_tools.push_back({
                std::string{executable_path},
                {arguments.begin(), arguments.end()},
                nullptr,
                false,
            });
            return result;
        }
        recorded_tools.push_back({
            std::string{executable_path},
            {arguments.begin(), arguments.end()},
            nullptr,
            false,
        });
        trace->recorded_tools.push_back(recorded_tools.back());
        if (!tool_commands.empty()) {
            CommandResult result{std::move(tool_commands.front())};
            tool_commands.pop_front();
            return result;
        }
        const std::string_view command{
            arguments.empty() ? std::string_view{} : std::string_view{arguments.front()}};
        if (arguments.size() == 3U && arguments[0] == "-4"
            && arguments[1] == "rule" && arguments[2] == "show") {
            return {CommandResultKind::success, 0, {}, ipv4_rules_output};
        }
        if (arguments.size() == 4U && arguments[0] == "-4"
            && arguments[1] == "route" && arguments[2] == "show"
            && arguments[3] == "default") {
            return {CommandResultKind::success, 0, {}, ipv4_default_route_output};
        }
        if (arguments.size() == 5U && arguments[0] == "-d"
            && arguments[1] == "link" && arguments[2] == "show"
            && arguments[3] == "dev") {
            return {CommandResultKind::success, 0, {}, default_route_link_output};
        }
        if (command == "status") {
            if (arguments.size() > 1U && arguments[1] == "numbered") {
                return {CommandResultKind::success, 0, {}, ufw_rules_output};
            }
            return {CommandResultKind::success, 0, {}, ufw_status_output};
        }
        if (command == "--state") {
            const bool running{firewalld_state_output == "running\n"};
            return {firewalld_state_kind.value_or(
                        running ? CommandResultKind::success
                                : CommandResultKind::nonzero_exit),
                    running ? 0 : 252, {}, firewalld_state_output};
        }
        if (command.starts_with("--get-zone-of-interface=")) {
            return {CommandResultKind::success, 0, {}, firewalld_zone + "\n"};
        }
        if (command.starts_with("--new-policy=")) {
            firewalld_policy_exists = true;
            firewalld_ingress_configured = false;
            firewalld_egress_configured = false;
            firewalld_rule_configured = false;
            return {CommandResultKind::success, 0, {}, {}};
        }
        if (arguments.size() == 2U && arguments[0].starts_with("--policy=")) {
            const std::string_view operation{arguments[1]};
            if (operation.starts_with("--add-ingress-zone=")) {
                if (operation == firewalld_failure_argument) {
                    return {CommandResultKind::nonzero_exit, 1, "COMMAND_FAILED", {}};
                }
                firewalld_ingress_configured = true;
                return {CommandResultKind::success, 0, {}, {}};
            }
            if (operation == "--add-egress-zone=ANY") {
                if (operation == firewalld_failure_argument) {
                    return {CommandResultKind::nonzero_exit, 1, "COMMAND_FAILED", {}};
                }
                firewalld_egress_configured = true;
                return {CommandResultKind::success, 0, {}, {}};
            }
            if (operation.starts_with("--add-rich-rule=")) {
                if (operation == firewalld_failure_argument) {
                    return {CommandResultKind::nonzero_exit, 1, "COMMAND_FAILED", {}};
                }
                firewalld_rule_configured = true;
                return {CommandResultKind::success, 0, {}, {}};
            }
        }
        if (command.starts_with("--info-policy=")) {
            if (!firewalld_policy_exists) {
                return {CommandResultKind::nonzero_exit, 2, "INVALID_POLICY", {}};
            }
            return {CommandResultKind::success, 0, {},
                    "ingress-zones: "
                        + (firewalld_ingress_configured ? firewalld_zone : "")
                        + "\negress-zones: "
                        + (firewalld_egress_configured ? "ANY" : "")
                        + "\nrich rules:\n"
                        + (firewalld_rule_configured
                               ? " rule family=\"ipv4\" source address=\"10.200.0.2/32\" accept\n"
                               : "")};
        }
        if (command.starts_with("--delete-policy=")) {
            firewalld_policy_exists = false;
            firewalld_ingress_configured = false;
            firewalld_egress_configured = false;
            firewalld_rule_configured = false;
            return {CommandResultKind::success, 0, {}, {}};
        }
        if (command == "list" && arguments.size() == 2U
            && arguments[1] == "ruleset") {
            return {CommandResultKind::success, 0, {}, nft_ruleset_output};
        }
        if (command == "list" && arguments.size() == 4U
            && arguments[1] == "table") {
            if (arguments[3] == nft_table_name) {
                return {CommandResultKind::success, 0, {},
                        "table ip " + arguments[3] + " {}\n"};
            }
            return {CommandResultKind::nonzero_exit, 1, "No such file", {}};
        }
        if (command == "add" && arguments.size() == 4U
            && arguments[1] == "table") {
            nft_table_name = arguments[3];
        }
        if (command == "delete" && arguments.size() == 4U
            && arguments[1] == "table") {
            nft_table_name.clear();
        }
        if (arguments.size() > 1U && arguments[0] == "route"
            && arguments[1] == "allow") {
            const std::string marker{arguments.back()};
            ufw_rules_output = "[ 1] 10.200.0.2 on nll-host ALLOW FWD # "
                               + marker + "\n";
        } else if (arguments.size() > 1U && arguments[0] == "route"
                   && arguments[1] == "delete") {
            ufw_rules_output.clear();
        }
        return {CommandResultKind::success, 0, {}, {}};
    }

    [[nodiscard]] std::optional<bool> ipv4_forwarding_enabled() override
    {
        return forwarding_enabled;
    }

    [[nodiscard]] std::optional<bool> legacy_iptables_rules_present() override
    {
        return legacy_rules;
    }

    [[nodiscard]] bool request_firewall_consent(
        const std::string_view rule,
        std::chrono::steady_clock::time_point) override
    {
        trace->consent_requests.emplace_back(rule);
        return consent_granted;
    }

    [[nodiscard]] int open_recovery_directory() override
    {
        return recovery_directory_descriptor_ == -1
            ? -1
            : dup(recovery_directory_descriptor_);
    }

    [[nodiscard]] bool remove_recovery_record()
    {
        return recovery_directory_descriptor_ != -1
            && unlinkat(recovery_directory_descriptor_, "recovery.state", 0) == 0;
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

private:
    std::string recovery_directory_path_;
    int recovery_directory_descriptor_{-1};
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
    const auto nat_rule{std::find_if(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "rule";
        })};
    ASSERT_NE(nat_rule, observed->recorded_tools.end());
    EXPECT_EQ(
        nat_rule->arguments,
        (std::vector<std::string>{
            "add", "rule", "ip", nat_rule->arguments[3], "postrouting",
            "iifname", "\"nll-host\"", "ip", "saddr", "10.200.0.2/32",
            "counter", "masquerade", "comment",
            "\"netlaglab:" + nat_rule->arguments[3].substr(10) + "\""}));
}

TEST(NetworkProductionAdapterTest, AppliesOutboundDelayThroughTheOwnedNamespace)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    for (int index{}; index < 11; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    const std::string noqueue{"qdisc noqueue 0: root refcnt 2\n"};
    production->tc_commands = {
        {CommandResultKind::success, 0, {}, noqueue},
        {CommandResultKind::success, 0, {}},
    };
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);
    queue_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};
    auto* environment{std::get_if<PreparedNetworkEnvironment>(&result)};
    ASSERT_NE(environment, nullptr);

    EXPECT_EQ(environment->set_delay(ShapingDirection::outbound, 100U),
              DelayChangeResult::applied);
    const auto shaping_command{std::find_if(
        observed->recorded_commands.begin(), observed->recorded_commands.end(),
        [](const RecordedCommand& command) {
            return command.executable == "/usr/sbin/tc"
                && command.arguments.size() > 1U
                && command.arguments[1] == "add";
        })};
    ASSERT_NE(shaping_command, observed->recorded_commands.end());
    EXPECT_TRUE(shaping_command->enters_namespace);
    EXPECT_EQ(shaping_command->arguments,
              (std::vector<std::string>{"qdisc", "add", "dev", "nll-app",
                  "root", "handle", "4e4c:", "netem", "delay", "100ms"}));
}

TEST(NetworkProductionAdapterTest, AppliesInboundDelayThroughHostTc)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    for (int index{}; index < 11; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    const std::string noqueue{"qdisc noqueue 0: root refcnt 2\n"};
    production->tc_commands = {
        {CommandResultKind::success, 0, {}, noqueue},
        {CommandResultKind::success, 0, {}},
    };
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);
    queue_placed_pair(*production);

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};
    auto* environment{std::get_if<PreparedNetworkEnvironment>(&result)};
    ASSERT_NE(environment, nullptr);
    EXPECT_EQ(environment->set_delay(ShapingDirection::inbound, 50U),
              DelayChangeResult::applied);

    const auto inbound_command{std::find_if(
        observed->recorded_tools.begin(), observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 3U && command.arguments[0] == "qdisc"
                && command.arguments[1] == "add"
                && command.arguments[3] == "nll-host";
        })};
    ASSERT_NE(inbound_command, observed->recorded_tools.end());
    EXPECT_EQ(inbound_command->executable, "/usr/sbin/tc");
    EXPECT_EQ(inbound_command->arguments.back(), "50ms");
}

TEST(NetworkProductionAdapterTest, DoesNotMatchDelayFromAnotherQdiscLine)
{
    const std::string noqueue{"qdisc noqueue 0: root refcnt 2\n"};
    const std::array<std::string, 2> unsupported_states{
        "qdisc netem 4e4c: root refcnt 2 limit 1000 delay 110.0ms\n"
            "qdisc netem 1: parent 4e4c:1 limit 1000 delay 10.0ms\n",
        "qdisc netem 4e4c: root refcnt 2 limit 1000 delay 10.0ms 2.0ms loss 1%\n",
    };
    for (const std::string& unsupported_state : unsupported_states) {
        auto production{std::make_unique<FakeProductionPlatform>()};
        for (int index{}; index < 11; ++index) {
            production->commands.push_back({CommandResultKind::success, 0, {}});
        }
        production->tc_commands = {
            {CommandResultKind::success, 0, {}, noqueue},
            {CommandResultKind::nonzero_exit, 1, {}},
            {CommandResultKind::success, 0, {}, unsupported_state},
            {CommandResultKind::success, 0, {}},
            {CommandResultKind::success, 0, {}, noqueue},
        };
        queue_namespace_creation(*production);
        queue_namespace_cleanup(*production);
        queue_host_pair(*production);
        queue_placed_pair(*production);
        queue_placed_pair(*production);
        queue_absent_placed_pair(*production);
        queue_placed_pair(*production);

        PreparationResult result{testing::prepare_with_production_platform(
            std::make_unique<PassingPreflight>(), std::move(production))};
        auto* environment{std::get_if<PreparedNetworkEnvironment>(&result)};
        ASSERT_NE(environment, nullptr);

        EXPECT_EQ(environment->set_delay(ShapingDirection::outbound, 10U),
                  DelayChangeResult::restored_after_failure);
    }
}

TEST(NetworkProductionAdapterTest, RefusesCustomIpv4PolicyRouting)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->ipv4_rules_output += "100: from all lookup 100\n";
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::unsupported_host_configuration);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, RefusesVpnDefaultRouteInTheMainTable)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->ipv4_default_route_output = "default dev tun0 proto static\n";
    production->default_route_link_output =
        "7: tun0: <POINTOPOINT,UP>\n    link/none\n    tun type tun\n";
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::unsupported_host_configuration);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, RefusesWhenNoIpv4DefaultRouteCanBeFollowed)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->ipv4_default_route_output.clear();
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::unsupported_host_configuration);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, RefusesDefaultRouteWithoutAnEgressDevice)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->ipv4_default_route_output =
        "default via 192.0.2.1 proto static\n"
        "default via 198.51.100.1 dev eth0 proto static\n";
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::unsupported_host_configuration);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, SupportsPppDefaultRoute)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    auto* const fake{production.get()};
    production->ipv4_default_route_output = "default dev ppp0 proto static\n";
    production->default_route_link_output =
        "9: ppp0: <POINTOPOINT,UP>\n    link/ppp\n";
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
    EXPECT_TRUE(std::any_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments == std::vector<std::string>{
                "-d", "link", "show", "dev", "ppp0"};
        }));
    CleanupResult cleanup{std::move(
        std::get<PreparedNetworkEnvironment>(result)).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_TRUE(fake->nft_table_name.empty());
}

TEST(NetworkProductionAdapterTest, DoesNotTreatFirewalldInspectionFailureAsInactive)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->firewalld_state_kind = CommandResultKind::system_failure;
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::unsupported_host_configuration);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, RefusesFirewallMutationWithoutInteractiveConsent)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->ufw_status_output = "Status: active\n";
    production->consent_granted = false;
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::internet_connectivity);
    EXPECT_TRUE(failure.rollback_failures.empty());
    ASSERT_EQ(observed->consent_requests.size(), 1U);
    EXPECT_NE(observed->consent_requests.front().find("nll-host"), std::string::npos);
    EXPECT_NE(observed->consent_requests.front().find("10.200.0.2"), std::string::npos);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && (command.arguments[1] == "table"
                    || command.arguments[1] == "rule");
        }));
}

TEST(NetworkProductionAdapterTest, AddsAndRemovesOnlyTheConsentedUfwRule)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->ufw_status_output = "Status: active\n";
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
    CleanupResult cleanup{std::move(
        std::get<PreparedNetworkEnvironment>(result)).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    ASSERT_EQ(observed->consent_requests.size(), 1U);
    EXPECT_NE(observed->consent_requests.front().find("nll-host"), std::string::npos);
    const auto allow{std::find_if(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "route"
                && command.arguments[1] == "allow";
        })};
    ASSERT_NE(allow, observed->recorded_tools.end());
    EXPECT_EQ(
        allow->arguments,
        (std::vector<std::string>{
            "route", "allow", "in", "on", "nll-host", "from", "10.200.0.2",
            "comment", allow->arguments.back()}));
    const auto remove{std::find_if(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "route"
                && command.arguments[1] == "delete";
        })};
    ASSERT_NE(remove, observed->recorded_tools.end());
    EXPECT_EQ(allow->arguments[allow->arguments.size() - 1U],
              remove->arguments.back());
}

TEST(NetworkProductionAdapterTest, AddsAndRemovesScopedFirewalldPolicy)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->firewalld_state_output = "running\n";
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
    CleanupResult cleanup{std::move(
        std::get<PreparedNetworkEnvironment>(result)).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    ASSERT_EQ(observed->consent_requests.size(), 1U);
    EXPECT_NE(observed->consent_requests.front().find("public"), std::string::npos);
    EXPECT_NE(observed->consent_requests.front().find("10.200.0.2/32"), std::string::npos);
    EXPECT_TRUE(std::any_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return !command.arguments.empty()
                && command.arguments[0].starts_with("--new-policy=netlaglab-");
        }));
    EXPECT_TRUE(std::any_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return !command.arguments.empty()
                && command.arguments[0].starts_with("--delete-policy=netlaglab-");
        }));
}

TEST(NetworkProductionAdapterTest, RemovesPartialFirewalldPolicyAfterSetupFailure)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->firewalld_state_output = "running\n";
    production->firewalld_failure_argument = "--add-egress-zone=ANY";
    for (int index{}; index < 11; ++index) {
        production->commands.push_back({CommandResultKind::success, 0, {}});
    }
    queue_namespace_creation(*production);
    queue_namespace_cleanup(*production);
    queue_host_pair(*production);
    queue_placed_pair(*production);
    queue_placed_pair(*production);
    queue_absent_placed_pair(*production);
    auto* const fake{production.get()};

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::internet_connectivity);
    EXPECT_TRUE(failure.rollback_failures.empty());
    EXPECT_FALSE(failure.residual.has_value());
    EXPECT_FALSE(fake->firewalld_policy_exists);
    EXPECT_TRUE(fake->nft_table_name.empty());
    EXPECT_TRUE(std::any_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return !command.arguments.empty()
                && command.arguments[0].starts_with("--delete-policy=netlaglab-");
        }));
}

TEST(NetworkProductionAdapterTest, RefusesTruncatedNftRulesetInspection)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    production->nft_ruleset_output = std::string(4096U, ' ');
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

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.stage,
        Stage::internet_connectivity);
    EXPECT_TRUE(std::none_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "add"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, RecreatesMissingJournalFromInMemoryOwnershipProof)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    auto* const fake{production.get()};
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
    ASSERT_TRUE(fake->remove_recovery_record());
    CleanupResult cleanup{std::move(
        std::get<PreparedNetworkEnvironment>(result)).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_TRUE(fake->nft_table_name.empty());
    EXPECT_TRUE(std::any_of(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() > 1U
                && command.arguments[0] == "delete"
                && command.arguments[1] == "table";
        }));
}

TEST(NetworkProductionAdapterTest, ReconcilesJournalBeforeRejectingFixedNameCollision)
{
    auto production{std::make_unique<FakeProductionPlatform>()};
    const auto observed{production->trace};
    const int journal_directory{production->open_recovery_directory()};
    ASSERT_NE(journal_directory, -1);
    const detail::RecoveryRecord record{
        detail::RecoveryBackend::nftables,
        detail::RecoveryPhase::applied,
        "0123456789abcdef0123456789abcdef",
    };
    production->nft_table_name = "netlaglab_" + record.token;
    ASSERT_TRUE(detail::write_recovery_record(
        journal_directory, ::geteuid(), record));

    PreparationResult result{testing::prepare_with_production_platform(
        std::make_unique<PassingPreflight>(true), std::move(production))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    EXPECT_EQ(
        std::get<PreparationFailure>(result).primary.cause,
        Cause::collision);
    const auto table_deletion{std::find_if(
        observed->recorded_tools.begin(),
        observed->recorded_tools.end(),
        [](const RecordedCommand& command) {
            return command.arguments.size() == 4U
                && command.arguments[0] == "delete"
                && command.arguments[1] == "table";
        })};
    std::string tool_log;
    for (const RecordedCommand& command : observed->recorded_tools) {
        for (const std::string& argument : command.arguments) {
            tool_log += argument + ' ';
        }
        tool_log.push_back('\n');
    }
    ASSERT_NE(table_deletion, observed->recorded_tools.end()) << tool_log;
    EXPECT_EQ(table_deletion->arguments[3], "netlaglab_" + record.token);
    EXPECT_EQ(
        detail::read_recovery_record(journal_directory, ::geteuid()).status,
        detail::RecoveryReadStatus::empty);
    close(journal_directory);
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

} // namespace
} // namespace netlaglab::network_environment
