#include "network_environment/preflight.hpp"
#include "network_environment/production_test_support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace netlaglab::network_environment::detail {
namespace {

constexpr std::uint32_t sequence{17};
constexpr std::uint32_t port_id{29};
constexpr Ipv4Prefix session_subnet{0x0ac80000U, 30};

class FakeHostLock final : public HostLock {
public:
    explicit FakeHostLock(std::shared_ptr<bool> alive)
        : alive_{std::move(alive)}
    {
        *alive_ = true;
    }

    ~FakeHostLock() override { *alive_ = false; }

private:
    std::shared_ptr<bool> alive_;
};

class FakePreflightPlatform final : public PreflightPlatform {
public:
    HostLockResult acquire_host_lock() override
    {
        calls->emplace_back("lock");
        if (!lock_succeeds) {
            return {nullptr, lock_failure};
        }
        return {
            std::make_unique<FakeHostLock>(lock_alive),
            Cause::system_failure,
        };
    }

    bool privileged() const override
    {
        calls->emplace_back("privilege");
        return is_privileged;
    }

    ToolQuery query_tool(const std::string_view path) override
    {
        calls->emplace_back(std::string{"tool:"} + std::string{path});
        if (path == valid_tool_path) {
            return {QueryStatus::present, valid_tool_metadata};
        }
        return {tool_query_status, {}};
    }

    QueryStatus query_namespace_name() override
    {
        calls->emplace_back("namespace");
        return namespace_status;
    }

    QueryStatus query_link_name(const std::string_view name) override
    {
        calls->emplace_back(std::string{"link:"} + std::string{name});
        return name == "nll-host" ? host_link_status : session_link_status;
    }

    AddressQuery query_addresses() override
    {
        calls->emplace_back("addresses");
        return addresses;
    }

    RouteDumpStatus query_routes(
        const std::chrono::steady_clock::time_point) override
    {
        calls->emplace_back("routes");
        return route_status;
    }

    std::shared_ptr<std::vector<std::string>> calls{
        std::make_shared<std::vector<std::string>>()};
    bool lock_succeeds{true};
    Cause lock_failure{Cause::system_failure};
    std::shared_ptr<bool> lock_alive{std::make_shared<bool>(false)};
    bool is_privileged{true};
    std::string valid_tool_path{"/usr/bin/ip"};
    FileMetadata valid_tool_metadata{true, 0, 0755};
    QueryStatus tool_query_status{QueryStatus::absent};
    QueryStatus namespace_status{QueryStatus::absent};
    QueryStatus host_link_status{QueryStatus::absent};
    QueryStatus session_link_status{QueryStatus::absent};
    AddressQuery addresses{true, {}};
    RouteDumpStatus route_status{RouteDumpStatus::complete};
};

void append_bytes(std::vector<std::byte>& output, const void* data, std::size_t size)
{
    const auto* first{static_cast<const std::byte*>(data)};
    output.insert(output.end(), first, first + size);
}

void append_route(
    std::vector<std::byte>& output,
    const std::uint32_t destination,
    const std::uint8_t prefix_length,
    const std::uint32_t table)
{
    struct RouteMessage {
        nlmsghdr header;
        rtmsg route;
        rtattr destination_attribute;
        std::uint32_t destination_value;
        rtattr table_attribute;
        std::uint32_t table_value;
    } message{};
    message.header.nlmsg_len = sizeof(message);
    message.header.nlmsg_type = RTM_NEWROUTE;
    message.header.nlmsg_flags = NLM_F_MULTI;
    message.header.nlmsg_seq = sequence;
    message.header.nlmsg_pid = port_id;
    message.route.rtm_family = AF_INET;
    message.route.rtm_dst_len = prefix_length;
    message.route.rtm_table = RT_TABLE_UNSPEC;
    message.destination_attribute.rta_len = RTA_LENGTH(sizeof(destination));
    message.destination_attribute.rta_type = RTA_DST;
    message.destination_value = htonl(destination);
    message.table_attribute.rta_len = RTA_LENGTH(sizeof(table));
    message.table_attribute.rta_type = RTA_TABLE;
    message.table_value = table;
    append_bytes(output, &message, sizeof(message));
}

void append_done(std::vector<std::byte>& output)
{
    nlmsghdr message{};
    message.nlmsg_len = NLMSG_LENGTH(0);
    message.nlmsg_type = NLMSG_DONE;
    message.nlmsg_flags = NLM_F_MULTI;
    message.nlmsg_seq = sequence;
    message.nlmsg_pid = port_id;
    append_bytes(output, &message, sizeof(message));
}

TEST(NetworkPrefixTest, DetectsExactBroaderAndNarrowerIntersections)
{
    EXPECT_TRUE(prefixes_overlap(session_subnet, {0x0ac80000U, 30}));
    EXPECT_TRUE(prefixes_overlap(session_subnet, {0x0a000000U, 8}));
    EXPECT_TRUE(prefixes_overlap(session_subnet, {0x0ac80002U, 32}));
    EXPECT_FALSE(prefixes_overlap(session_subnet, {0x0ac80004U, 30}));
}

TEST(NetworkPreflightTest, AcceptsOnlyClosedListRootOwnedSafeExecutables)
{
    const auto paths{trusted_ip_paths()};
    EXPECT_EQ(
        std::vector<std::string_view>(paths.begin(), paths.end()),
        (std::vector<std::string_view>{
            std::string_view{"/usr/sbin/ip"},
            std::string_view{"/usr/bin/ip"},
            std::string_view{"/sbin/ip"},
            std::string_view{"/bin/ip"},
        }));
    EXPECT_TRUE(is_trusted_executable({true, 0, 0755}));
    EXPECT_FALSE(is_trusted_executable({false, 0, 0755}));
    EXPECT_FALSE(is_trusted_executable({true, 1000, 0755}));
    EXPECT_FALSE(is_trusted_executable({true, 0, 0775}));
    EXPECT_TRUE(is_trusted_executable({true, 0, 0754}));
    EXPECT_FALSE(is_trusted_executable({true, 0, 0757}));
    EXPECT_FALSE(is_trusted_executable({true, 0, 0644}));
}

TEST(NetworkPreflightTest, CompletesEveryReadOnlyCheckInOrder)
{
    FakePreflightPlatform platform;

    const PreflightResult result{run_preflight(platform, {})};

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.ip_path, "/usr/bin/ip");
    EXPECT_EQ(
        *platform.calls,
        (std::vector<std::string>{
            "privilege",
            "tool:/usr/sbin/ip",
            "tool:/usr/bin/ip",
            "namespace",
            "link:nll-host",
            "link:nll-app",
            "addresses",
            "routes",
        }));
}

