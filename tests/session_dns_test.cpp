#include "session_dns.hpp"

#include <gtest/gtest.h>

#include <string>

namespace netlaglab::session_dns {
namespace {

TEST(SessionDnsTest, BuildsResolverSnapshotAndRestrictsHostsLookupToFilesAndDns)
{
    const SnapshotResult result{parse_host_dns_snapshot(
        "# generated configuration\n"
        "nameserver 192.0.2.53\n"
        "search example.test corp.example.test\n"
        "options ndots:2 timeout:1 attempts:2 edns0\n",
        "passwd: files systemd\n"
        "hosts: files mdns4_minimal [NOTFOUND=return] dns\n"
        "group: files systemd\n")};

    ASSERT_TRUE(result.snapshot.has_value()) << result.error;
    EXPECT_EQ(
        result.snapshot->resolver_contents,
        "nameserver 192.0.2.53\n"
        "search example.test corp.example.test\n"
        "options ndots:2 timeout:1 attempts:2 edns0\n");
    EXPECT_EQ(
        result.snapshot->nsswitch_contents,
        "passwd: files systemd\n"
        "hosts: files dns\n"
        "group: files systemd\n");
}

TEST(SessionDnsTest, RejectsLoopbackResolverThatWouldDependOnHostOnlyService)
{
    const SnapshotResult result{parse_host_dns_snapshot(
        "nameserver 127.0.0.53\n",
        "hosts: files resolve dns\n")};

    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_NE(result.error.find("loopback"), std::string::npos);
}

TEST(SessionDnsTest, RejectsUnsupportedSplitDnsNssResolver)
{
    const SnapshotResult result{parse_host_dns_snapshot(
        "nameserver 192.0.2.53\n",
        "hosts: files resolve [!UNAVAIL=return] dns\n")};

    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_NE(result.error.find("split-DNS"), std::string::npos);
}

TEST(SessionDnsTest, RejectsRouteOnlySplitDnsSearchDomains)
{
    const SnapshotResult result{parse_host_dns_snapshot(
        "nameserver 192.0.2.53\nsearch ~private.example.test\n",
        "hosts: files dns\n")};

    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_NE(result.error.find("Split-DNS"), std::string::npos);
}

TEST(SessionDnsTest, RejectsIpv6AndMalformedResolverInputs)
{
    const SnapshotResult ipv6{parse_host_dns_snapshot(
        "nameserver 2001:db8::53\n", "hosts: files dns\n")};
    const SnapshotResult malformed{parse_host_dns_snapshot(
        "nameserver 192.0.2.999\n", "hosts: files dns\n")};

    EXPECT_FALSE(ipv6.snapshot.has_value());
    EXPECT_FALSE(malformed.snapshot.has_value());
}

TEST(SessionDnsTest, RejectsResolverSnapshotsWithoutAnUpstreamServer)
{
    const SnapshotResult result{parse_host_dns_snapshot(
        "search example.test\n", "hosts: files dns\n")};

    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_NE(result.error.find("nameserver"), std::string::npos);
}

TEST(SessionDnsTest, RejectsOversizedResolverAndNssInputs)
{
    const std::string oversized(64U * 1024U + 1U, 'x');
    const SnapshotResult resolver_result{parse_host_dns_snapshot(
        oversized, "hosts: files dns\n")};
    const SnapshotResult nss_result{parse_host_dns_snapshot(
        "nameserver 192.0.2.53\n", oversized)};

    EXPECT_FALSE(resolver_result.snapshot.has_value());
    EXPECT_FALSE(nss_result.snapshot.has_value());
}

TEST(SessionDnsTest, RejectsUnknownResolverOptionsAndNssIncludes)
{
    const SnapshotResult option_result{parse_host_dns_snapshot(
        "nameserver 192.0.2.53\noptions rotate-unknown\n",
        "hosts: files dns\n")};
    const SnapshotResult include_result{parse_host_dns_snapshot(
        "nameserver 192.0.2.53\n", "include /etc/nss-extra.conf\n")};

    EXPECT_FALSE(option_result.snapshot.has_value());
    EXPECT_FALSE(include_result.snapshot.has_value());
}

} // namespace
} // namespace netlaglab::session_dns
