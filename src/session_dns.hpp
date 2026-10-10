#pragma once

#include "file_descriptor.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace netlaglab::session_dns {

struct Snapshot {
    std::string resolver_contents;
    std::string nsswitch_contents;
};

struct SnapshotResult {
    std::optional<Snapshot> snapshot;
    std::string error;
};

[[nodiscard]] SnapshotResult parse_host_dns_snapshot(
    std::string_view resolver_contents,
    std::string_view nsswitch_contents);

[[nodiscard]] SnapshotResult read_host_dns_snapshot();

class PrivateDnsMount {
public:
    PrivateDnsMount(PrivateDnsMount&&) noexcept = default;
    PrivateDnsMount& operator=(PrivateDnsMount&&) noexcept = default;
    PrivateDnsMount(const PrivateDnsMount&) = delete;
    PrivateDnsMount& operator=(const PrivateDnsMount&) = delete;

    [[nodiscard]] static std::optional<PrivateDnsMount> prepare(
        const Snapshot& snapshot,
        int& error_number);
    [[nodiscard]] bool install_in_child(int& error_number) const noexcept;

private:
    PrivateDnsMount(
        FileDescriptor resolver_file,
        FileDescriptor nsswitch_file,
        std::string resolver_source,
        std::string nsswitch_source) noexcept;

    FileDescriptor resolver_file_;
    FileDescriptor nsswitch_file_;
    std::string resolver_source_;
    std::string nsswitch_source_;
};

} // namespace netlaglab::session_dns
