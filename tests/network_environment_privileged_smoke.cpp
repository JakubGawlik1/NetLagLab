#include "file_descriptor.hpp"
#include "network_environment/network_environment.hpp"
#include "network_environment/production_test_support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <net/route.h>
#include <optional>
#include <poll.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace netlaglab::network_environment {
namespace {

constexpr int io_timeout_milliseconds{5'000};
constexpr std::string_view packet_payload{"netlaglab-smoke"};

class ChildGuard {
public:
    explicit ChildGuard(const pid_t child) : child_{child} {}
    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;

    ~ChildGuard()
    {
        if (child_ > 0) {
            (void)kill(child_, SIGKILL);
            int status{};
            while (waitpid(child_, &status, 0) == -1 && errno == EINTR) {
            }
        }
    }

    [[nodiscard]] int reap()
    {
        int status{};
        pid_t result{};
        do {
            result = waitpid(child_, &status, 0);
        } while (result == -1 && errno == EINTR);
        child_ = -1;
        return result > 0 ? status : -1;
    }

private:
    pid_t child_;
};

struct ChildReady {
    std::int32_t status;
    std::uint16_t port;
};

[[nodiscard]] bool wait_readable(const int descriptor)
{
    pollfd event{descriptor, POLLIN, 0};
    int result{};
    do {
        result = poll(&event, 1, io_timeout_milliseconds);
    } while (result == -1 && errno == EINTR);
    return result > 0 && (event.revents & POLLIN) != 0;
}

[[nodiscard]] bool write_complete(
    const int descriptor,
    const void* data,
    const std::size_t size)
{
    const auto* bytes{static_cast<const std::byte*>(data)};
    std::size_t written{};
    while (written < size) {
        const ssize_t result{write(descriptor, bytes + written, size - written)};
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_complete(
    const int descriptor,
    void* data,
    const std::size_t size)
{
    auto* bytes{static_cast<std::byte*>(data)};
    std::size_t received{};
    while (received < size) {
        const ssize_t result{read(descriptor, bytes + received, size - received)};
        if (result > 0) {
            received += static_cast<std::size_t>(result);
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_ipv4_interface(
    const std::string_view name,
    const char* address,
    const char* netmask)
{
    in_addr expected_address{};
    in_addr expected_netmask{};
    if (inet_pton(AF_INET, address, &expected_address) != 1
        || inet_pton(AF_INET, netmask, &expected_netmask) != 1) {
        return false;
    }

    ifaddrs* raw_interfaces{};
    if (getifaddrs(&raw_interfaces) == -1) {
        return false;
    }
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> interfaces{
        raw_interfaces, freeifaddrs};
    for (const ifaddrs* current{interfaces.get()}; current != nullptr;
         current = current->ifa_next) {
        if (current->ifa_name == nullptr || current->ifa_addr == nullptr
            || current->ifa_netmask == nullptr
            || current->ifa_addr->sa_family != AF_INET
            || name != current->ifa_name || (current->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        const auto* actual_address{
            reinterpret_cast<const sockaddr_in*>(current->ifa_addr)};
        const auto* actual_netmask{
            reinterpret_cast<const sockaddr_in*>(current->ifa_netmask)};
        if (actual_address->sin_addr.s_addr == expected_address.s_addr
            && actual_netmask->sin_addr.s_addr == expected_netmask.s_addr) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_expected_default_route()
{
    in_addr gateway{};
    if (inet_pton(AF_INET, "10.200.0.1", &gateway) != 1) {
        return false;
    }
    std::ifstream routes{"/proc/net/route"};
    std::string line;
    (void)std::getline(routes, line);
    while (std::getline(routes, line)) {
        std::istringstream fields{line};
        std::string interface;
        unsigned long destination{};
        unsigned long actual_gateway{};
        unsigned long flags{};
        unsigned long reference_count{};
        unsigned long use{};
        unsigned long metric{};
        unsigned long mask{};
        if (fields >> interface >> std::hex >> destination >> actual_gateway
            >> flags >> std::dec >> reference_count >> use >> metric >> std::hex
            >> mask
            && interface == "nll-app" && destination == 0 && mask == 0
            && actual_gateway == gateway.s_addr && (flags & RTF_UP) != 0
            && (flags & RTF_GATEWAY) != 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<netlaglab::FileDescriptor> bind_udp(
    const char* address,
    std::uint16_t& port)
{
    netlaglab::FileDescriptor socket_descriptor{
        socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (socket_descriptor.get() == -1) {
        return std::nullopt;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = 0;
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1
        || bind(
               socket_descriptor.get(),
               reinterpret_cast<const sockaddr*>(&endpoint),
               sizeof(endpoint))
            == -1) {
        return std::nullopt;
    }
    socklen_t endpoint_size{sizeof(endpoint)};
    if (getsockname(
            socket_descriptor.get(),
            reinterpret_cast<sockaddr*>(&endpoint),
            &endpoint_size)
            == -1
        || endpoint_size != sizeof(endpoint)) {
        return std::nullopt;
    }
    port = ntohs(endpoint.sin_port);
    return socket_descriptor;
}

[[nodiscard]] int child_echo(
    const int namespace_descriptor,
    const int ready_descriptor)
{
    const auto report = [&](const std::int32_t status, const std::uint16_t port) {
        const ChildReady result{status, port};
        return write_complete(ready_descriptor, &result, sizeof(result));
    };
    if (setns(namespace_descriptor, CLONE_NEWNET) == -1) {
        (void)report(1, 0);
        return 1;
    }
    if (!has_ipv4_interface("lo", "127.0.0.1", "255.0.0.0")
        || !has_ipv4_interface("nll-app", "10.200.0.2", "255.255.255.252")) {
        (void)report(2, 0);
        return 2;
    }
    if (!has_expected_default_route()) {
        (void)report(3, 0);
        return 3;
    }

    std::uint16_t port{};
    auto socket_descriptor{bind_udp("10.200.0.2", port)};
    if (!socket_descriptor || !report(0, port)) {
        return 4;
    }
    if (!wait_readable(socket_descriptor->get())) {
        return 5;
    }
    std::array<char, 128> packet{};
    sockaddr_in sender{};
    socklen_t sender_size{sizeof(sender)};
    const ssize_t received{recvfrom(
        socket_descriptor->get(),
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<sockaddr*>(&sender),
        &sender_size)};
    if (received != static_cast<ssize_t>(packet_payload.size())) {
        return 6;
    }
    const ssize_t sent{sendto(
        socket_descriptor->get(),
        packet.data(),
        static_cast<std::size_t>(received),
        0,
        reinterpret_cast<const sockaddr*>(&sender),
        sender_size)};
    return sent == received ? 0 : 7;
}

[[nodiscard]] std::string exchange_local_packet(
    const int namespace_descriptor)
{
    if (!has_ipv4_interface("nll-host", "10.200.0.1", "255.255.255.252")) {
        return "host address or link state is incorrect";
    }

    std::uint16_t host_port{};
    auto host_socket{bind_udp("10.200.0.1", host_port)};
    if (!host_socket || host_port == 0) {
        return "failed to bind host UDP endpoint";
    }

    int ready_pipe[2]{};
    if (pipe2(ready_pipe, O_CLOEXEC) == -1) {
        return "failed to create child readiness pipe";
    }
    netlaglab::FileDescriptor ready_reader{ready_pipe[0]};
    netlaglab::FileDescriptor ready_writer{ready_pipe[1]};
    const pid_t child{fork()};
    if (child == -1) {
        return "failed to fork UDP echo child";
    }
    if (child == 0) {
        ready_reader.reset();
        const int result{child_echo(namespace_descriptor, ready_writer.get())};
        _exit(result);
    }
    ChildGuard child_guard{child};
    ready_writer.reset();

    ChildReady ready{};
    if (!wait_readable(ready_reader.get())
        || !read_complete(ready_reader.get(), &ready, sizeof(ready))) {
        return "UDP echo child did not become ready";
    }
    if (ready.status != 0 || ready.port == 0) {
        return "namespace verification failed in UDP echo child: "
            + std::to_string(ready.status);
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(ready.port);
    if (inet_pton(AF_INET, "10.200.0.2", &destination.sin_addr) != 1) {
        return "failed to construct namespace UDP endpoint";
    }
    const ssize_t sent{sendto(
        host_socket->get(),
        packet_payload.data(),
        packet_payload.size(),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination))};
    if (sent != static_cast<ssize_t>(packet_payload.size())) {
        return "failed to send local UDP packet";
    }
    if (!wait_readable(host_socket->get())) {
        return "timed out waiting for local UDP echo";
    }
    std::array<char, 128> reply{};
    const ssize_t received{
        recv(host_socket->get(), reply.data(), reply.size(), 0)};
    if (received != static_cast<ssize_t>(packet_payload.size())
        || std::string_view{reply.data(), static_cast<std::size_t>(received)}
            != packet_payload) {
        return "local UDP echo payload did not match";
    }

    const int child_status{child_guard.reap()};
    if (child_status == -1 || !WIFEXITED(child_status)
        || WEXITSTATUS(child_status) != 0) {
        return "UDP echo child did not exit successfully";
    }
    return {};
}

TEST(NetworkEnvironmentPrivilegedSmoke, PreparesExchangesPacketAndCleansUp)
{
    PreparationResult preparation{prepare_network_environment()};
    ASSERT_TRUE(std::holds_alternative<PreparedNetworkEnvironment>(preparation));
    PreparedNetworkEnvironment environment{
        std::move(std::get<PreparedNetworkEnvironment>(preparation))};

    const int namespace_descriptor{
        testing::borrow_prepared_namespace_descriptor(environment)};
    ASSERT_GE(namespace_descriptor, 0);
    const std::string packet_error{
        exchange_local_packet(namespace_descriptor)};

    CleanupResult cleanup{std::move(environment).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_EQ(access("/run/netns/netlaglab", F_OK), -1);
    EXPECT_EQ(if_nametoindex("nll-host"), 0U);
    EXPECT_EQ(if_nametoindex("nll-app"), 0U);
    EXPECT_TRUE(packet_error.empty()) << packet_error;
}

} // namespace
} // namespace netlaglab::network_environment

int main(int argc, char** argv)
{
    if (geteuid() != 0) {
        std::cerr << "NetLagLab privileged smoke requires root and never invokes sudo.\n";
        return EXIT_FAILURE;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
