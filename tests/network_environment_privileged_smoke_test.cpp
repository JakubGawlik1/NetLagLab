#include "file_descriptor.hpp"
#include "network_environment/network_environment.hpp"
#include "network_environment/production_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <memory>
#include <net/if.h>
#include <optional>
#include <poll.h>
#include <sched.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace netlaglab::network_environment {
namespace {

constexpr int io_timeout_ms{3000};
constexpr char payload[]{"nll-udp"};

struct UdpExchangeResult {
    bool succeeded;
    std::string reason;
};

[[nodiscard]] std::string system_error(const char* operation, const int error)
{
    return std::string{operation} + ": " + std::strerror(error);
}

[[nodiscard]] bool parse_ipv4(
    const char* text,
    in_addr& address)
{
    return inet_pton(AF_INET, text, &address) == 1;
}

[[nodiscard]] bool interface_has_address(
    const char* interface_name,
    const char* address_text,
    const char* netmask_text)
{
    in_addr expected_address{};
    in_addr expected_netmask{};
    if (!parse_ipv4(address_text, expected_address)
        || !parse_ipv4(netmask_text, expected_netmask)) {
        return false;
    }

    ifaddrs* raw_addresses{};
    if (getifaddrs(&raw_addresses) == -1) {
        return false;
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses{
        raw_addresses, freeifaddrs};
    for (const ifaddrs* entry{addresses.get()}; entry != nullptr;
         entry = entry->ifa_next) {
        if (entry->ifa_name == nullptr
            || std::strcmp(entry->ifa_name, interface_name) != 0
            || entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr
            || entry->ifa_addr->sa_family != AF_INET
            || entry->ifa_netmask->sa_family != AF_INET) {
            continue;
        }
        const auto* address{
            reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)};
        const auto* netmask{
            reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)};
        if (address->sin_addr.s_addr == expected_address.s_addr
            && netmask->sin_addr.s_addr == expected_netmask.s_addr) {
            return true;
        }
    }
    return false;
}

template<typename Value>
[[nodiscard]] bool copy_object(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    Value& value)
{
    if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return true;
}

