#pragma once

#include "link_inventory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace netlaglab::network_environment::detail {

enum class QdiscMessageKind { root, ignored, dump_complete };

enum class RootQdiscKind { noqueue, netem, other };

struct RootQdisc {
    std::uint32_t handle{};
    RootQdiscKind kind{RootQdiscKind::other};
    std::uint64_t delay_nanoseconds{};
    std::uint64_t jitter_nanoseconds{};
    std::uint32_t loss_probability{};
    bool has_unsupported_options{};
};

struct QdiscMessage {
    InventoryStatus status{InventoryStatus::malformed};
    QdiscMessageKind kind{QdiscMessageKind::ignored};
    std::optional<RootQdisc> root;
};

struct QdiscQuery {
    InventoryStatus status;
    std::optional<RootQdisc> root;
};

class QdiscMessageDecoder {
public:
    QdiscMessageDecoder(
        std::uint32_t sequence,
        std::uint32_t port_id,
        std::uint32_t expected_index);

    [[nodiscard]] QdiscMessage consume(std::span<const std::byte> bytes);

private:
    std::uint32_t sequence_;
    std::uint32_t port_id_;
    std::uint32_t expected_index_;
};

} // namespace netlaglab::network_environment::detail
