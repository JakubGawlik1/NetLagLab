#pragma once

#include "network_environment.hpp"
#include "preflight.hpp"

#include <cstddef>
#include <memory>

namespace netlaglab::network_environment::testing {

struct ProductionTrace {
    std::size_t semantic_mutation_requests{};
};

[[nodiscard]] PreparationResult prepare_with_preflight_platform(
    std::unique_ptr<detail::PreflightPlatform> platform,
    std::shared_ptr<ProductionTrace> trace = {});

} // namespace netlaglab::network_environment::testing