[[nodiscard]] bool default_route_uses_session_link()
{
    const unsigned int session_index{if_nametoindex("nll-app")};
    in_addr expected_gateway{};
    if (session_index == 0
        || !parse_ipv4("10.200.0.1", expected_gateway)) {
        return false;
    }

    const int raw_socket{
        socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)};
    if (raw_socket == -1) {
        return false;
    }
    netlaglab::FileDescriptor socket_descriptor{raw_socket};
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(
            socket_descriptor.get(),
            reinterpret_cast<const sockaddr*>(&local),
            sizeof(local))
        == -1) {
        return false;
    }
    socklen_t local_length{sizeof(local)};
    if (getsockname(
            socket_descriptor.get(),
            reinterpret_cast<sockaddr*>(&local),
            &local_length)
            == -1
        || local_length != sizeof(local) || local.nl_family != AF_NETLINK) {
        return false;
    }

    constexpr std::uint32_t sequence{17};
    struct Request {
        nlmsghdr header;
        rtmsg route;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = sequence;
    request.header.nlmsg_pid = local.nl_pid;
    request.route.rtm_family = AF_INET;

    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    const ssize_t sent{sendto(
        socket_descriptor.get(),
        &request,
        request.header.nlmsg_len,
        0,
        reinterpret_cast<const sockaddr*>(&kernel),
        sizeof(kernel))};
    if (sent != static_cast<ssize_t>(request.header.nlmsg_len)) {
        return false;
    }

    bool found_default{};
    bool finished{};
    const auto deadline{std::chrono::steady_clock::now()
        + std::chrono::milliseconds{io_timeout_ms}};
    std::vector<std::byte> buffer(65536);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{socket_descriptor.get(), POLLIN, 0};
        const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())};
        int poll_result{};
        do {
            poll_result = poll(
                &descriptor,
                1,
                static_cast<int>(std::max<std::int64_t>(0, remaining.count())));
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
            return false;
        }

        sockaddr_nl sender{};
        iovec vector{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_name = &sender;
        message.msg_namelen = sizeof(sender);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const ssize_t received{recvmsg(socket_descriptor.get(), &message, 0)};
        if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0
            || message.msg_namelen != sizeof(sender)
            || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) {
            return false;
        }

        const auto bytes{std::span<const std::byte>{buffer}.first(
            static_cast<std::size_t>(received))};
        std::size_t offset{};
        while (offset < bytes.size()) {
            nlmsghdr header{};
            if (!copy_object(bytes, offset, header)
                || header.nlmsg_len < sizeof(nlmsghdr)
                || header.nlmsg_len > bytes.size() - offset
                || header.nlmsg_seq != sequence
                || header.nlmsg_pid != local.nl_pid) {
                return false;
            }
            const auto message_bytes{bytes.subspan(offset, header.nlmsg_len)};
            if (header.nlmsg_type == NLMSG_DONE) {
                finished = true;
            } else if (header.nlmsg_type == NLMSG_ERROR) {
                nlmsgerr error{};
                if (!copy_object(message_bytes, NLMSG_HDRLEN, error)
                    || error.error != 0) {
                    return false;
                }
            } else if (header.nlmsg_type == RTM_NEWROUTE
                && header.nlmsg_len >= NLMSG_LENGTH(sizeof(rtmsg))) {
                rtmsg route{};
                if (!copy_object(message_bytes, NLMSG_HDRLEN, route)) {
                    return false;
                }
                if (route.rtm_family == AF_INET && route.rtm_dst_len == 0
                    && route.rtm_table == RT_TABLE_MAIN
                    && route.rtm_type == RTN_UNICAST) {
                    std::uint32_t output_index{};
                    in_addr gateway{};
                    bool has_output_index{};
                    bool has_gateway{};
                    std::size_t attribute_offset{
                        NLMSG_LENGTH(sizeof(rtmsg))};
                    while (attribute_offset < message_bytes.size()) {
                        rtattr attribute{};
                        if (!copy_object(
                                message_bytes, attribute_offset, attribute)
                            || attribute.rta_len < RTA_LENGTH(0)
                            || attribute.rta_len
                                > message_bytes.size() - attribute_offset) {
                            return false;
                        }
                        const std::size_t attribute_size{
                            attribute.rta_len - RTA_LENGTH(0)};
                        const std::size_t payload_offset{
                            attribute_offset + RTA_LENGTH(0)};
                        if (attribute.rta_type == RTA_OIF) {
                            if (has_output_index
                                || attribute_size != sizeof(output_index)
                                || !copy_object(
                                    message_bytes,
                                    payload_offset,
                                    output_index)) {
                                return false;
                            }
                            has_output_index = true;
                        } else if (attribute.rta_type == RTA_GATEWAY) {
                            if (has_gateway
                                || attribute_size != sizeof(gateway)
                                || !copy_object(
                                    message_bytes, payload_offset, gateway)) {
                                return false;
                            }
                            has_gateway = true;
                        }
                        const std::size_t aligned{
                            RTA_ALIGN(attribute.rta_len)};
                        if (aligned > message_bytes.size() - attribute_offset) {
                            return false;
                        }
                        attribute_offset += aligned;
                    }
                    found_default = found_default
                        || (has_output_index && has_gateway
                            && output_index == session_index
                            && gateway.s_addr == expected_gateway.s_addr);
                }
            }
            const std::size_t aligned{NLMSG_ALIGN(header.nlmsg_len)};
            if (aligned > bytes.size() - offset) {
                if (header.nlmsg_len != bytes.size() - offset) {
                    return false;
                }
                offset = bytes.size();
            } else {
                offset += aligned;
            }
        }
    }
    return finished && found_default;
}