TEST(NetworkPreflightTest, RejectsEveryCollisionBeforeAnyLaterCheck)
{
    struct Case {
        QueryStatus FakePreflightPlatform::*status;
        std::vector<std::string> final_calls;
    };
    const std::array cases{
        Case{&FakePreflightPlatform::namespace_status, {"namespace"}},
        Case{&FakePreflightPlatform::host_link_status, {"link:nll-host"}},
        Case{&FakePreflightPlatform::session_link_status, {"link:nll-app"}},
    };
    for (const Case& value : cases) {
        FakePreflightPlatform platform;
        platform.*(value.status) = QueryStatus::present;

        const PreflightResult result{run_preflight(platform, {})};

        EXPECT_FALSE(result.succeeded);
        EXPECT_EQ(result.cause, Cause::collision);
        ASSERT_GE(platform.calls->size(), value.final_calls.size());
        EXPECT_EQ(
            std::vector<std::string>(
                platform.calls->end()
                    - static_cast<std::ptrdiff_t>(value.final_calls.size()),
                platform.calls->end()),
            value.final_calls);
    }
}

TEST(NetworkPreflightTest, RejectsIntersectingAddressesAndRoutes)
{
    for (const Ipv4Prefix prefix : std::array{
             Ipv4Prefix{0x0ac80000U, 30},
             Ipv4Prefix{0x0a000000U, 8},
             Ipv4Prefix{0x0ac80002U, 32},
         }) {
        FakePreflightPlatform platform;
        platform.addresses.prefixes = {prefix};
        const PreflightResult result{run_preflight(platform, {})};
        EXPECT_FALSE(result.succeeded);
        EXPECT_EQ(result.cause, Cause::collision);
        EXPECT_EQ(platform.calls->back(), "addresses");
    }

    FakePreflightPlatform platform;
    platform.route_status = RouteDumpStatus::collision;
    const PreflightResult result{run_preflight(platform, {})};
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.cause, Cause::collision);
    EXPECT_EQ(platform.calls->back(), "routes");
}

