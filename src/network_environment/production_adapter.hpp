#pragma once

#include "command_runner.hpp"
#include "link_inventory.hpp"
#include "network_environment.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace netlaglab::network_environment::detail {

struct NamespaceIdentity {
    std::uint64_t device;
    std::uint64_t inode;

    friend bool operator==(
        const NamespaceIdentity&,
        const NamespaceIdentity&) = default;
};

class NamespaceHandle {
public:
    virtual ~NamespaceHandle() = default;
    [[nodiscard]] virtual NamespaceIdentity identity() const = 0;
};

struct NamespaceQuery {
    InventoryStatus status;
    std::unique_ptr<NamespaceHandle> handle;
};

class ProductionPlatform {
public:
    virtual ~ProductionPlatform() = default;

    [[nodiscard]] virtual CommandResult run_ip(
        std::string_view executable_path,
        std::span<const std::string> arguments,
        const NamespaceHandle* inherited_namespace,
        std::chrono::steady_clock::time_point deadline) = 0;
    [[nodiscard]] virtual NamespaceQuery query_namespace() = 0;
    [[nodiscard]] virtual std::string namespace_file_argument(
        const NamespaceHandle& handle) const = 0;
    [[nodiscard]] virtual LinkQuery query_host_link(
        std::string_view name,
        std::chrono::steady_clock::time_point deadline) = 0;
    [[nodiscard]] virtual LinkQuery query_namespace_link(
        const NamespaceHandle& handle,
        std::string_view name,
        std::chrono::steady_clock::time_point deadline) = 0;
};

[[nodiscard]] std::unique_ptr<ProductionPlatform> make_linux_production_platform();

} // namespace netlaglab::network_environment::detail
