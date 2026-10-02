#include "preflight.hpp"

#include "file_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <memory>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace netlaglab::network_environment::detail {
namespace {

constexpr std::string_view runtime_directory{"/run/netlaglab"};
constexpr std::string_view lock_name{"host.lock"};
constexpr std::string_view namespace_path{"/run/netns/netlaglab"};

class FileHostLock final : public HostLock {
public:
    explicit FileHostLock(netlaglab::FileDescriptor descriptor)
        : descriptor_{std::move(descriptor)}
    {
    }

private:
    netlaglab::FileDescriptor descriptor_;
};

[[nodiscard]] bool has_mode(const struct stat& status, const mode_t mode)
{
    return (status.st_mode & 0777) == mode;
}

[[nodiscard]] QueryStatus path_status(const std::string_view path)
{
    struct stat status {};
    if (lstat(path.data(), &status) == 0) {
        return QueryStatus::present;
    }
    return errno == ENOENT || errno == ENOTDIR ? QueryStatus::absent
                                                : QueryStatus::failure;
}

[[nodiscard]] bool prefix_length(const std::uint32_t mask, std::uint8_t& length)
{
    bool found_zero{};
    length = 0;
    for (std::uint32_t bit{0x80000000U}; bit != 0; bit >>= 1U) {
        if ((mask & bit) != 0) {
            if (found_zero) {
                return false;
            }
            ++length;
        } else {
            found_zero = true;
        }
    }
    return true;
}

[[nodiscard]] int poll_timeout(
    const std::chrono::steady_clock::time_point deadline)
{
    const auto remaining{deadline - std::chrono::steady_clock::now()};
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto milliseconds{
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)};
    const auto rounded{milliseconds
            + (milliseconds < remaining ? std::chrono::milliseconds{1}
                                        : std::chrono::milliseconds{0})};
    return static_cast<int>(std::min<std::int64_t>(rounded.count(), INT_MAX));
}

