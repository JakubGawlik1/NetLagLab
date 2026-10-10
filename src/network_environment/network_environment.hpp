#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace netlaglab::network_environment {

enum class Stage {
    preflight,
    namespace_creation,
    veth_creation,
    host_configuration,
    namespace_configuration,
    route_configuration,
    internet_connectivity,
    cleanup,
};

enum class Cause {
    collision,
    unavailable_or_invalid_tool,
    system_failure,
    command_exit,
    command_signal,
    timeout,
    identity_unavailable,
    identity_mismatch,
    incomplete_cleanup,
    unsupported_host_configuration,
};

struct Failure {
    Stage stage;
    Cause cause;
};

namespace detail {
struct OwnerState;
struct OwnerAccess;
struct PreparationRuntime;
struct PreparationAccess;
class NamespaceHandle;
} // namespace detail

class CleanupResult;

class WorkloadNamespaceEntry {
public:
    WorkloadNamespaceEntry() noexcept = default;
    WorkloadNamespaceEntry(const WorkloadNamespaceEntry&) = default;
    WorkloadNamespaceEntry& operator=(const WorkloadNamespaceEntry&) = default;

    [[nodiscard]] bool enter() const noexcept;

private:
    explicit WorkloadNamespaceEntry(const detail::NamespaceHandle* handle) noexcept;

    const detail::NamespaceHandle* handle_{};

    friend class PreparedNetworkEnvironment;
};

class PreparationInput {
public:
    PreparationInput(PreparationInput&&) noexcept;
    PreparationInput& operator=(PreparationInput&&) = delete;
    PreparationInput(const PreparationInput&) = delete;
    PreparationInput& operator=(const PreparationInput&) = delete;
    ~PreparationInput();

private:
    explicit PreparationInput(
        std::unique_ptr<detail::PreparationRuntime> runtime);

    std::unique_ptr<detail::PreparationRuntime> runtime_;

    friend struct detail::PreparationAccess;
};

class PreparedNetworkEnvironment {
public:
    PreparedNetworkEnvironment(PreparedNetworkEnvironment&&) noexcept;
    PreparedNetworkEnvironment& operator=(PreparedNetworkEnvironment&&) = delete;
    PreparedNetworkEnvironment(const PreparedNetworkEnvironment&) = delete;
    PreparedNetworkEnvironment& operator=(const PreparedNetworkEnvironment&) = delete;
    ~PreparedNetworkEnvironment() noexcept;

    [[nodiscard]] WorkloadNamespaceEntry workload_namespace() const noexcept;
    [[nodiscard]] CleanupResult cleanup() &&;

private:
    explicit PreparedNetworkEnvironment(std::unique_ptr<detail::OwnerState> state);

    std::unique_ptr<detail::OwnerState> state_;

    friend struct detail::OwnerAccess;
};

class ResidualCleanup {
public:
    ResidualCleanup(ResidualCleanup&&) noexcept;
    ResidualCleanup& operator=(ResidualCleanup&&) = delete;
    ResidualCleanup(const ResidualCleanup&) = delete;
    ResidualCleanup& operator=(const ResidualCleanup&) = delete;
    ~ResidualCleanup() noexcept;

    [[nodiscard]] CleanupResult retry() &&;

private:
    explicit ResidualCleanup(std::unique_ptr<detail::OwnerState> state);

    std::unique_ptr<detail::OwnerState> state_;

    friend struct detail::OwnerAccess;
};

struct CleanupResult {
    std::vector<Failure> failures;
    std::optional<ResidualCleanup> residual;
};

struct PreparationFailure {
    Failure primary;
    std::vector<Failure> rollback_failures;
    std::optional<ResidualCleanup> residual;
};

using PreparationResult =
    std::variant<PreparedNetworkEnvironment, PreparationFailure>;

[[nodiscard]] PreparationResult prepare_network_environment(
    PreparationInput input);

[[nodiscard]] PreparationResult prepare_network_environment();

} // namespace netlaglab::network_environment
