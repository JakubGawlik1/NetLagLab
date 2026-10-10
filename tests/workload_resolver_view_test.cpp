#include "workload_resolver_view.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <string>
#include <string_view>

namespace netlaglab {
namespace {

TEST(WorkloadResolverViewTest, BuildsBoundedIpv4ResolverSnapshot)
{
    std::string failure;
    const auto snapshot{make_resolver_snapshot(
        "# host resolver\nnameserver 192.0.2.53\n"
        "search example.test corp.example.test\noptions timeout:2 attempts:2\n",
        failure)};

    ASSERT_TRUE(snapshot.has_value()) << failure;
    EXPECT_EQ(
        *snapshot,
        "nameserver 192.0.2.53\n"
        "search example.test corp.example.test\n"
        "options timeout:2 attempts:2\n");
}

TEST(WorkloadResolverViewTest, RejectsHostOnlyAndNonIpv4Resolvers)
{
    std::string failure;
    EXPECT_FALSE(make_resolver_snapshot("nameserver 127.0.0.53\n", failure));
    EXPECT_FALSE(make_resolver_snapshot("nameserver 2001:db8::53\n", failure));
    EXPECT_FALSE(failure.empty());
}

TEST(WorkloadResolverViewTest, RejectsResolverDirectivesWithUnmodeledSemantics)
{
    std::string failure;
    EXPECT_FALSE(make_resolver_snapshot(
        "nameserver 192.0.2.53\noptions rotate\nsortlist 192.0.2.0/24\n",
        failure));
    EXPECT_FALSE(failure.empty());
}

TEST(WorkloadResolverViewTest, ReplacesHostLookupModulesWithPrivateFilesAndDns)
{
    std::string failure;
    const auto snapshot{make_nsswitch_snapshot(
        "passwd: files\nhosts: mymachines resolve [!UNAVAIL=return] files myhostname dns # host\n"
        "group: files\n",
        failure)};
    ASSERT_TRUE(snapshot.has_value()) << failure;
    EXPECT_EQ(
        *snapshot,
        "passwd: files\nhosts: files dns # host\ngroup: files\n");

    EXPECT_FALSE(make_nsswitch_snapshot("hosts: files\nhosts: dns\n", failure));
    EXPECT_FALSE(failure.empty());
}

TEST(WorkloadResolverViewTest, SnapshotFilesAreSealedBeforeWorkloadLaunch)
{
    const ResolverViewConfiguration configuration{
        "nameserver 192.0.2.53\n",
        "hosts: files dns\n",
    };
    WorkloadResolverViewResult result{
        create_workload_resolver_view(configuration)};

    ASSERT_TRUE(result.view.has_value()) << result.failure;
    EXPECT_EQ(
        fcntl(result.view->resolv_conf_descriptor(), F_GET_SEALS),
        F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    EXPECT_EQ(
        fcntl(result.view->nsswitch_conf_descriptor(), F_GET_SEALS),
        F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
}

} // namespace
} // namespace netlaglab
