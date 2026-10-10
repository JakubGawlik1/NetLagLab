#include "file_descriptor.hpp"
#include "network_environment/command_runner.hpp"
#include "network_environment/network_environment.hpp"
#include "network_environment/production_test_support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <net/route.h>
#include <optional>
#include <poll.h>
#include <sched.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace netlaglab::network_environment {
namespace {

constexpr int io_timeout_milliseconds{5'000};
constexpr std::string_view packet_payload{"netlaglab-smoke"};
constexpr std::string_view nat_payload{"netlaglab-nat-smoke"};

[[nodiscard]] std::string trusted_ip_path()
{
    constexpr std::array<std::string_view, 2> paths{
        "/usr/sbin/ip", "/usr/bin/ip"};
    for (const std::string_view path : paths) {
        struct stat metadata{};
        if (stat(path.data(), &metadata) == 0 && S_ISREG(metadata.st_mode)
            && metadata.st_uid == 0 && (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0
            && access(path.data(), X_OK) == 0) {
            return std::string{path};
        }
    }
    return {};
}

[[nodiscard]] CommandResult run_ip_result(
    const std::string_view executable,
    const std::vector<std::string>& arguments)
{
    return run_command(
        executable,
        arguments,
        std::chrono::steady_clock::now()
            + std::chrono::seconds{io_timeout_milliseconds / 1000});
}

[[nodiscard]] bool run_ip_command(
    const std::string_view executable,
    const std::vector<std::string>& arguments)
{
    return run_ip_result(executable, arguments).kind == CommandResultKind::success;
}

class TemporaryExternalPeer {
public:
    TemporaryExternalPeer()
    {
        std::uint32_t suffix{};
        if (getrandom(&suffix, sizeof(suffix), GRND_NONBLOCK)
            != static_cast<ssize_t>(sizeof(suffix))) {
            error_ = "could not generate temporary external peer names";
            return;
        }
        std::ostringstream name;
        name << std::hex << suffix;
        const std::string token{name.str()};
        namespace_name_ = "nll-peer-" + token;
        host_link_ = "nllh-" + token;
        peer_link_ = "nllp-" + token;

        const std::string ip{trusted_ip_path()};
        if (ip.empty()) {
            error_ = "trusted iproute2 executable is unavailable";
            return;
        }
        namespace_file_ = "/run/netns/" + namespace_name_;
        struct stat existing_namespace{};
        if (lstat(namespace_file_.c_str(), &existing_namespace) == 0
            || errno != ENOENT) {
            error_ = "temporary external namespace name is unavailable";
            return;
        }
        const CommandResult namespace_creation{
            run_ip_result(ip, {"netns", "add", namespace_name_})};
        if (namespace_creation.kind != CommandResultKind::success) {
            error_ = "could not create temporary external network namespace";
            if (lstat(namespace_file_.c_str(), &existing_namespace) == 0
                || errno != ENOENT) {
                namespace_creation_unconfirmed_ = true;
                append_cleanup_failure(cleanup());
            }
            return;
        }
        namespace_created_ = true;
        if (if_nametoindex(host_link_.c_str()) != 0
            || if_nametoindex(peer_link_.c_str()) != 0) {
            error_ = "temporary external veth name is already in use";
            append_cleanup_failure(cleanup());
            return;
        }
        const CommandResult veth_creation{run_ip_result(
                ip,
                {"link", "add", host_link_, "type", "veth", "peer", "name", peer_link_})};
        if (veth_creation.kind != CommandResultKind::success) {
            error_ = "could not create temporary external veth pair";
            if (if_nametoindex(host_link_.c_str()) != 0
                || if_nametoindex(peer_link_.c_str()) != 0) {
                veth_creation_unconfirmed_ = true;
            }
            append_cleanup_failure(cleanup());
            return;
        }
        host_veth_created_ = true;
        host_link_index_ = if_nametoindex(host_link_.c_str());
        if (host_link_index_ == 0) {
            error_ = "could not identify temporary external veth after creation";
            append_cleanup_failure(cleanup());
            return;
        }
        if (!run_ip_command(
                ip, {"link", "set", "dev", peer_link_, "netns", namespace_name_})
            || !run_ip_command(
                ip, {"address", "add", "198.18.0.1/30", "dev", host_link_})
            || !run_ip_command(ip, {"link", "set", "dev", host_link_, "up"})
            || !run_ip_command(ip, {"-n", namespace_name_, "link", "set", "lo", "up"})
            || !run_ip_command(
                ip,
                {"-n", namespace_name_, "address", "add", "198.18.0.2/30", "dev", peer_link_})
            || !run_ip_command(
                ip, {"-n", namespace_name_, "link", "set", "dev", peer_link_, "up"})) {
            error_ = "could not configure temporary external veth peer";
            append_cleanup_failure(cleanup());
            return;
        }
        const int descriptor{open(namespace_file_.c_str(), O_RDONLY | O_CLOEXEC)};
        if (descriptor == -1) {
            error_ = "could not open temporary external namespace";
            append_cleanup_failure(cleanup());
            return;
        }
        namespace_descriptor_ = netlaglab::FileDescriptor{descriptor};
        ready_ = true;
    }

    TemporaryExternalPeer(const TemporaryExternalPeer&) = delete;
    TemporaryExternalPeer& operator=(const TemporaryExternalPeer&) = delete;

    ~TemporaryExternalPeer()
    {
        const std::string cleanup_error{cleanup()};
        if (!cleanup_error.empty()) {
            std::cerr << "Temporary external peer cleanup failed: "
                      << cleanup_error << '\n';
        }
    }

    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] int namespace_descriptor() const
    {
        return namespace_descriptor_.get();
    }
    [[nodiscard]] std::string cleanup()
    {
        std::vector<std::string> failures;
        const std::string ip{trusted_ip_path()};
        namespace_descriptor_.reset();
        if (veth_creation_unconfirmed_) {
            if (if_nametoindex(host_link_.c_str()) == 0
                && if_nametoindex(peer_link_.c_str()) == 0) {
                veth_creation_unconfirmed_ = false;
            } else {
                failures.emplace_back(
                    "temporary external veth creation outcome is unconfirmed");
            }
        }
        if (namespace_creation_unconfirmed_) {
            struct stat metadata{};
            if (lstat(namespace_file_.c_str(), &metadata) == -1
                && errno == ENOENT) {
                namespace_creation_unconfirmed_ = false;
            } else {
                failures.emplace_back(
                    "temporary external namespace creation outcome is unconfirmed");
            }
        }
        if (host_veth_created_) {
            const unsigned int current_index{if_nametoindex(host_link_.c_str())};
            if (current_index != host_link_index_) {
                failures.emplace_back(
                    "temporary external veth identity changed; refusing deletion by name");
                host_veth_created_ = false;
            } else if (ip.empty()
                       || !run_ip_command(ip, {"link", "delete", "dev", host_link_})) {
                failures.emplace_back("could not delete temporary external veth");
            } else {
                host_veth_created_ = false;
            }
        }
        if (namespace_created_) {
            if (ip.empty()
                || !run_ip_command(ip, {"netns", "delete", namespace_name_})) {
                failures.emplace_back(
                    "could not delete temporary external network namespace");
            } else {
                namespace_created_ = false;
            }
        }
        if (!namespace_created_ && !host_veth_created_
            && !namespace_creation_unconfirmed_ && !veth_creation_unconfirmed_) {
            ready_ = false;
        }
        std::string result;
        for (const std::string& failure : failures) {
            if (!result.empty()) {
                result.append("; ");
            }
            result.append(failure);
        }
        return result;
    }

private:
    void append_cleanup_failure(std::string failure)
    {
        if (!failure.empty()) {
            error_.append("; ");
            error_.append(failure);
        }
    }

    std::string namespace_name_;
    std::string host_link_;
    std::string peer_link_;
    std::string namespace_file_;
    std::string error_;
    netlaglab::FileDescriptor namespace_descriptor_{-1};
    unsigned int host_link_index_{};
    bool namespace_created_{};
    bool host_veth_created_{};
    bool namespace_creation_unconfirmed_{};
    bool veth_creation_unconfirmed_{};
    bool ready_{};
};

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

struct NatPeerReady {
    std::int32_t status;
    std::uint16_t tcp_port;
    std::uint16_t udp_port;
};

[[nodiscard]] std::optional<netlaglab::FileDescriptor> bind_tcp(
    const char* address,
    std::uint16_t& port)
{
    netlaglab::FileDescriptor socket_descriptor{
        socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (socket_descriptor.get() == -1) {
        return std::nullopt;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1
        || bind(
               socket_descriptor.get(),
               reinterpret_cast<const sockaddr*>(&endpoint),
               sizeof(endpoint))
            == -1
        || listen(socket_descriptor.get(), 1) == -1) {
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

[[nodiscard]] bool same_address(const sockaddr_in& address, const char* text)
{
    in_addr expected{};
    return inet_pton(AF_INET, text, &expected) == 1
        && address.sin_addr.s_addr == expected.s_addr;
}

[[nodiscard]] bool send_complete(
    const int descriptor,
    const void* data,
    const std::size_t size)
{
    const auto* bytes{static_cast<const std::byte*>(data)};
    std::size_t sent{};
    while (sent < size) {
        const ssize_t result{send(
            descriptor, bytes + sent, size - sent, MSG_NOSIGNAL)};
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int child_nat_echo_server(
    const int namespace_descriptor,
    const int ready_descriptor)
{
    const auto report = [&](const std::int32_t status,
                            const std::uint16_t tcp_port,
                            const std::uint16_t udp_port) {
        const NatPeerReady result{status, tcp_port, udp_port};
        return write_complete(ready_descriptor, &result, sizeof(result));
    };
    if (setns(namespace_descriptor, CLONE_NEWNET) == -1) {
        (void)report(1, 0, 0);
        return 1;
    }
    std::uint16_t tcp_port{};
    std::uint16_t udp_port{};
    auto tcp_listener{bind_tcp("198.18.0.2", tcp_port)};
    auto udp_socket{bind_udp("198.18.0.2", udp_port)};
    if (!tcp_listener || !udp_socket || !report(0, tcp_port, udp_port)) {
        return 2;
    }

    if (!wait_readable(tcp_listener->get())) {
        return 3;
    }
    sockaddr_in tcp_peer{};
    socklen_t tcp_peer_size{sizeof(tcp_peer)};
    netlaglab::FileDescriptor tcp_connection{accept4(
        tcp_listener->get(),
        reinterpret_cast<sockaddr*>(&tcp_peer),
        &tcp_peer_size,
        SOCK_CLOEXEC)};
    std::array<char, nat_payload.size()> tcp_payload{};
    if (tcp_connection.get() == -1 || !same_address(tcp_peer, "198.18.0.1")
        || !read_complete(
            tcp_connection.get(), tcp_payload.data(), tcp_payload.size())
        || std::string_view{tcp_payload.data(), tcp_payload.size()}
            != nat_payload
        || !send_complete(
            tcp_connection.get(), tcp_payload.data(), tcp_payload.size())) {
        return 4;
    }

    if (!wait_readable(udp_socket->get())) {
        return 5;
    }
    std::array<char, 128> udp_payload{};
    sockaddr_in udp_peer{};
    socklen_t udp_peer_size{sizeof(udp_peer)};
    const ssize_t received{recvfrom(
        udp_socket->get(),
        udp_payload.data(),
        udp_payload.size(),
        0,
        reinterpret_cast<sockaddr*>(&udp_peer),
        &udp_peer_size)};
    if (received != static_cast<ssize_t>(nat_payload.size())
        || !same_address(udp_peer, "198.18.0.1")
        || std::string_view{
               udp_payload.data(), static_cast<std::size_t>(received)}
            != nat_payload
        || sendto(
               udp_socket->get(),
               udp_payload.data(),
               static_cast<std::size_t>(received),
               0,
               reinterpret_cast<const sockaddr*>(&udp_peer),
               udp_peer_size)
            != received) {
        return 6;
    }
    return 0;
}

[[nodiscard]] bool connect_with_timeout(
    const int descriptor,
    const sockaddr* address,
    const socklen_t address_size)
{
    const int result{connect(descriptor, address, address_size)};
    if (result == 0) {
        return true;
    }
    if (errno != EINPROGRESS) {
        return false;
    }
    pollfd event{descriptor, POLLOUT, 0};
    int poll_result{};
    do {
        poll_result = poll(&event, 1, io_timeout_milliseconds);
    } while (poll_result == -1 && errno == EINTR);
    if (poll_result <= 0 || (event.revents & POLLOUT) == 0) {
        return false;
    }
    int socket_error{};
    socklen_t error_size{sizeof(socket_error)};
    return getsockopt(
               descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_size)
            == 0
        && error_size == sizeof(socket_error) && socket_error == 0;
}

[[nodiscard]] int child_nat_client(
    const int namespace_descriptor,
    const NatPeerReady peer)
{
    if (setns(namespace_descriptor, CLONE_NEWNET) == -1) {
        return 1;
    }
    sockaddr_in tcp_endpoint{};
    tcp_endpoint.sin_family = AF_INET;
    tcp_endpoint.sin_port = htons(peer.tcp_port);
    if (inet_pton(AF_INET, "198.18.0.2", &tcp_endpoint.sin_addr) != 1) {
        return 2;
    }
    netlaglab::FileDescriptor tcp_socket{
        socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (tcp_socket.get() == -1
        || !connect_with_timeout(
            tcp_socket.get(),
            reinterpret_cast<const sockaddr*>(&tcp_endpoint),
            sizeof(tcp_endpoint))
        || !send_complete(
            tcp_socket.get(), nat_payload.data(), nat_payload.size())) {
        return 3;
    }
    pollfd tcp_event{tcp_socket.get(), POLLIN, 0};
    int tcp_poll{};
    do {
        tcp_poll = poll(&tcp_event, 1, io_timeout_milliseconds);
    } while (tcp_poll == -1 && errno == EINTR);
    std::array<char, nat_payload.size()> tcp_reply{};
    if (tcp_poll <= 0 || (tcp_event.revents & POLLIN) == 0
        || !read_complete(
            tcp_socket.get(), tcp_reply.data(), tcp_reply.size())
        || std::string_view{tcp_reply.data(), tcp_reply.size()} != nat_payload) {
        return 4;
    }

    netlaglab::FileDescriptor udp_socket{
        socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    sockaddr_in udp_endpoint{};
    udp_endpoint.sin_family = AF_INET;
    udp_endpoint.sin_port = htons(peer.udp_port);
    if (udp_socket.get() == -1
        || inet_pton(AF_INET, "198.18.0.2", &udp_endpoint.sin_addr) != 1
        || sendto(
               udp_socket.get(),
               nat_payload.data(),
               nat_payload.size(),
               0,
               reinterpret_cast<const sockaddr*>(&udp_endpoint),
               sizeof(udp_endpoint))
            != static_cast<ssize_t>(nat_payload.size())
        || !wait_readable(udp_socket.get())) {
        return 5;
    }
    std::array<char, 128> udp_reply{};
    sockaddr_in udp_source{};
    socklen_t udp_source_size{sizeof(udp_source)};
    const ssize_t udp_received{recvfrom(
        udp_socket.get(),
        udp_reply.data(),
        udp_reply.size(),
        0,
        reinterpret_cast<sockaddr*>(&udp_source),
        &udp_source_size)};
    return udp_received == static_cast<ssize_t>(nat_payload.size())
            && same_address(udp_source, "198.18.0.2")
            && std::string_view{
                   udp_reply.data(), static_cast<std::size_t>(udp_received)}
                == nat_payload
        ? 0
        : 6;
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

TEST(NetworkEnvironmentPrivilegedSmoke, ForwardsTcpAndUdpThroughOwnedNat)
{
    TemporaryExternalPeer external_peer;
    ASSERT_TRUE(external_peer.ready()) << external_peer.error();

    int server_ready_pipe[2]{};
    ASSERT_EQ(pipe2(server_ready_pipe, O_CLOEXEC), 0);
    netlaglab::FileDescriptor server_ready_reader{server_ready_pipe[0]};
    netlaglab::FileDescriptor server_ready_writer{server_ready_pipe[1]};
    const pid_t server{fork()};
    ASSERT_NE(server, -1);
    if (server == 0) {
        server_ready_reader.reset();
        const int result{child_nat_echo_server(
            external_peer.namespace_descriptor(), server_ready_writer.get())};
        _exit(result);
    }
    ChildGuard server_guard{server};
    server_ready_writer.reset();

    NatPeerReady peer{};
    ASSERT_TRUE(wait_readable(server_ready_reader.get()));
    ASSERT_TRUE(read_complete(
        server_ready_reader.get(), &peer, sizeof(peer)));
    ASSERT_EQ(peer.status, 0);
    ASSERT_NE(peer.tcp_port, 0U);
    ASSERT_NE(peer.udp_port, 0U);

    PreparationResult preparation{prepare_network_environment()};
    ASSERT_TRUE(std::holds_alternative<PreparedNetworkEnvironment>(preparation));
    PreparedNetworkEnvironment environment{
        std::move(std::get<PreparedNetworkEnvironment>(preparation))};
    const int namespace_descriptor{
        testing::borrow_prepared_namespace_descriptor(environment)};
    ASSERT_GE(namespace_descriptor, 0);

    const pid_t client{fork()};
    ASSERT_NE(client, -1);
    if (client == 0) {
        _exit(child_nat_client(namespace_descriptor, peer));
    }
    ChildGuard client_guard{client};
    const int client_status{client_guard.reap()};
    EXPECT_NE(client_status, -1);
    EXPECT_TRUE(WIFEXITED(client_status));
    EXPECT_EQ(WEXITSTATUS(client_status), 0);

    const int server_status{server_guard.reap()};
    EXPECT_NE(server_status, -1);
    EXPECT_TRUE(WIFEXITED(server_status));
    EXPECT_EQ(WEXITSTATUS(server_status), 0);

    CleanupResult cleanup{std::move(environment).cleanup()};
    EXPECT_TRUE(cleanup.failures.empty());
    EXPECT_FALSE(cleanup.residual.has_value());
    EXPECT_EQ(access("/run/netns/netlaglab", F_OK), -1);
    EXPECT_EQ(if_nametoindex("nll-host"), 0U);
    EXPECT_EQ(if_nametoindex("nll-app"), 0U);
    EXPECT_TRUE(external_peer.cleanup().empty()) << "external peer cleanup failed";
}

} // namespace
} // namespace netlaglab::network_environment

int main(int argc, char** argv)
{
    if (geteuid() != 0
        || std::getenv("NETLAGLAB_ALLOW_PRIVILEGED_TESTS") == nullptr
        || std::string_view{std::getenv("NETLAGLAB_ALLOW_PRIVILEGED_TESTS")} != "1") {
        std::cerr << "Set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 and run as root to enable "
                     "the host-network mutating smoke tests.\n";
        return 77;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
