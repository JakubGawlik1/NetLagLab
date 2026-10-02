#include "preflight.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <optional>

namespace netlaglab::network_environment::detail {
namespace {

constexpr Ipv4Prefix session_subnet{0x0ac80000U, 30};
constexpr std::array<std::string_view, 4> ip_paths{
    "/usr/sbin/ip",
    "/usr/bin/ip",
    "/sbin/ip",
    "/bin/ip",
};

[[nodiscard]] std::uint32_t prefix_mask(const std::uint8_t length)
{
    if (length == 0) {
        return 0;
    }
    return 0xffffffffU << (32U - length);
}

template<typename Value>
[[nodiscard]] bool copy_object(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    Value& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

[[nodiscard]] RouteDumpStatus inspect_route(
    const std::span<const std::byte> message,
    const nlmsghdr& header)
{
    if (header.nlmsg_len < NLMSG_LENGTH(sizeof(rtmsg))) {
        return RouteDumpStatus::malformed;
    }
    rtmsg route{};
    if (!copy_object(message, NLMSG_HDRLEN, route)) {
        return RouteDumpStatus::malformed;
    }
    if (route.rtm_family != AF_INET) {
        return RouteDumpStatus::pending;
    }
    if (route.rtm_dst_len > 32) {
        return RouteDumpStatus::malformed;
    }

    std::uint32_t destination{};
    bool has_destination{};
    bool has_table{};
    std::size_t offset{NLMSG_LENGTH(sizeof(rtmsg))};
    std::size_t remaining{header.nlmsg_len - offset};
    while (remaining != 0) {
        rtattr attribute{};
        if (!copy_object(message, offset, attribute)
            || attribute.rta_len < RTA_LENGTH(0)
            || attribute.rta_len > remaining) {
            return RouteDumpStatus::malformed;
        }
        const std::size_t payload_length{
            attribute.rta_len - RTA_LENGTH(0)};
        if (attribute.rta_type == RTA_DST) {
            if (has_destination || payload_length != sizeof(destination)
                || !copy_object(
                    message, offset + RTA_LENGTH(0), destination)) {
                return RouteDumpStatus::malformed;
            }
            has_destination = true;
        } else if (attribute.rta_type == RTA_TABLE) {
            std::uint32_t table{};
            if (has_table || payload_length != sizeof(table)
                || !copy_object(message, offset + RTA_LENGTH(0), table)) {
                return RouteDumpStatus::malformed;
            }
            has_table = true;
        }
        const std::size_t aligned_length{RTA_ALIGN(attribute.rta_len)};
        if (aligned_length > remaining) {
            return RouteDumpStatus::malformed;
        }
        offset += aligned_length;
        remaining -= aligned_length;
    }
    if (route.rtm_dst_len != 0 && !has_destination) {
        return RouteDumpStatus::malformed;
    }
    if (route.rtm_dst_len == 0) {
        return RouteDumpStatus::pending;
    }

    const Ipv4Prefix prefix{
        ntohl(destination),
        route.rtm_dst_len,
    };
    return prefixes_overlap(prefix, session_subnet)
        ? RouteDumpStatus::collision
        : RouteDumpStatus::pending;
}

} // namespace

bool prefixes_overlap(const Ipv4Prefix left, const Ipv4Prefix right)
{
    if (left.length > 32 || right.length > 32) {
        return false;
    }
    const std::uint8_t common_length{std::min(left.length, right.length)};
    const std::uint32_t mask{prefix_mask(common_length)};
    return (left.address & mask) == (right.address & mask);
}

bool is_trusted_executable(const FileMetadata metadata)
{
    constexpr std::uint32_t executable_bits{0111};
    constexpr std::uint32_t unsafe_write_bits{0022};
    return metadata.regular && metadata.owner == 0
        && (metadata.mode & executable_bits) != 0
        && (metadata.mode & unsafe_write_bits) == 0;
}

std::span<const std::string_view> trusted_ip_paths()
{
    return ip_paths;
}

PreflightResult run_preflight(
    PreflightPlatform& platform,
    const std::chrono::steady_clock::time_point deadline)
{
    if (!platform.privileged()) {
        return {false, Cause::system_failure, {}};
    }

    std::string selected_tool;
    for (const std::string_view path : trusted_ip_paths()) {
        const ToolQuery query{platform.query_tool(path)};
        if (query.status == QueryStatus::failure) {
            return {false, Cause::system_failure, {}};
        }
        if (query.status == QueryStatus::present
            && is_trusted_executable(query.metadata)) {
            selected_tool = path;
            break;
        }
    }
    if (selected_tool.empty()) {
        return {false, Cause::unavailable_or_invalid_tool, {}};
    }

    const auto check_name = [](const QueryStatus status) -> std::optional<Cause> {
        if (status == QueryStatus::present) {
            return Cause::collision;
        }
        if (status == QueryStatus::failure) {
            return Cause::system_failure;
        }
        return std::nullopt;
    };
    if (const auto cause{check_name(platform.query_namespace_name())}) {
        return {false, *cause, {}};
    }
    for (const std::string_view name : {"nll-host", "nll-app"}) {
        if (const auto cause{check_name(platform.query_link_name(name))}) {
            return {false, *cause, {}};
        }
    }

    const AddressQuery addresses{platform.query_addresses()};
    if (!addresses.succeeded) {
        return {false, Cause::system_failure, {}};
    }
    for (const Ipv4Prefix prefix : addresses.prefixes) {
        if (prefix.length > 32) {
            return {false, Cause::system_failure, {}};
        }
        if (prefixes_overlap(prefix, session_subnet)) {
            return {false, Cause::collision, {}};
        }
    }

    const RouteDumpStatus routes{platform.query_routes(deadline)};
    if (routes == RouteDumpStatus::collision) {
        return {false, Cause::collision, {}};
    }
    if (routes == RouteDumpStatus::timeout) {
        return {false, Cause::timeout, {}};
    }
    if (routes != RouteDumpStatus::complete) {
        return {false, Cause::system_failure, {}};
    }
    return {true, Cause::system_failure, std::move(selected_tool)};
}

RouteDumpDecoder::RouteDumpDecoder(
    const std::uint32_t sequence,
    const std::uint32_t port_id)
    : sequence_{sequence}
    , port_id_{port_id}
{
}

RouteDumpStatus RouteDumpDecoder::consume(const std::span<const std::byte> bytes)
{
    if (status_ != RouteDumpStatus::pending) {
        return status_;
    }
    if (bytes.empty()) {
        status_ = RouteDumpStatus::malformed;
        return status_;
    }

    std::size_t offset{};
    while (offset < bytes.size()) {
        if (bytes.size() - offset < sizeof(nlmsghdr)) {
            status_ = RouteDumpStatus::malformed;
            return status_;
        }
        nlmsghdr header{};
        if (!copy_object(bytes, offset, header)
            || header.nlmsg_len < sizeof(nlmsghdr)
            || header.nlmsg_len > bytes.size() - offset
            || header.nlmsg_seq != sequence_
            || header.nlmsg_pid != port_id_
            || (header.nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
            status_ = RouteDumpStatus::malformed;
            return status_;
        }
        const auto message{bytes.subspan(offset, header.nlmsg_len)};
        const std::size_t aligned_length{NLMSG_ALIGN(header.nlmsg_len)};
        if (aligned_length > bytes.size() - offset) {
            status_ = RouteDumpStatus::malformed;
            return status_;
        }

        if (header.nlmsg_type == NLMSG_DONE) {
            if ((header.nlmsg_flags & NLM_F_MULTI) == 0
                || aligned_length != bytes.size() - offset) {
                status_ = RouteDumpStatus::malformed;
                return status_;
            }
            if (header.nlmsg_len != NLMSG_LENGTH(0)) {
                if (header.nlmsg_len < NLMSG_LENGTH(sizeof(int))) {
                    status_ = RouteDumpStatus::malformed;
                    return status_;
                }
                int error{};
                if (!copy_object(message, NLMSG_HDRLEN, error)) {
                    status_ = RouteDumpStatus::malformed;
                    return status_;
                }
                if (error != 0) {
                    error_number_ = error < 0 ? -error : error;
                    status_ = RouteDumpStatus::netlink_error;
                    return status_;
                }
            }
            status_ = RouteDumpStatus::complete;
            return status_;
        }
        if (header.nlmsg_type == NLMSG_ERROR) {
            if (aligned_length != bytes.size() - offset
                || header.nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
                status_ = RouteDumpStatus::malformed;
                return status_;
            }
            nlmsgerr error{};
            if (!copy_object(message, NLMSG_HDRLEN, error)) {
                status_ = RouteDumpStatus::malformed;
                return status_;
            }
            if (error.error == 0) {
                status_ = RouteDumpStatus::malformed;
            } else {
                error_number_ = error.error < 0 ? -error.error : error.error;
                status_ = RouteDumpStatus::netlink_error;
            }
            return status_;
        }
        if (header.nlmsg_type != RTM_NEWROUTE
            || (header.nlmsg_flags & NLM_F_MULTI) == 0) {
            status_ = RouteDumpStatus::malformed;
            return status_;
        }

        const RouteDumpStatus route_status{inspect_route(message, header)};
        if (route_status != RouteDumpStatus::pending) {
            status_ = route_status;
            return status_;
        }

        offset += aligned_length;
    }
    return status_;
}

RouteDumpStatus RouteDumpDecoder::status() const
{
    return status_;
}

int RouteDumpDecoder::error_number() const
{
    return error_number_;
}

} // namespace netlaglab::network_environment::detail