TEST(NetworkPreflightTest, MapsToolAndInventoryFailuresToStableCauses)
{
    FakePreflightPlatform unprivileged;
    unprivileged.is_privileged = false;
    EXPECT_EQ(run_preflight(unprivileged, {}).cause, Cause::system_failure);
    EXPECT_EQ(*unprivileged.calls, (std::vector<std::string>{"privilege"}));

    FakePreflightPlatform invalid_tool;
    invalid_tool.valid_tool_metadata.mode = 0775;
    EXPECT_EQ(
        run_preflight(invalid_tool, {}).cause,
        Cause::unavailable_or_invalid_tool);

    FakePreflightPlatform failed_tool_query;
    failed_tool_query.valid_tool_path.clear();
    failed_tool_query.tool_query_status = QueryStatus::failure;
    EXPECT_EQ(
        run_preflight(failed_tool_query, {}).cause, Cause::system_failure);

    FakePreflightPlatform failed_name_query;
    failed_name_query.namespace_status = QueryStatus::failure;
    EXPECT_EQ(
        run_preflight(failed_name_query, {}).cause, Cause::system_failure);

    FakePreflightPlatform failed_addresses;
    failed_addresses.addresses.succeeded = false;
    EXPECT_EQ(run_preflight(failed_addresses, {}).cause, Cause::system_failure);

    for (const RouteDumpStatus status : {
             RouteDumpStatus::pending,
             RouteDumpStatus::netlink_error,
             RouteDumpStatus::malformed,
         }) {
        FakePreflightPlatform failed_routes;
        failed_routes.route_status = status;
        EXPECT_EQ(
            run_preflight(failed_routes, {}).cause, Cause::system_failure);
    }

    FakePreflightPlatform timed_out_routes;
    timed_out_routes.route_status = RouteDumpStatus::timeout;
    EXPECT_EQ(run_preflight(timed_out_routes, {}).cause, Cause::timeout);
}

TEST(NetworkProductionPreparationTest, AcquiresLockBeforeTypedPreflightFailure)
{
    auto platform{std::make_unique<FakePreflightPlatform>()};
    const auto calls{platform->calls};
    const auto lock_alive{platform->lock_alive};
    platform->namespace_status = QueryStatus::present;

    PreparationResult result{
        testing::prepare_with_preflight_platform(std::move(platform))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::preflight);
    EXPECT_EQ(failure.primary.cause, Cause::collision);
    EXPECT_EQ(calls->front(), "lock");
    EXPECT_FALSE(*lock_alive);
}

TEST(NetworkProductionPreparationTest, MapsHostLockFailureWithoutStartingPreflight)
{
    auto platform{std::make_unique<FakePreflightPlatform>()};
    const auto calls{platform->calls};
    platform->lock_succeeds = false;
    platform->lock_failure = Cause::collision;

    PreparationResult result{
        testing::prepare_with_preflight_platform(std::move(platform))};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::preflight);
    EXPECT_EQ(failure.primary.cause, Cause::collision);
    EXPECT_EQ(*calls, (std::vector<std::string>{"lock"}));
}

TEST(NetworkProductionPreparationTest, EveryRejectedPreconditionStartsNoMutation)
{
    const auto verify_rejection = [](auto configure) {
        auto platform{std::make_unique<FakePreflightPlatform>()};
        configure(*platform);
        auto trace{std::make_shared<testing::ProductionTrace>()};

        PreparationResult result{testing::prepare_with_preflight_platform(
            std::move(platform), trace)};

        ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
        EXPECT_EQ(
            std::get<PreparationFailure>(result).primary.stage,
            Stage::preflight);
        EXPECT_EQ(trace->semantic_mutation_requests, 0U);
    };

    verify_rejection([](FakePreflightPlatform& value) {
        value.is_privileged = false;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.valid_tool_metadata.mode = 0775;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.valid_tool_path.clear();
        value.tool_query_status = QueryStatus::failure;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.namespace_status = QueryStatus::present;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.namespace_status = QueryStatus::failure;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.host_link_status = QueryStatus::present;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.session_link_status = QueryStatus::present;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.addresses.prefixes = {{0x0ac80002U, 32}};
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.addresses.succeeded = false;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.route_status = RouteDumpStatus::collision;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.route_status = RouteDumpStatus::malformed;
    });
    verify_rejection([](FakePreflightPlatform& value) {
        value.route_status = RouteDumpStatus::timeout;
    });
}

TEST(NetworkProductionPreparationTest, MutationTraceDetectsAcceptedPreflight)
{
    auto platform{std::make_unique<FakePreflightPlatform>()};
    auto trace{std::make_shared<testing::ProductionTrace>()};

    PreparationResult result{testing::prepare_with_preflight_platform(
        std::move(platform), trace)};

    ASSERT_TRUE(std::holds_alternative<PreparationFailure>(result));
    const PreparationFailure& failure{std::get<PreparationFailure>(result)};
    EXPECT_EQ(failure.primary.stage, Stage::namespace_creation);
    EXPECT_EQ(trace->semantic_mutation_requests, 1U);
}

