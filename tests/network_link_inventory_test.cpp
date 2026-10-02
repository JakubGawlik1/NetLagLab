#include "network_environment/link_inventory.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/if_link.h>
#include <linux/netlink.h>
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
    const rtattr attribute{
        static_cast<std::uint16_t>(length),
        type,
    };
    write_object(bytes, offset, attribute);
    std::memcpy(
        bytes.data() + offset + RTA_LENGTH(0),
        payload.data(),
        payload.size());
}

template<typename Value>
void append_attribute(
    std::vector<std::byte>& bytes,
    const std::uint16_t type,
    const Value& value)
{
    append_attribute(
        bytes,
        type,
        std::as_bytes(std::span{&value, std::size_t{1}}));
}

std::vector<std::byte> link_message(
    const std::string& name,
    const std::string& kind = "veth")
{
    constexpr std::uint32_t sequence{19};
    constexpr std::uint32_t port_id{31};
    constexpr std::uint32_t index{20};
    constexpr std::uint32_t peer_index{21};
    constexpr std::int32_t peer_namespace{4};
    std::vector<std::byte> message(NLMSG_LENGTH(sizeof(ifinfomsg)));
    ifinfomsg information{};
    information.ifi_family = AF_UNSPEC;
    information.ifi_index = static_cast<int>(index);
    write_object(message, NLMSG_HDRLEN, information);

    std::vector<std::byte> name_bytes(name.size() + 1);
    std::memcpy(name_bytes.data(), name.c_str(), name_bytes.size());
    append_attribute(
        message, IFLA_IFNAME, std::span<const std::byte>{name_bytes});
    append_attribute(message, IFLA_LINK, peer_index);
    append_attribute(message, IFLA_LINK_NETNSID, peer_namespace);

    std::vector<std::byte> link_info;
    std::vector<std::byte> kind_bytes(kind.size() + 1);
    std::memcpy(kind_bytes.data(), kind.c_str(), kind_bytes.size());
    append_attribute(
        link_info, IFLA_INFO_KIND, std::span<const std::byte>{kind_bytes});
    append_attribute(
        message,
        static_cast<std::uint16_t>(IFLA_LINKINFO | NLA_F_NESTED),
        std::span<const std::byte>{link_info});

    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(message.size());
    header.nlmsg_type = RTM_NEWLINK;
    header.nlmsg_seq = sequence;
    header.nlmsg_pid = port_id;
    write_object(message, 0, header);
    return message;
}

TEST(LinkMessageDecoderTest, DecodesVethIdentityAndPeerNamespace)
{
    LinkMessageDecoder decoder{19, 31, 20, "nll-host"};

    const LinkQuery result{decoder.consume(link_message("nll-host"))};

    ASSERT_EQ(result.status, InventoryStatus::present);
    ASSERT_TRUE(result.identity.has_value());
    EXPECT_EQ(result.identity->index, 20U);
    EXPECT_EQ(result.identity->peer_index, 21U);
    EXPECT_EQ(result.identity->peer_namespace_id, 4);
}

TEST(LinkMessageDecoderTest, RejectsWrongNameOrNonVethKind)
{
    LinkMessageDecoder wrong_name{19, 31, 20, "nll-host"};
    LinkMessageDecoder wrong_kind{19, 31, 20, "nll-host"};

    EXPECT_EQ(
        wrong_name.consume(link_message("other")).status,
        InventoryStatus::malformed);
    EXPECT_EQ(
        wrong_kind.consume(link_message("nll-host", "dummy")).status,
        InventoryStatus::malformed);
}

TEST(LinkMessageDecoderTest, MapsMissingLinkErrorToAbsence)
{
    std::vector<std::byte> message(NLMSG_LENGTH(sizeof(nlmsgerr)));
    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(message.size());
    header.nlmsg_type = NLMSG_ERROR;
    header.nlmsg_seq = 19;
    header.nlmsg_pid = 31;
    write_object(message, 0, header);
    nlmsgerr error{};
    error.error = -ENODEV;
    write_object(message, NLMSG_HDRLEN, error);
    LinkMessageDecoder decoder{19, 31, 20, "nll-host"};

    const LinkQuery result{decoder.consume(message)};

    EXPECT_EQ(result.status, InventoryStatus::absent);
    EXPECT_EQ(decoder.error_number(), ENODEV);
}

TEST(LinkMessageDecoderTest, RejectsTruncatedAttribute)
{
    std::vector<std::byte> message{link_message("nll-host")};
    message.pop_back();
    LinkMessageDecoder decoder{19, 31, 20, "nll-host"};

    EXPECT_EQ(
        decoder.consume(message).status,
        InventoryStatus::malformed);
}

} // namespace
} // namespace netlaglab::network_environment::detail
