#include "network_environment/qdisc_inventory.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace netlaglab::network_environment::detail {
namespace {

template<typename Value>
void write_object(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const Value& value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void append_attribute(
    std::vector<std::byte>& bytes,
    const std::uint16_t type,
    const std::span<const std::byte> payload)
{
    const std::size_t offset{bytes.size()};
    const std::size_t length{RTA_LENGTH(payload.size())};
    bytes.resize(offset + RTA_ALIGN(length));
    const rtattr attribute{static_cast<std::uint16_t>(length), type};
    write_object(bytes, offset, attribute);
    std::memcpy(bytes.data() + offset + RTA_LENGTH(0), payload.data(), payload.size());
}

template<typename Value>
void append_attribute(
    std::vector<std::byte>& bytes,
    const std::uint16_t type,
    const Value& value)
{
    append_attribute(bytes, type, std::as_bytes(std::span{&value, std::size_t{1}}));
}

std::vector<std::byte> netem_root_message()
{
    constexpr std::uint32_t sequence{19};
    constexpr std::uint32_t port_id{31};
    constexpr std::uint32_t ifindex{20};
    std::vector<std::byte> message(NLMSG_LENGTH(sizeof(tcmsg)));
    tcmsg information{};
    information.tcm_family = AF_UNSPEC;
    information.tcm_ifindex = static_cast<int>(ifindex);
    information.tcm_handle = TC_H_MAKE(0x4e4cU, 0U);
    information.tcm_parent = TC_H_ROOT;
    write_object(message, NLMSG_HDRLEN, information);

    constexpr std::string_view kind{"netem"};
    std::vector<std::byte> kind_bytes(kind.size() + 1U);
    std::memcpy(kind_bytes.data(), kind.data(), kind.size());
    append_attribute(message, TCA_KIND, std::span<const std::byte>{kind_bytes});

    tc_netem_qopt base_options{};
    base_options.limit = 1000U;
    base_options.loss = 64424509U; // tc's 1.5% fixed-point value.
    std::vector<std::byte> options(sizeof(base_options));
    std::memcpy(options.data(), &base_options, sizeof(base_options));
    constexpr std::uint64_t zero_delay{};
    constexpr std::uint64_t ten_milliseconds{10'000'000U};
    append_attribute(options, TCA_NETEM_LATENCY64, zero_delay);
    append_attribute(options, TCA_NETEM_JITTER64, ten_milliseconds);
    append_attribute(
        message,
        static_cast<std::uint16_t>(TCA_OPTIONS | NLA_F_NESTED),
        std::span<const std::byte>{options});

    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(message.size());
    header.nlmsg_type = RTM_NEWQDISC;
    header.nlmsg_flags = NLM_F_MULTI;
    header.nlmsg_seq = sequence;
    header.nlmsg_pid = port_id;
    write_object(message, 0U, header);
    return message;
}

TEST(QdiscMessageDecoderTest, ReadsZeroDelayJitterAndLossFromNetemAttributes)
{
    QdiscMessageDecoder decoder{19U, 31U, 20U};

    const QdiscMessage result{decoder.consume(netem_root_message())};

    ASSERT_EQ(result.status, InventoryStatus::present);
    ASSERT_EQ(result.kind, QdiscMessageKind::root);
    ASSERT_TRUE(result.root.has_value());
    EXPECT_EQ(result.root->kind, RootQdiscKind::netem);
    EXPECT_EQ(result.root->handle, TC_H_MAKE(0x4e4cU, 0U));
    EXPECT_EQ(result.root->delay_nanoseconds, 0U);
    EXPECT_EQ(result.root->jitter_nanoseconds, 10'000'000U);
    EXPECT_EQ(result.root->loss_probability, 64424509U);
    EXPECT_FALSE(result.root->has_unsupported_options);
}

TEST(QdiscMessageDecoderTest, RejectsTruncatedNetemAttributes)
{
    std::vector<std::byte> message{netem_root_message()};
    message.pop_back();
    QdiscMessageDecoder decoder{19U, 31U, 20U};

    EXPECT_EQ(decoder.consume(message).status, InventoryStatus::malformed);
}

} // namespace
} // namespace netlaglab::network_environment::detail
