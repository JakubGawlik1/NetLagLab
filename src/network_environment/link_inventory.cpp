#include "link_inventory.hpp"

#include <cerrno>
#include <cstring>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <string_view>

namespace netlaglab::network_environment::detail {
namespace {

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

[[nodiscard]] bool string_attribute(
    const std::span<const std::byte> message,
    const std::size_t offset,
    const std::size_t length,
    std::string_view& value)
{
    if (length == 0 || offset > message.size()
        || message.size() - offset < length) {
        return false;
    }
    const char* data{reinterpret_cast<const char*>(message.data() + offset)};
    if (data[length - 1] != '\0'
        || std::memchr(data, '\0', length - 1) != nullptr) {
        return false;
    }
    value = std::string_view{data, length - 1};
    return true;
}

[[nodiscard]] bool veth_kind(
    const std::span<const std::byte> message,
    const std::size_t payload_offset,
    const std::size_t payload_length)
{
    std::size_t offset{payload_offset};
    std::size_t remaining{payload_length};
    bool found_kind{};
    while (remaining != 0) {
        rtattr attribute{};
        if (!copy_object(message, offset, attribute)
            || attribute.rta_len < RTA_LENGTH(0)
            || attribute.rta_len > remaining) {
            return false;
        }
        const std::size_t length{attribute.rta_len - RTA_LENGTH(0)};
        if ((attribute.rta_type & NLA_TYPE_MASK) == IFLA_INFO_KIND) {
            if (found_kind) {
                return false;
            }
            std::string_view kind;
            if (!string_attribute(
                    message, offset + RTA_LENGTH(0), length, kind)
                || kind != "veth") {
                return false;
            }
            found_kind = true;
        }
        const std::size_t aligned{RTA_ALIGN(attribute.rta_len)};
        if (aligned > remaining) {
            return false;
        }
        offset += aligned;
        remaining -= aligned;
    }
    return found_kind;
}

} // namespace

LinkMessageDecoder::LinkMessageDecoder(
    const std::uint32_t sequence,
    const std::uint32_t port_id,
    const std::uint32_t expected_index,
    std::string expected_name)
    : sequence_{sequence}
    , port_id_{port_id}
    , expected_index_{expected_index}
    , expected_name_{std::move(expected_name)}
{
}

LinkQuery LinkMessageDecoder::consume(const std::span<const std::byte> bytes)
{
    if (bytes.size() < sizeof(nlmsghdr)) {
        return {InventoryStatus::malformed, std::nullopt};
    }
    nlmsghdr header{};
    if (!copy_object(bytes, 0, header)
        || header.nlmsg_len < sizeof(nlmsghdr)
        || header.nlmsg_len > bytes.size()
        || NLMSG_ALIGN(header.nlmsg_len) != bytes.size()
        || header.nlmsg_seq != sequence_ || header.nlmsg_pid != port_id_) {
        return {InventoryStatus::malformed, std::nullopt};
    }
    const auto message{bytes.first(header.nlmsg_len)};
    if (header.nlmsg_type == NLMSG_ERROR) {
        if (header.nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
            return {InventoryStatus::malformed, std::nullopt};
        }
        nlmsgerr error{};
        if (!copy_object(message, NLMSG_HDRLEN, error)) {
            return {InventoryStatus::malformed, std::nullopt};
        }
        if (error.error == 0) {
            return {InventoryStatus::malformed, std::nullopt};
        }
        error_number_ = error.error < 0 ? -error.error : error.error;
        return {
            error_number_ == ENODEV || error_number_ == ENXIO
                ? InventoryStatus::absent
                : InventoryStatus::failure,
            std::nullopt,
        };
    }
    if (header.nlmsg_type != RTM_NEWLINK
        || header.nlmsg_len < NLMSG_LENGTH(sizeof(ifinfomsg))) {
        return {InventoryStatus::malformed, std::nullopt};
    }
    ifinfomsg information{};
    if (!copy_object(message, NLMSG_HDRLEN, information)
        || information.ifi_index <= 0
        || static_cast<std::uint32_t>(information.ifi_index) != expected_index_) {
        return {InventoryStatus::malformed, std::nullopt};
    }

    std::string_view name;
    std::uint32_t peer_index{};
    std::optional<std::int32_t> peer_namespace_id;
    bool has_name{};
    bool has_peer{};
    bool has_kind{};
    std::size_t offset{NLMSG_LENGTH(sizeof(ifinfomsg))};
    std::size_t remaining{header.nlmsg_len - offset};
    while (remaining != 0) {
        rtattr attribute{};
        if (!copy_object(message, offset, attribute)
            || attribute.rta_len < RTA_LENGTH(0)
            || attribute.rta_len > remaining) {
            return {InventoryStatus::malformed, std::nullopt};
        }
        const std::size_t length{attribute.rta_len - RTA_LENGTH(0)};
        const std::size_t payload{offset + RTA_LENGTH(0)};
        const std::uint16_t type{static_cast<std::uint16_t>(
            attribute.rta_type & NLA_TYPE_MASK)};
        if (type == IFLA_IFNAME) {
            if (has_name || !string_attribute(message, payload, length, name)) {
                return {InventoryStatus::malformed, std::nullopt};
            }
            has_name = true;
        } else if (type == IFLA_LINK) {
            if (has_peer || length != sizeof(peer_index)
                || !copy_object(message, payload, peer_index)) {
                return {InventoryStatus::malformed, std::nullopt};
            }
            has_peer = true;
        } else if (type == IFLA_LINK_NETNSID) {
            std::int32_t value{};
            if (peer_namespace_id || length != sizeof(value)
                || !copy_object(message, payload, value)) {
                return {InventoryStatus::malformed, std::nullopt};
            }
            peer_namespace_id = value;
        } else if (type == IFLA_LINKINFO) {
            if (has_kind || !veth_kind(message, payload, length)) {
                return {InventoryStatus::malformed, std::nullopt};
            }
            has_kind = true;
        }
        const std::size_t aligned{RTA_ALIGN(attribute.rta_len)};
        if (aligned > remaining) {
            return {InventoryStatus::malformed, std::nullopt};
        }
        offset += aligned;
        remaining -= aligned;
    }
    if (!has_name || name != expected_name_ || !has_peer || peer_index == 0
        || !has_kind) {
        return {InventoryStatus::malformed, std::nullopt};
    }
    return {
        InventoryStatus::present,
        LinkIdentity{expected_index_, peer_index, peer_namespace_id},
    };
}

int LinkMessageDecoder::error_number() const
{
    return error_number_;
}

} // namespace netlaglab::network_environment::detail
