#pragma once

#include "network_environment.hpp"
#include "preflight.hpp"
#include "production_adapter.hpp"

#include <cstddef>
#include <memory>

#ifdef NETLAGLAB_BUILD_PRIVILEGED_TESTS
namespace netlaglab::network_environment::detail {

[[nodiscard]] int duplicate_namespace_descriptor_for_test(
    const NamespaceHandle& handle);

} // namespace netlaglab::network_environment::detail
#endif

namespace netlaglab::network_environment::testing {

struct ProductionTrace {
    std::size_t semantic_mutation_requests{};
};

[[nodiscard]] PreparationResult prepare_with_preflight_platform(
    std::unique_ptr<detail::PreflightPlatform> platform,
    std::shared_ptr<ProductionTrace> trace = {});

[[nodiscard]] PreparationResult prepare_with_production_platform(
    std::unique_ptr<detail::PreflightPlatform> preflight,
    std::unique_ptr<detail::ProductionPlatform> production);

#ifdef NETLAGLAB_BUILD_PRIVILEGED_TESTS
[[nodiscard]] int duplicate_owned_namespace_descriptor(
    const PreparedNetworkEnvironment& environment);
#endif

} // namespace netlaglab::network_environment::testing
