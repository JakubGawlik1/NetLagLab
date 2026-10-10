#include "session_dns.hpp"
#include "workload_process.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab {
namespace {

class DnsFixture {
public:
    ~DnsFixture()
    {
        stopping_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (descriptor_ != -1) {
            close(descriptor_);
        }
    }

    [[nodiscard]] bool start(std::string& error)
    {
        descriptor_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (descriptor_ == -1) {
            error = std::strerror(errno);
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(53);
        (void)inet_pton(AF_INET, "127.0.0.2", &address.sin_addr);
        if (bind(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address))
            == -1) {
            error = std::strerror(errno);
            return false;
        }
        worker_ = std::thread{[this] { serve_one_query(); }};
        return true;
    }

    [[nodiscard]] bool responded() const noexcept
    {
        return responded_.load();
    }

private:
    void serve_one_query() noexcept
    {
        while (!stopping_.load()) {
            pollfd descriptor{descriptor_, POLLIN, 0};
            const int ready{poll(&descriptor, 1, 100)};
            if (ready <= 0) {
                continue;
            }
            std::array<unsigned char, 2048> query{};
            sockaddr_in peer{};
            socklen_t peer_size{sizeof(peer)};
            const ssize_t length{recvfrom(
                descriptor_, query.data(), query.size(), 0,
                reinterpret_cast<sockaddr*>(&peer), &peer_size)};
            if (length < 17) {
                continue;
            }
            std::size_t question_end{12};
            while (question_end < static_cast<std::size_t>(length)
                   && query[question_end] != 0U) {
                const std::size_t label_size{query[question_end]};
                if (label_size > 63U || question_end + label_size + 1U
                        >= static_cast<std::size_t>(length)) {
                    question_end = 0;
                    break;
                }
                question_end += label_size + 1U;
            }
            if (question_end == 0 || question_end + 5U
                    > static_cast<std::size_t>(length)) {
                continue;
            }
            question_end += 5U;
            std::vector<unsigned char> response{
                query.begin(), query.begin() + static_cast<std::ptrdiff_t>(question_end)};
            response[2] = 0x81;
            response[3] = 0x80;
            response[6] = 0;
            response[7] = 1;
            response[8] = response[9] = response[10] = response[11] = 0;
            constexpr std::array<unsigned char, 16> answer{
                0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                0x00, 0x3c, 0x00, 0x04, 203, 0, 113, 77};
            response.insert(response.end(), answer.begin(), answer.end());
            const ssize_t sent{sendto(
                descriptor_, response.data(), response.size(), 0,
                reinterpret_cast<const sockaddr*>(&peer), peer_size)};
            responded_.store(sent == static_cast<ssize_t>(response.size()));
            return;
        }
    }

    int descriptor_{-1};
    std::atomic<bool> stopping_{};
    std::atomic<bool> responded_{};
    std::thread worker_;
};

[[nodiscard]] std::optional<std::string> read_file(const char* const path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

TEST(SessionDnsPrivilegedTest, WorkloadUsesControlledDnsWithoutChangingHostResolverFiles)
{
    const char* const authorization{std::getenv("NETLAGLAB_ALLOW_PRIVILEGED_TESTS")};
    if (authorization == nullptr || std::string_view{authorization} != "1") {
        GTEST_SKIP() << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 for an explicit privileged run";
    }
    if (geteuid() != 0) {
        GTEST_SKIP() << "this DNS mount qualification requires direct root execution";
    }

    const auto host_resolver_before{read_file("/etc/resolv.conf")};
    const auto host_nsswitch_before{read_file("/etc/nsswitch.conf")};
    ASSERT_TRUE(host_resolver_before.has_value());
    ASSERT_TRUE(host_nsswitch_before.has_value());

    DnsFixture fixture;
    std::string fixture_error;
    ASSERT_TRUE(fixture.start(fixture_error)) << fixture_error;

    WorkloadContext context{
        "/", {DNS_PROBE_PATH, "session-dns-fixture.test"}, {"PATH=/usr/bin"}};
    const int group_count{getgroups(0, nullptr)};
    ASSERT_GE(group_count, 0);
    std::vector<gid_t> groups(static_cast<std::size_t>(group_count));
    if (group_count > 0) {
        ASSERT_EQ(getgroups(group_count, groups.data()), group_count);
    }
    const WorkloadIdentity identity{getuid(), getgid(), std::move(groups)};
    const session_dns::Snapshot dns_snapshot{
        "nameserver 127.0.0.2\noptions ndots:1 timeout:1 attempts:1\n",
        "hosts: files dns\n"};

    int output_pipe[2]{};
    ASSERT_EQ(pipe2(output_pipe, O_CLOEXEC), 0) << std::strerror(errno);
    FileDescriptor output_reader{output_pipe[0]};
    FileDescriptor output_writer{output_pipe[1]};
    WorkloadStandardDescriptors descriptors;
    descriptors.sources[1] = output_writer.get();
    WorkloadLaunchResult launch{
        launch_workload(context, identity, descriptors, nullptr, &dns_snapshot)};
    ASSERT_TRUE(launch.process.has_value()) << launch.diagnostic;
    output_writer.reset();

    const std::optional<WorkloadStatus> status{launch.process->wait()};
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(status->exited);
    EXPECT_EQ(status->value, 0);

    std::ostringstream output;
    std::array<char, 256> buffer{};
    while (true) {
        const ssize_t count{read(output_reader.get(), buffer.data(), buffer.size())};
        if (count > 0) {
            output.write(buffer.data(), count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        ASSERT_EQ(count, 0) << std::strerror(errno);
        break;
    }
    EXPECT_NE(output.str().find("203.0.113.77"), std::string::npos);
    EXPECT_TRUE(fixture.responded());
    EXPECT_EQ(read_file("/etc/resolv.conf"), host_resolver_before);
    EXPECT_EQ(read_file("/etc/nsswitch.conf"), host_nsswitch_before);
}

} // namespace
} // namespace netlaglab
