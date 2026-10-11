#include "qdisc_inventory.hpp"

#include <cerrno>
#include <cstring>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <utility>

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
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t length,
    std::string_view& value)
{
    if (length == 0 || offset > bytes.size() || bytes.size() - offset < length) {
        return false;
    }
    const char* const data{reinterpret_cast<const char*>(bytes.data() + offset)};
    if (data[length - 1U] != '\0'
        || std::memchr(data, '\0', length - 1U) != nullptr) {
        return false;
    }
    value = {data, length - 1U};
    return true;
}

[[nodiscard]] bool netem_options(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t length,
    RootQdisc& qdisc)
{
    tc_netem_qopt options{};
    if (length < sizeof(options) || !copy_object(bytes, offset, options)) {
        return false;
    }
    qdisc.loss_probability = options.loss;
    qdisc.has_unsupported_options = options.limit != 1000U
        || options.latency != 0U || options.jitter != 0U || options.gap != 0U
        || options.duplicate != 0U;

    std::size_t cursor{offset + sizeof(options)};
    std::size_t remaining{length - sizeof(options)};
    bool has_delay{};
    bool has_jitter{};
    while (remaining != 0U) {
        rtattr attribute{};
        if (!copy_object(bytes, cursor, attribute)
            || attribute.rta_len < RTA_LENGTH(0)
            || attribute.rta_len > remaining) {
            return false;
        }
        const std::size_t payload_length{attribute.rta_len - RTA_LENGTH(0)};
        const std::size_t payload_offset{cursor + RTA_LENGTH(0)};
        const std::uint16_t type{static_cast<std::uint16_t>(
            attribute.rta_type & NLA_TYPE_MASK)};
        if (type == TCA_NETEM_LATENCY64) {
            if (has_delay || payload_length != sizeof(qdisc.delay_nanoseconds)
                || !copy_object(bytes, payload_offset, qdisc.delay_nanoseconds)) {
                return false;
            }
            has_delay = true;
        } else if (type == TCA_NETEM_JITTER64) {
            if (has_jitter || payload_length != sizeof(qdisc.jitter_nanoseconds)
                || !copy_object(bytes, payload_offset, qdisc.jitter_nanoseconds)) {
                return false;
            }
            has_jitter = true;
        } else {
            qdisc.has_unsupported_options = true;
        }
        const std::size_t aligned{RTA_ALIGN(attribute.rta_len)};
        if (aligned > remaining) return false;
        cursor += aligned;
        remaining -= aligned;
    }
    return true;
}

[[nodiscard]] QdiscMessage malformed()
{
    return {InventoryStatus::malformed, QdiscMessageKind::ignored, std::nullopt};
}

} // namespace

QdiscMessageDecoder::QdiscMessageDecoder(
    const std::uint32_t sequence,
    const std::uint32_t port_id,
    const std::uint32_t expected_index)
    : sequence_{sequence}
    , port_id_{port_id}
    , expected_index_{expected_index}
{
}

QdiscMessage QdiscMessageDecoder::consume(
    const std::span<const std::byte> bytes)
{
    nlmsghdr header{};
    if (bytes.size() < sizeof(header) || !copy_object(bytes, 0U, header)
        || header.nlmsg_len < sizeof(header) || header.nlmsg_len > bytes.size()
        || NLMSG_ALIGN(header.nlmsg_len) != bytes.size()
        || header.nlmsg_seq != sequence_ || header.nlmsg_pid != port_id_
        || (header.nlmsg_flags & NLM_F_DUMP_INTR) != 0U) {
        return malformed();
    }
    const auto message{bytes.first(header.nlmsg_len)};
    if (header.nlmsg_type == NLMSG_DONE) {
        if (header.nlmsg_len != NLMSG_LENGTH(0U)
            && header.nlmsg_len != NLMSG_LENGTH(sizeof(int))) {
            return malformed();
        }
        if (header.nlmsg_len == NLMSG_LENGTH(sizeof(int))) {
            int error{};
            if (!copy_object(message, NLMSG_HDRLEN, error) || error != 0) {
                return {InventoryStatus::failure,
                    QdiscMessageKind::dump_complete, std::nullopt};
            }
        }
        return {InventoryStatus::present,
            QdiscMessageKind::dump_complete, std::nullopt};
    }
    if (header.nlmsg_type == NLMSG_ERROR) {
        nlmsgerr error{};
        if (header.nlmsg_len != NLMSG_LENGTH(sizeof(error))
            || !copy_object(message, NLMSG_HDRLEN, error) || error.error == 0) {
            return malformed();
        }
        const int number{error.error < 0 ? -error.error : error.error};
        return {number == ENODEV || number == ENXIO
                ? InventoryStatus::absent
                : InventoryStatus::failure,
            QdiscMessageKind::ignored, std::nullopt};
    }
    if (header.nlmsg_type != RTM_NEWQDISC
        || header.nlmsg_len < NLMSG_LENGTH(sizeof(tcmsg))) {
        return malformed();
    }

    tcmsg information{};
    if (!copy_object(message, NLMSG_HDRLEN, information)
        || information.tcm_family != AF_UNSPEC
        || information.tcm_ifindex <= 0) {
        return malformed();
    }
    if (static_cast<std::uint32_t>(information.tcm_ifindex) != expected_index_
        || information.tcm_parent != TC_H_ROOT) {
        return {InventoryStatus::present, QdiscMessageKind::ignored, std::nullopt};
    }

    std::string_view kind;
    bool has_kind{};
    std::optional<std::pair<std::size_t, std::size_t>> options;
    std::size_t cursor{NLMSG_LENGTH(sizeof(tcmsg))};
    std::size_t remaining{header.nlmsg_len - cursor};
    while (remaining != 0U) {
        rtattr attribute{};
        if (!copy_object(message, cursor, attribute)
            || attribute.rta_len < RTA_LENGTH(0)
            || attribute.rta_len > remaining) {
            return malformed();
        }
        const std::size_t payload_length{attribute.rta_len - RTA_LENGTH(0)};
        const std::size_t payload_offset{cursor + RTA_LENGTH(0)};
        const std::uint16_t type{static_cast<std::uint16_t>(
            attribute.rta_type & NLA_TYPE_MASK)};
        if (type == TCA_KIND) {
            if (has_kind
                || !string_attribute(message, payload_offset, payload_length, kind)) {
                return malformed();
            }
            has_kind = true;
        } else if (type == TCA_OPTIONS) {
            if (options) return malformed();
            options = std::pair{payload_offset, payload_length};
        }
        const std::size_t aligned{RTA_ALIGN(attribute.rta_len)};
        if (aligned > remaining) return malformed();
        cursor += aligned;
        remaining -= aligned;
    }
    if (!has_kind) return malformed();

    RootQdisc qdisc{};
    qdisc.handle = information.tcm_handle;
    if (kind == "noqueue") {
        if (options || information.tcm_handle != 0U) return malformed();
        qdisc.kind = RootQdiscKind::noqueue;
    } else if (kind == "netem") {
        if (!options) return malformed();
        qdisc.kind = RootQdiscKind::netem;
        if (!netem_options(message, options->first, options->second, qdisc)) {
            return malformed();
        }
    } else {
        qdisc.kind = RootQdiscKind::other;
        qdisc.has_unsupported_options = true;
    }
    return {InventoryStatus::present, QdiscMessageKind::root, qdisc};
}

} // namespace netlaglab::network_environment::detail
