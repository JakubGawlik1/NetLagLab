#pragma once

#include "file_descriptor.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace netlaglab {

struct ResolverViewConfiguration {
    std::string resolv_conf;
    std::string nsswitch_conf;
};

struct ResolverViewReadResult {
    std::optional<ResolverViewConfiguration> configuration;
    std::string failure;
};

[[nodiscard]] std::optional<std::string> make_resolver_snapshot(
    std::string_view source,
    std::string& failure);
[[nodiscard]] std::optional<std::string> make_nsswitch_snapshot(
    std::string_view source,
    std::string& failure);
[[nodiscard]] ResolverViewReadResult read_host_resolver_view();

class WorkloadResolverView {
public:
    WorkloadResolverView(
        FileDescriptor resolv_conf,
        FileDescriptor nsswitch_conf) noexcept;
    WorkloadResolverView(WorkloadResolverView&&) noexcept = default;
    WorkloadResolverView& operator=(WorkloadResolverView&&) noexcept = default;
    WorkloadResolverView(const WorkloadResolverView&) = delete;
    WorkloadResolverView& operator=(const WorkloadResolverView&) = delete;

    [[nodiscard]] int resolv_conf_descriptor() const noexcept;
    [[nodiscard]] int nsswitch_conf_descriptor() const noexcept;

private:
    FileDescriptor resolv_conf_;
    FileDescriptor nsswitch_conf_;
};

struct WorkloadResolverViewResult {
    std::optional<WorkloadResolverView> view;
    std::string failure;
};

[[nodiscard]] WorkloadResolverViewResult create_workload_resolver_view(
    const ResolverViewConfiguration& configuration);

} // namespace netlaglab