TEST(NetworkRouteDumpTest, AcceptsDefaultsAcrossTablesAndMultipartCompletion)
{
    std::vector<std::byte> first;
    append_route(first, 0, 0, RT_TABLE_MAIN);
    std::vector<std::byte> second;
    append_route(second, 0, 0, 100);
    append_done(second);

    RouteDumpDecoder decoder{sequence, port_id};
    EXPECT_EQ(decoder.consume(first), RouteDumpStatus::pending);
    EXPECT_EQ(decoder.consume(second), RouteDumpStatus::complete);
}

TEST(NetworkRouteDumpTest, RejectsIntersectingRouteInAnyTable)
{
    std::vector<std::byte> bytes;
    append_route(bytes, 0xc0000200U, 24, RT_TABLE_MAIN);
    append_route(bytes, 0x0ac80002U, 32, 100);
    append_done(bytes);

    RouteDumpDecoder decoder{sequence, port_id};
    EXPECT_EQ(decoder.consume(bytes), RouteDumpStatus::collision);
}

TEST(NetworkRouteDumpTest, RejectsNetlinkErrorsTruncationAndMalformedAttributes)
{
    nlmsgerr error{};
    error.error = -EPERM;
    nlmsghdr error_header{};
    error_header.nlmsg_len = NLMSG_LENGTH(sizeof(error));
    error_header.nlmsg_type = NLMSG_ERROR;
    error_header.nlmsg_seq = sequence;
    error_header.nlmsg_pid = port_id;
    std::vector<std::byte> error_bytes;
    append_bytes(error_bytes, &error_header, sizeof(error_header));
    append_bytes(error_bytes, &error, sizeof(error));
    RouteDumpDecoder error_decoder{sequence, port_id};
    EXPECT_EQ(error_decoder.consume(error_bytes), RouteDumpStatus::netlink_error);
    EXPECT_EQ(error_decoder.error_number(), EPERM);

    std::vector<std::byte> truncated;
    append_route(truncated, 0xc0000200U, 24, RT_TABLE_MAIN);
    truncated.pop_back();
    RouteDumpDecoder truncated_decoder{sequence, port_id};
    EXPECT_EQ(
        truncated_decoder.consume(truncated), RouteDumpStatus::malformed);

    std::vector<std::byte> malformed;
    append_route(malformed, 0xc0000200U, 24, RT_TABLE_MAIN);
    rtattr invalid_attribute{};
    invalid_attribute.rta_len = sizeof(rtattr) - 1;
    invalid_attribute.rta_type = RTA_DST;
    std::memcpy(
        malformed.data() + NLMSG_LENGTH(sizeof(rtmsg)),
        &invalid_attribute,
        sizeof(invalid_attribute));
    RouteDumpDecoder malformed_decoder{sequence, port_id};
    EXPECT_EQ(
        malformed_decoder.consume(malformed), RouteDumpStatus::malformed);
}

TEST(NetworkRouteDumpTest, RequiresMatchingMultipartCompletion)
{
    std::vector<std::byte> bytes;
    append_route(bytes, 0xc0000200U, 24, RT_TABLE_MAIN);

    RouteDumpDecoder decoder{sequence, port_id};
    EXPECT_EQ(decoder.consume(bytes), RouteDumpStatus::pending);

    std::vector<std::byte> completion;
    append_done(completion);
    nlmsghdr done{};
    std::memcpy(&done, completion.data(), sizeof(done));
    done.nlmsg_seq = sequence + 1;
    std::memcpy(completion.data(), &done, sizeof(done));
    EXPECT_EQ(decoder.consume(completion), RouteDumpStatus::malformed);
}

TEST(NetworkRouteDumpTest, RejectsDataAfterMultipartCompletion)
{
    std::vector<std::byte> bytes;
    append_done(bytes);
    append_route(bytes, 0xc0000200U, 24, RT_TABLE_MAIN);

    RouteDumpDecoder decoder{sequence, port_id};
    EXPECT_EQ(decoder.consume(bytes), RouteDumpStatus::malformed);
}

TEST(NetworkRouteDumpTest, RejectsRouteOutsideMultipartDump)
{
    std::vector<std::byte> bytes;
    append_route(bytes, 0xc0000200U, 24, RT_TABLE_MAIN);
    nlmsghdr header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.nlmsg_flags = 0;
    std::memcpy(bytes.data(), &header, sizeof(header));

    RouteDumpDecoder decoder{sequence, port_id};
    EXPECT_EQ(decoder.consume(bytes), RouteDumpStatus::malformed);
}

} // namespace
} // namespace netlaglab::network_environment::detail