class LinuxPreflightPlatform final : public PreflightPlatform {
public:
    [[nodiscard]] HostLockResult acquire_host_lock() override
    {
        const int mkdir_result{mkdir(runtime_directory.data(), 0755)};
        if (mkdir_result == -1 && errno != EEXIST) {
            return {};
        }
        const int directory_descriptor{open(
            runtime_directory.data(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (directory_descriptor == -1) {
            return {};
        }
        netlaglab::FileDescriptor directory{directory_descriptor};
        if (mkdir_result == 0 && fchmod(directory.get(), 0755) == -1) {
            return {};
        }
        struct stat directory_status {};
        if (fstat(directory.get(), &directory_status) == -1
            || !S_ISDIR(directory_status.st_mode)
            || directory_status.st_uid != 0
            || !has_mode(directory_status, 0755)) {
            return {};
        }

        bool created_lock{true};
        int lock_descriptor{openat(
            directory.get(),
            lock_name.data(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600)};
        if (lock_descriptor == -1 && errno == EEXIST) {
            created_lock = false;
            lock_descriptor = openat(
                directory.get(),
                lock_name.data(),
                O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        }
        if (lock_descriptor == -1) {
            return {};
        }
        netlaglab::FileDescriptor descriptor{lock_descriptor};
        if (created_lock && fchmod(descriptor.get(), 0600) == -1) {
            return {};
        }
        struct stat lock_status {};
        if (fstat(descriptor.get(), &lock_status) == -1
            || !S_ISREG(lock_status.st_mode) || lock_status.st_uid != 0
            || !has_mode(lock_status, 0600)) {
            return {};
        }
        if (flock(descriptor.get(), LOCK_EX | LOCK_NB) == -1) {
            const Cause cause{errno == EWOULDBLOCK || errno == EAGAIN
                    ? Cause::collision
                    : Cause::system_failure};
            return {nullptr, cause};
        }
        return {
            std::make_unique<FileHostLock>(std::move(descriptor)),
            Cause::system_failure,
        };
    }

    [[nodiscard]] bool privileged() const override { return geteuid() == 0; }

    [[nodiscard]] ToolQuery query_tool(const std::string_view path) override
    {
        struct stat status {};
        if (stat(path.data(), &status) == -1) {
            return {
                errno == ENOENT || errno == ENOTDIR ? QueryStatus::absent
                                                    : QueryStatus::failure,
                {},
            };
        }
        return {
            QueryStatus::present,
            {
                S_ISREG(status.st_mode),
                static_cast<std::uint32_t>(status.st_uid),
                static_cast<std::uint32_t>(status.st_mode & 07777),
            },
        };
    }

    [[nodiscard]] QueryStatus query_namespace_name() override
    {
        return path_status(namespace_path);
    }

    [[nodiscard]] QueryStatus query_link_name(
        const std::string_view name) override
    {
        errno = 0;
        if (if_nametoindex(std::string{name}.c_str()) != 0) {
            return QueryStatus::present;
        }
        return errno == 0 || errno == ENXIO || errno == ENODEV
            ? QueryStatus::absent
            : QueryStatus::failure;
    }

    [[nodiscard]] AddressQuery query_addresses() override
    {
        ifaddrs* raw_addresses{};
        if (getifaddrs(&raw_addresses) == -1) {
            return {false, {}};
        }
        const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses{
            raw_addresses,
            freeifaddrs,
        };
        std::vector<Ipv4Prefix> result;
        for (const ifaddrs* address{addresses.get()}; address != nullptr;
             address = address->ifa_next) {
            if (address->ifa_addr == nullptr
                || address->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if (address->ifa_netmask == nullptr
                || address->ifa_netmask->sa_family != AF_INET) {
                return {false, {}};
            }
            const auto* ipv4{
                reinterpret_cast<const sockaddr_in*>(address->ifa_addr)};
            const auto* netmask{
                reinterpret_cast<const sockaddr_in*>(address->ifa_netmask)};
            std::uint8_t length{};
            if (!prefix_length(ntohl(netmask->sin_addr.s_addr), length)) {
                return {false, {}};
            }
            result.push_back({ntohl(ipv4->sin_addr.s_addr), length});
        }
        return {true, std::move(result)};
    }

    [[nodiscard]] RouteDumpStatus query_routes(
        const std::chrono::steady_clock::time_point deadline) override
    {
        if (std::chrono::steady_clock::now() >= deadline) {
            return RouteDumpStatus::timeout;
        }
        const int raw_socket{socket(
            AF_NETLINK,
            SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
            NETLINK_ROUTE)};
        if (raw_socket == -1) {
            return RouteDumpStatus::netlink_error;
        }
        netlaglab::FileDescriptor route_socket{raw_socket};
        sockaddr_nl local_address{};
        local_address.nl_family = AF_NETLINK;
        if (bind(
                route_socket.get(),
                reinterpret_cast<const sockaddr*>(&local_address),
                sizeof(local_address))
            == -1) {
            return RouteDumpStatus::netlink_error;
        }
        socklen_t local_length{sizeof(local_address)};
        if (getsockname(
                route_socket.get(),
                reinterpret_cast<sockaddr*>(&local_address),
                &local_length)
                == -1
            || local_length != sizeof(local_address)
            || local_address.nl_family != AF_NETLINK) {
            return RouteDumpStatus::netlink_error;
        }

        constexpr std::uint32_t sequence{1};
        struct Request {
            nlmsghdr header;
            rtmsg route;
        } request{};
        request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
        request.header.nlmsg_type = RTM_GETROUTE;
        request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        request.header.nlmsg_seq = sequence;
        request.header.nlmsg_pid = local_address.nl_pid;
        request.route.rtm_family = AF_INET;
        sockaddr_nl kernel_address{};
        kernel_address.nl_family = AF_NETLINK;
        const ssize_t sent{sendto(
            route_socket.get(),
            &request,
            request.header.nlmsg_len,
            0,
            reinterpret_cast<const sockaddr*>(&kernel_address),
            sizeof(kernel_address))};
        if (sent != static_cast<ssize_t>(request.header.nlmsg_len)) {
            return RouteDumpStatus::netlink_error;
        }

        RouteDumpDecoder decoder{sequence, local_address.nl_pid};
        std::array<std::byte, 65536> buffer{};
        while (decoder.status() == RouteDumpStatus::pending) {
            pollfd descriptor{route_socket.get(), POLLIN, 0};
            int poll_result{};
            do {
                poll_result = poll(&descriptor, 1, poll_timeout(deadline));
            } while (poll_result == -1 && errno == EINTR);
            if (poll_result == 0) {
                return RouteDumpStatus::timeout;
            }
            if (poll_result == -1
                || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0
                || (descriptor.revents & POLLIN) == 0) {
                return RouteDumpStatus::netlink_error;
            }

            sockaddr_nl sender{};
            iovec vector{buffer.data(), buffer.size()};
            msghdr message{};
            message.msg_name = &sender;
            message.msg_namelen = sizeof(sender);
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            ssize_t received{};
            do {
                received = recvmsg(route_socket.get(), &message, 0);
            } while (received == -1 && errno == EINTR);
            if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            if (received == -1) {
                return RouteDumpStatus::netlink_error;
            }
            if (received == 0 || (message.msg_flags & MSG_TRUNC) != 0
                || message.msg_namelen != sizeof(sender)
                || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) {
                return RouteDumpStatus::malformed;
            }
            (void)decoder.consume(
                std::span{buffer}.first(static_cast<std::size_t>(received)));
        }
        return decoder.status();
    }
};

} // namespace

std::unique_ptr<PreflightPlatform> make_linux_preflight_platform()
{
    return std::make_unique<LinuxPreflightPlatform>();
}

} // namespace netlaglab::network_environment::detail
