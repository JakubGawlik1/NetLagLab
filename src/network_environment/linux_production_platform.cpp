#include "production_adapter.hpp"

#include "file_descriptor.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <memory>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace netlaglab::network_environment::detail {
namespace {

constexpr const char* namespace_path{"/run/netns/netlaglab"};

class LinuxNamespaceHandle final : public NamespaceHandle {
public:
    LinuxNamespaceHandle(
        netlaglab::FileDescriptor descriptor,
        const NamespaceIdentity identity)
        : descriptor_{std::move(descriptor)}
        , identity_{identity}
    {
    }

    [[nodiscard]] NamespaceIdentity identity() const override
    {
        return identity_;
    }

    [[nodiscard]] int descriptor() const { return descriptor_.get(); }

private:
    netlaglab::FileDescriptor descriptor_;
    NamespaceIdentity identity_;
};

[[nodiscard]] const LinuxNamespaceHandle* linux_handle(
    const NamespaceHandle& handle)
{
    return dynamic_cast<const LinuxNamespaceHandle*>(&handle);
}

[[nodiscard]] int poll_timeout(
    const std::chrono::steady_clock::time_point deadline)
{
    const auto remaining{deadline - std::chrono::steady_clock::now()};
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto milliseconds{
        std::chrono::ceil<std::chrono::milliseconds>(remaining)};
    return milliseconds.count() > INT_MAX
        ? INT_MAX
        : static_cast<int>(milliseconds.count());
}

[[nodiscard]] LinkQuery query_current_namespace_link(
    const std::string_view requested_name,
    const std::chrono::steady_clock::time_point deadline)
{
    if (std::chrono::steady_clock::now() >= deadline) {
        return {InventoryStatus::timeout, std::nullopt};
    }
    const std::string name{requested_name};
    errno = 0;
    const unsigned int index{if_nametoindex(name.c_str())};
    if (index == 0) {
        return {
            errno == 0 || errno == ENODEV || errno == ENXIO
                ? InventoryStatus::absent
                : InventoryStatus::failure,
            std::nullopt,
        };
    }

    const int raw_socket{socket(
        AF_NETLINK,
        SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
        NETLINK_ROUTE)};
    if (raw_socket == -1) {
        return {InventoryStatus::failure, std::nullopt};
    }
    netlaglab::FileDescriptor socket_descriptor{raw_socket};
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(
            socket_descriptor.get(),
            reinterpret_cast<const sockaddr*>(&local),
            sizeof(local))
        == -1) {
        return {InventoryStatus::failure, std::nullopt};
    }
    socklen_t local_length{sizeof(local)};
    if (getsockname(
            socket_descriptor.get(),
            reinterpret_cast<sockaddr*>(&local),
            &local_length)
            == -1
        || local_length != sizeof(local) || local.nl_family != AF_NETLINK) {
        return {InventoryStatus::failure, std::nullopt};
    }

    constexpr std::uint32_t sequence{1};
    struct Request {
        nlmsghdr header;
        ifinfomsg information;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.header.nlmsg_seq = sequence;
    request.header.nlmsg_pid = local.nl_pid;
    request.information.ifi_family = AF_UNSPEC;
    request.information.ifi_index = static_cast<int>(index);
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
        return {InventoryStatus::failure, std::nullopt};
    }

    LinkMessageDecoder decoder{sequence, local.nl_pid, index, name};
    std::array<std::byte, 65536> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{socket_descriptor.get(), POLLIN, 0};
        int poll_result{};
        do {
            poll_result = poll(&descriptor, 1, poll_timeout(deadline));
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result == 0) {
            return {InventoryStatus::timeout, std::nullopt};
        }
        if (poll_result == -1
            || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0
            || (descriptor.revents & POLLIN) == 0) {
            return {InventoryStatus::failure, std::nullopt};
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
            received = recvmsg(socket_descriptor.get(), &message, 0);
        } while (received == -1 && errno == EINTR);
        if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0
            || message.msg_namelen != sizeof(sender)
            || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) {
            return {InventoryStatus::malformed, std::nullopt};
        }
        return decoder.consume(
            std::span{buffer}.first(static_cast<std::size_t>(received)));
    }
    return {InventoryStatus::timeout, std::nullopt};
}

struct LinkQueryWire {
    std::int32_t status;
    std::uint32_t index;
    std::uint32_t peer_index;
    std::int32_t peer_namespace_id;
    std::uint8_t has_identity;
    std::uint8_t has_peer_namespace_id;
};

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

