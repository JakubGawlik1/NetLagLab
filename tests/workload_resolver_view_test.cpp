#include "workload_resolver_view.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <array>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <string_view>

namespace netlaglab {
namespace {


class SnapshotDirectory {
public:
    SnapshotDirectory()
    {
        std::array<char, 64> name{};
        constexpr std::string_view pattern{"/tmp/netlaglab-snapshot-XXXXXX"};
        std::copy(pattern.begin(), pattern.end(), name.begin());
        if (mkdtemp(name.data()) != nullptr) path = name.data();
    }
    ~SnapshotDirectory() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

TEST(WorkloadResolverViewTest, MaterializesSealedSnapshotsAsLinkedReadOnlyFilesystemFiles)
{
    SnapshotDirectory storage;
    ASSERT_FALSE(storage.path.empty());
    FileDescriptor directory{open(storage.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    ASSERT_NE(directory.get(), -1);
    const ResolverViewConfiguration configuration{"nameserver 192.0.2.53\n", "hosts: files dns\n"};
    auto view{create_workload_resolver_view(configuration)};
    ASSERT_TRUE(view.view.has_value());
    FileDescriptor copy{materialize_resolver_snapshot(
        view.view->resolv_conf_descriptor(), directory.get(), ResolverSnapshotFile::resolv_conf)};
    ASSERT_NE(copy.get(), -1);
    struct stat status{};
    struct stat directory_status{};
    ASSERT_EQ(fstat(copy.get(), &status), 0);
    ASSERT_EQ(fstat(directory.get(), &directory_status), 0);
    EXPECT_EQ(status.st_dev, directory_status.st_dev);
    EXPECT_EQ(status.st_nlink, 1U);
    EXPECT_EQ(status.st_mode & 0777, 0444U);
    EXPECT_NE(fcntl(copy.get(), F_GETFD) & FD_CLOEXEC, 0);
    std::array<char, 64> contents{};
    const auto count{pread(copy.get(), contents.data(), contents.size(), 0)};
    ASSERT_GT(count, 0);
    EXPECT_EQ(std::string_view(contents.data(), static_cast<std::size_t>(count)), configuration.resolv_conf);
    EXPECT_TRUE(std::filesystem::exists(storage.path / "resolv.conf"));
    EXPECT_EQ(fcntl(view.view->resolv_conf_descriptor(), F_GET_SEALS),
              F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
}


TEST(WorkloadResolverViewTest, RejectsUnsafeStorageAndUnsealedSourceBeforeCreatingCopy)
{
    SnapshotDirectory storage;
    ASSERT_FALSE(storage.path.empty());
    FileDescriptor directory{open(storage.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    ASSERT_NE(directory.get(), -1);
    auto view{create_workload_resolver_view({"nameserver 192.0.2.53\n", "hosts: files dns\n"})};
    ASSERT_TRUE(view.view.has_value());
    ASSERT_EQ(fchmod(directory.get(), 0777), 0);
    auto unsafe{materialize_resolver_snapshot(view.view->resolv_conf_descriptor(),
        directory.get(), ResolverSnapshotFile::resolv_conf)};
    EXPECT_EQ(unsafe.get(), -1);
    EXPECT_TRUE(std::filesystem::is_empty(storage.path));
    ASSERT_EQ(fchmod(directory.get(), 0700), 0);
    auto unsealed{materialize_resolver_snapshot(directory.get(), directory.get(),
        ResolverSnapshotFile::resolv_conf)};
    EXPECT_EQ(unsealed.get(), -1);
    EXPECT_TRUE(std::filesystem::is_empty(storage.path));
}

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

TEST(WorkloadResolverViewTest, SelectsIpv4ResolversFromMixedHostConfiguration)
{
    std::string failure;
    const auto snapshot{make_resolver_snapshot(
        "# NetworkManager\nsearch home play.pl\n"
        "nameserver 2001:db8::53\n"
        "nameserver 192.0.2.53\n"
        "nameserver 2001:db8::54\n"
        "nameserver 192.0.2.54\n"
        "options timeout:2 attempts:2\n",
        failure)};

    ASSERT_TRUE(snapshot.has_value()) << failure;
    EXPECT_EQ(*snapshot,
        "search home play.pl\n"
        "nameserver 192.0.2.53\n"
        "nameserver 192.0.2.54\n"
        "options timeout:2 attempts:2\n");
}

TEST(WorkloadResolverViewTest, RejectsHostOnlyAndNonIpv4Resolvers)
{
    std::string failure;
    EXPECT_FALSE(make_resolver_snapshot("nameserver 127.0.0.53\n", failure));
    EXPECT_FALSE(make_resolver_snapshot("nameserver 2001:db8::53\n", failure));
    EXPECT_FALSE(make_resolver_snapshot(
        "nameserver 192.0.2.53\nnameserver not:an:address\n", failure));
    EXPECT_FALSE(make_resolver_snapshot(
        "nameserver 192.0.2.53\nnameserver 2001:db8::53 extra\n", failure));
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
