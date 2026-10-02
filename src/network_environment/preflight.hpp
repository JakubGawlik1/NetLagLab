#pragma once

#include "network_environment.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace netlaglab::network_environment::detail {

struct Ipv4Prefix {
    std::uint32_t address;
    std::uint8_t length;
};

[[nodiscard]] bool prefixes_overlap(Ipv4Prefix left, Ipv4Prefix right);

struct FileMetadata {
    bool regular;
    std::uint32_t owner;
    std::uint32_t mode;
};

[[nodiscard]] bool is_trusted_executable(FileMetadata metadata);
[[nodiscard]] std::span<const std::string_view> trusted_ip_paths();

enum class RouteDumpStatus {
    pending,
    complete,
    collision,
    netlink_error,
    timeout,
    malformed,
};

class RouteDumpDecoder {
public:
    RouteDumpDecoder(std::uint32_t sequence, std::uint32_t port_id);

    [[nodiscard]] RouteDumpStatus consume(std::span<const std::byte> bytes);
    [[nodiscard]] RouteDumpStatus status() const;
    [[nodiscard]] int error_number() const;

private:
    std::uint32_t sequence_;
    std::uint32_t port_id_;
    RouteDumpStatus status_{RouteDumpStatus::pending};
    int error_number_{};
};

enum class QueryStatus {
    absent,
    present,
    failure,
};

struct ToolQuery {
    QueryStatus status;
    FileMetadata metadata{};
};

struct AddressQuery {
    bool succeeded;
    std::vector<Ipv4Prefix> prefixes;
};

class HostLock {
public:
    virtual ~HostLock() = default;
};

struct HostLockResult {
    std::unique_ptr<HostLock> lock;
    Cause cause{Cause::system_failure};
};

class PreflightPlatform {
public:
    virtual ~PreflightPlatform() = default;

    [[nodiscard]] virtual HostLockResult acquire_host_lock() = 0;
    [[nodiscard]] virtual bool privileged() const = 0;
    [[nodiscard]] virtual ToolQuery query_tool(std::string_view path) = 0;
    [[nodiscard]] virtual QueryStatus query_namespace_name() = 0;
    [[nodiscard]] virtual QueryStatus query_link_name(std::string_view name) = 0;
    [[nodiscard]] virtual AddressQuery query_addresses() = 0;
    [[nodiscard]] virtual RouteDumpStatus query_routes(
        std::chrono::steady_clock::time_point deadline) = 0;
};

struct PreflightResult {
    bool succeeded;
    Cause cause;
    std::string ip_path;
};

[[nodiscard]] PreflightResult run_preflight(
    PreflightPlatform& platform,
    std::chrono::steady_clock::time_point deadline);

[[nodiscard]] std::unique_ptr<PreflightPlatform> make_linux_preflight_platform();

} // namespace netlaglab::network_environment::detail