[[nodiscard]] bool reap(const pid_t child, int& status)
{
    pid_t result{};
    do {
        result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    return result == child;
}

[[nodiscard]] LinkQuery decode_wire(const LinkQueryWire& wire)
{
    if (wire.status < static_cast<std::int32_t>(InventoryStatus::absent)
        || wire.status > static_cast<std::int32_t>(InventoryStatus::malformed)
        || wire.has_identity > 1 || wire.has_peer_namespace_id > 1) {
        return {InventoryStatus::malformed, std::nullopt};
    }
    const auto status{static_cast<InventoryStatus>(wire.status)};
    if (wire.has_identity == 0) {
        return {status, std::nullopt};
    }
    if (status != InventoryStatus::present || wire.index == 0
        || wire.peer_index == 0) {
        return {InventoryStatus::malformed, std::nullopt};
    }
    return {
        status,
        LinkIdentity{
            wire.index,
            wire.peer_index,
            wire.has_peer_namespace_id != 0
                ? std::optional<std::int32_t>{wire.peer_namespace_id}
                : std::nullopt,
        },
    };
}

class LinuxProductionPlatform final : public ProductionPlatform {
public:
    [[nodiscard]] CommandResult run_ip(
        const std::string_view executable_path,
        const std::span<const std::string> arguments,
        const NamespaceHandle* inherited_namespace,
        const std::chrono::steady_clock::time_point deadline) override
    {
        if (inherited_namespace == nullptr) {
            return run_command(executable_path, arguments, deadline);
        }
        const LinuxNamespaceHandle* handle{linux_handle(*inherited_namespace)};
        if (handle == nullptr) {
            return {CommandResultKind::system_failure, EINVAL, {}};
        }
        return run_command_with_inherited_descriptor(
            executable_path, arguments, handle->descriptor(), deadline);
    }

    [[nodiscard]] NamespaceQuery query_namespace() override
    {
        const int raw_descriptor{
            open(namespace_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        if (raw_descriptor == -1) {
            return {
                errno == ENOENT || errno == ENOTDIR
                    ? InventoryStatus::absent
                    : InventoryStatus::failure,
                nullptr,
            };
        }
        netlaglab::FileDescriptor descriptor{raw_descriptor};
        struct stat status {};
        struct statfs filesystem {};
        if (fstat(descriptor.get(), &status) == -1
            || fstatfs(descriptor.get(), &filesystem) == -1
            || static_cast<unsigned long>(filesystem.f_type) != NSFS_MAGIC) {
            return {InventoryStatus::failure, nullptr};
        }
        return {
            InventoryStatus::present,
            std::make_unique<LinuxNamespaceHandle>(
                std::move(descriptor),
                NamespaceIdentity{
                    static_cast<std::uint64_t>(status.st_dev),
                    static_cast<std::uint64_t>(status.st_ino),
                }),
        };
    }

    [[nodiscard]] std::string namespace_file_argument(
        const NamespaceHandle& namespace_handle) const override
    {
        const LinuxNamespaceHandle* handle{linux_handle(namespace_handle)};
        return handle == nullptr
            ? std::string{}
            : "/proc/self/fd/" + std::to_string(handle->descriptor());
    }

    [[nodiscard]] LinkQuery query_host_link(
        const std::string_view name,
        const std::chrono::steady_clock::time_point deadline) override
    {
        return query_current_namespace_link(name, deadline);
    }

    [[nodiscard]] LinkQuery query_namespace_link(
        const NamespaceHandle& namespace_handle,
        const std::string_view name,
        const std::chrono::steady_clock::time_point deadline) override
    {
        const LinuxNamespaceHandle* handle{linux_handle(namespace_handle)};
        if (handle == nullptr
            || std::chrono::steady_clock::now() >= deadline) {
            return {
                handle == nullptr ? InventoryStatus::failure
                                  : InventoryStatus::timeout,
                std::nullopt,
            };
        }
        int pipe_descriptors[2]{};
        if (pipe2(pipe_descriptors, O_CLOEXEC) == -1) {
            return {InventoryStatus::failure, std::nullopt};
        }
        netlaglab::FileDescriptor reader{pipe_descriptors[0]};
        netlaglab::FileDescriptor writer{pipe_descriptors[1]};
        const pid_t child{fork()};
        if (child == -1) {
            return {InventoryStatus::failure, std::nullopt};
        }
        if (child == 0) {
            reader.reset();
            if (setns(handle->descriptor(), CLONE_NEWNET) == -1) {
                _exit(125);
            }
            const LinkQuery query{query_current_namespace_link(name, deadline)};
            LinkQueryWire wire{
                static_cast<std::int32_t>(query.status),
                0,
                0,
                0,
                static_cast<std::uint8_t>(query.identity.has_value()),
                0,
            };
            if (query.identity) {
                wire.index = query.identity->index;
                wire.peer_index = query.identity->peer_index;
                wire.has_peer_namespace_id = static_cast<std::uint8_t>(
                    query.identity->peer_namespace_id.has_value());
                wire.peer_namespace_id =
                    query.identity->peer_namespace_id.value_or(0);
            }
            const bool written{
                write_complete(writer.get(), &wire, sizeof(wire))};
            _exit(written ? 0 : 125);
        }

        writer.reset();
        pollfd descriptor{reader.get(), POLLIN, 0};
        int poll_result{};
        do {
            poll_result = poll(&descriptor, 1, poll_timeout(deadline));
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
            (void)kill(child, SIGKILL);
            int status{};
            (void)reap(child, status);
            return {
                poll_result == 0 ? InventoryStatus::timeout
                                 : InventoryStatus::failure,
                std::nullopt,
            };
        }
        LinkQueryWire wire{};
        const bool read{read_complete(reader.get(), &wire, sizeof(wire))};
        int status{};
        if (!reap(child, status) || !read || !WIFEXITED(status)
            || WEXITSTATUS(status) != 0) {
            return {InventoryStatus::failure, std::nullopt};
        }
        return decode_wire(wire);
    }
};

} // namespace

std::unique_ptr<ProductionPlatform> make_linux_production_platform()
{
    return std::make_unique<LinuxProductionPlatform>();
}

} // namespace netlaglab::network_environment::detail