[[nodiscard]] bool poll_readable(const int descriptor, const int timeout_ms)
{
    pollfd watched{descriptor, POLLIN, 0};
    int result{};
    do {
        result = poll(&watched, 1, timeout_ms);
    } while (result == -1 && errno == EINTR);
    return result > 0 && (watched.revents & POLLIN) != 0;
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
    std::size_t read{};
    while (read < size) {
        const ssize_t result{::read(descriptor, bytes + read, size - read)};
        if (result > 0) {
            read += static_cast<std::size_t>(result);
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

class ChildProcess {
public:
    explicit ChildProcess(const pid_t process) : process_{process} {}
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() { stop_and_reap(); }

    [[nodiscard]] std::optional<int> wait_for_exit(
        const std::chrono::milliseconds timeout)
    {
        const auto deadline{std::chrono::steady_clock::now() + timeout};
        while (std::chrono::steady_clock::now() < deadline) {
            int status{};
            const pid_t result{waitpid(process_, &status, WNOHANG)};
            if (result == process_) {
                process_ = -1;
                return status;
            }
            if (result == -1 && errno != EINTR) {
                return std::nullopt;
            }
            (void)poll(nullptr, 0, 10);
        }
        return std::nullopt;
    }

private:
    void stop_and_reap() noexcept
    {
        if (process_ == -1) {
            return;
        }
        (void)kill(process_, SIGKILL);
        int status{};
        while (waitpid(process_, &status, 0) == -1 && errno == EINTR) {
        }
        process_ = -1;
    }

    pid_t process_;
};

enum ChildReadyStatus : int {
    child_ready = 0,
    child_enter_namespace_failed,
    child_session_address_missing,
    child_default_route_missing,
    child_socket_failed,
    child_bind_failed,
};

[[noreturn]] void run_namespace_echo_child(
    const int namespace_descriptor,
    const int host_socket,
    const int ready_reader,
    const int ready_writer,
    const std::uint16_t port)
{
    (void)close(host_socket);
    (void)close(ready_reader);
    ChildReadyStatus status{child_ready};
    if (setns(namespace_descriptor, CLONE_NEWNET) == -1) {
        status = child_enter_namespace_failed;
    } else if (!interface_has_address(
                   "nll-app", "10.200.0.2", "255.255.255.252")) {
        status = child_session_address_missing;
    } else if (!default_route_uses_session_link()) {
        status = child_default_route_missing;
    }

    int session_socket{-1};
    if (status == child_ready) {
        session_socket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (session_socket == -1) {
            status = child_socket_failed;
        }
    }
    netlaglab::FileDescriptor socket_descriptor{session_socket};
    if (status == child_ready) {
        sockaddr_in session_address{};
        session_address.sin_family = AF_INET;
        session_address.sin_port = htons(port);
        (void)parse_ipv4("10.200.0.2", session_address.sin_addr);
        if (bind(
                socket_descriptor.get(),
                reinterpret_cast<const sockaddr*>(&session_address),
                sizeof(session_address))
            == -1) {
            status = child_bind_failed;
        }
    }

    if (!write_complete(ready_writer, &status, sizeof(status))) {
        _exit(120);
    }
    (void)close(ready_writer);
    if (status != child_ready || !poll_readable(socket_descriptor.get(), io_timeout_ms)) {
        _exit(status == child_ready ? 121 : 122);
    }

    char received[32]{};
    sockaddr_in peer{};
    socklen_t peer_length{sizeof(peer)};
    const ssize_t received_size{recvfrom(
        socket_descriptor.get(),
        received,
        sizeof(received),
        0,
        reinterpret_cast<sockaddr*>(&peer),
        &peer_length)};
    if (received_size != static_cast<ssize_t>(sizeof(payload))) {
        _exit(123);
    }
    const ssize_t sent{sendto(
        socket_descriptor.get(),
        received,
        static_cast<std::size_t>(received_size),
        0,
        reinterpret_cast<const sockaddr*>(&peer),
        peer_length)};
    _exit(sent == received_size ? 0 : 124);
}

[[nodiscard]] UdpExchangeResult exchange_bounded_datagram(
    const int namespace_descriptor)
{
    if (!interface_has_address(
            "nll-host", "10.200.0.1", "255.255.255.252")) {
        return {false, "host endpoint is missing 10.200.0.1/30"};
    }

    const int raw_socket{socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (raw_socket == -1) {
        return {false, system_error("create host UDP socket", errno)};
    }
    netlaglab::FileDescriptor host_socket{raw_socket};
    sockaddr_in host_address{};
    host_address.sin_family = AF_INET;
    (void)parse_ipv4("10.200.0.1", host_address.sin_addr);
    if (bind(
            host_socket.get(),
            reinterpret_cast<const sockaddr*>(&host_address),
            sizeof(host_address))
        == -1) {
        return {false, system_error("bind host UDP endpoint", errno)};
    }
    socklen_t host_address_length{sizeof(host_address)};
    if (getsockname(
            host_socket.get(),
            reinterpret_cast<sockaddr*>(&host_address),
            &host_address_length)
        == -1) {
        return {false, system_error("read host UDP port", errno)};
    }

    int ready_pipe[2]{};
    if (pipe2(ready_pipe, O_CLOEXEC) == -1) {
        return {false, system_error("create namespace child pipe", errno)};
    }
    netlaglab::FileDescriptor ready_reader{ready_pipe[0]};
    netlaglab::FileDescriptor ready_writer{ready_pipe[1]};
    const pid_t process{fork()};
    if (process == -1) {
        return {false, system_error("fork namespace echo child", errno)};
    }
    if (process == 0) {
        run_namespace_echo_child(
            namespace_descriptor,
            host_socket.get(),
            ready_reader.get(),
            ready_writer.get(),
            ntohs(host_address.sin_port));
    }
    ChildProcess child{process};
    ready_writer.reset();

    if (!poll_readable(ready_reader.get(), io_timeout_ms)) {
        return {false, "namespace echo child did not become ready"};
    }
    ChildReadyStatus child_status{};
    if (!read_complete(ready_reader.get(), &child_status, sizeof(child_status))) {
        return {false, "could not read namespace echo child status"};
    }
    if (child_status != child_ready) {
        return {
            false,
            "namespace echo child failed setup with status "
                + std::to_string(static_cast<int>(child_status)),
        };
    }

    sockaddr_in session_address{};
    session_address.sin_family = AF_INET;
    session_address.sin_port = host_address.sin_port;
    (void)parse_ipv4("10.200.0.2", session_address.sin_addr);
    const ssize_t sent{sendto(
        host_socket.get(),
        payload,
        sizeof(payload),
        0,
        reinterpret_cast<const sockaddr*>(&session_address),
        sizeof(session_address))};
    if (sent != static_cast<ssize_t>(sizeof(payload))) {
        return {false, system_error("send bounded UDP datagram", errno)};
    }
    if (!poll_readable(host_socket.get(), io_timeout_ms)) {
        return {false, "host did not receive the echoed UDP datagram"};
    }

    char response[32]{};
    sockaddr_in peer{};
    socklen_t peer_length{sizeof(peer)};
    const ssize_t response_size{recvfrom(
        host_socket.get(),
        response,
        sizeof(response),
        0,
        reinterpret_cast<sockaddr*>(&peer),
        &peer_length)};
    in_addr expected_peer{};
    (void)parse_ipv4("10.200.0.2", expected_peer);
    if (response_size != static_cast<ssize_t>(sizeof(payload))
        || std::memcmp(response, payload, sizeof(payload)) != 0
        || peer.sin_family != AF_INET
        || peer.sin_addr.s_addr != expected_peer.s_addr
        || peer.sin_port != host_address.sin_port) {
        return {false, "UDP echo did not match the bounded payload and peer"};
    }

    const std::optional<int> child_status_after_echo{
        child.wait_for_exit(std::chrono::milliseconds{io_timeout_ms})};
    if (!child_status_after_echo || !WIFEXITED(*child_status_after_echo)
        || WEXITSTATUS(*child_status_after_echo) != 0) {
        return {false, "namespace echo child did not exit successfully"};
    }
    return {true, {}};
}

[[nodiscard]] bool namespace_root_is_absent()
{
    struct stat status {};
    return stat("/run/netns/netlaglab", &status) == -1 && errno == ENOENT;
}

[[nodiscard]] bool veth_root_is_absent()
{
    errno = 0;
    const unsigned int host_index{if_nametoindex("nll-host")};
    if (host_index != 0) {
        return false;
    }
    errno = 0;
    return if_nametoindex("nll-app") == 0;
}

TEST(NetworkEnvironmentPrivilegedSmokeTest, ExchangesUdpAndCleansOwnedRoots)
{
    const char* authorization{std::getenv("NETLAGLAB_ALLOW_PRIVILEGED_TESTS")};
    ASSERT_NE(authorization, nullptr)
        << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 for this explicit privileged run";
    ASSERT_STREQ(authorization, "1")
        << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 for this explicit privileged run";
    ASSERT_EQ(geteuid(), 0U)
        << "run this privileged CTest directly as root; it does not invoke sudo";

    PreparationResult result{prepare_network_environment()};
    auto* environment{std::get_if<PreparedNetworkEnvironment>(&result)};
    if (environment == nullptr) {
        auto& failure{std::get<PreparationFailure>(result)};
        if (failure.residual) {
            CleanupResult cleanup{std::move(*failure.residual).retry()};
            EXPECT_TRUE(cleanup.failures.empty());
            EXPECT_FALSE(cleanup.residual.has_value());
        }
        ADD_FAILURE() << "Network Environment preparation failed at stage "
                      << static_cast<int>(failure.primary.stage)
                      << " with cause "
                      << static_cast<int>(failure.primary.cause);
        EXPECT_TRUE(namespace_root_is_absent());
        EXPECT_TRUE(veth_root_is_absent());
        return;
    }

    netlaglab::FileDescriptor namespace_descriptor{
        testing::duplicate_owned_namespace_descriptor(*environment)};
    UdpExchangeResult exchange{false, "could not duplicate owned namespace handle"};
    if (namespace_descriptor.get() != -1) {
        exchange = exchange_bounded_datagram(namespace_descriptor.get());
    }
    namespace_descriptor.reset();

    CleanupResult cleanup{std::move(*environment).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty())
        << "explicit cleanup reported " << cleanup.failures.size()
        << " failure(s)";
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_TRUE(namespace_root_is_absent());
    EXPECT_TRUE(veth_root_is_absent());
    EXPECT_TRUE(exchange.succeeded) << exchange.reason;
}

} // namespace
} // namespace netlaglab::network_environment
