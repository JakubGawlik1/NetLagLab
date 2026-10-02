#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace netlaglab::network_environment::detail {

enum class InventoryStatus {
    absent,
    present,
    failure,
    timeout,
    malformed,
};

struct LinkIdentity {
    std::uint32_t index;
    std::uint32_t peer_index;
    std::optional<std::int32_t> peer_namespace_id;

    friend bool operator==(const LinkIdentity&, const LinkIdentity&) = default;
};

struct LinkQuery {
    InventoryStatus status;
    std::optional<LinkIdentity> identity;
};

class LinkMessageDecoder {
public:
    LinkMessageDecoder(
        std::uint32_t sequence,
        std::uint32_t port_id,
        std::uint32_t expected_index,
        std::string expected_name);

    [[nodiscard]] LinkQuery consume(std::span<const std::byte> bytes);
    [[nodiscard]] int error_number() const;

private:
    std::uint32_t sequence_;
    std::uint32_t port_id_;
    std::uint32_t expected_index_;
    std::string expected_name_;
    int error_number_{};
};

} // namespace netlaglab::network_environment::detail
