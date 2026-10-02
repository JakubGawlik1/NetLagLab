#include "network_environment.hpp"

#include "preflight.hpp"
#include "production_test_support.hpp"
#include "transaction_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace netlaglab::network_environment {
namespace {

using TimePoint = std::chrono::steady_clock::time_point;
using testing::Operation;
using testing::Outcome;
using testing::ScriptStep;

constexpr auto transaction_limit{std::chrono::seconds{30}};
constexpr auto operation_limit{std::chrono::seconds{5}};

struct IdentityUnconfirmed {
};

class NamespaceProof {
public:
    explicit NamespaceProof(std::unique_ptr<std::size_t> exact_handle)
        : exact_handle_{std::move(exact_handle)}
    {
    }

    NamespaceProof(NamespaceProof&&) noexcept = default;
    NamespaceProof& operator=(NamespaceProof&&) noexcept = default;
    NamespaceProof(const NamespaceProof&) = delete;
    NamespaceProof& operator=(const NamespaceProof&) = delete;

private:
    std::unique_ptr<std::size_t> exact_handle_;
};

class HostVethProof {
public:
    explicit HostVethProof(std::unique_ptr<std::size_t> identity)
        : identity_{std::move(identity)}
    {
    }

    HostVethProof(HostVethProof&&) noexcept = default;
    HostVethProof& operator=(HostVethProof&&) noexcept = default;
    HostVethProof(const HostVethProof&) = delete;
    HostVethProof& operator=(const HostVethProof&) = delete;

    [[nodiscard]] std::unique_ptr<std::size_t> release_identity()
    {
        return std::move(identity_);
    }

private:
    std::unique_ptr<std::size_t> identity_;
};

class PlacedVethProof {
public:
    explicit PlacedVethProof(std::unique_ptr<std::size_t> identity)
        : identity_{std::move(identity)}
    {
    }

    PlacedVethProof(PlacedVethProof&&) noexcept = default;
    PlacedVethProof& operator=(PlacedVethProof&&) noexcept = default;
    PlacedVethProof(const PlacedVethProof&) = delete;
    PlacedVethProof& operator=(const PlacedVethProof&) = delete;

private:
    std::unique_ptr<std::size_t> identity_;
};

using NamespaceState =
    std::variant<std::monostate, NamespaceProof, IdentityUnconfirmed>;
using VethState = std::variant<
    std::monostate,
    HostVethProof,
    PlacedVethProof,
    IdentityUnconfirmed>;
using ProvenVeth = std::variant<HostVethProof, PlacedVethProof>;

struct OperationResult {
    bool succeeded;
    Cause cause;
};

struct NamespaceMutationResult {
    bool succeeded;
    Cause cause;
    NamespaceState state;
};

struct VethMutationResult {
    bool succeeded;
    Cause cause;
    VethState state;
};

struct NamespaceRemovalResult {
    Cause cause;
    NamespaceState state;
};

struct VethRemovalResult {
    Cause cause;
    VethState state;
};

class MonotonicClock {
public:
    virtual ~MonotonicClock() = default;
    [[nodiscard]] virtual TimePoint now() const = 0;
};

class SystemClock final : public MonotonicClock {
public:
    [[nodiscard]] TimePoint now() const override
    {
        return std::chrono::steady_clock::now();
    }
};

class SemanticAdapter {
public:
    virtual ~SemanticAdapter() = default;

    [[nodiscard]] virtual OperationResult preflight(TimePoint deadline) = 0;
    [[nodiscard]] virtual NamespaceMutationResult create_namespace(
        TimePoint deadline) = 0;
    [[nodiscard]] virtual VethMutationResult create_veth(TimePoint deadline) = 0;
    [[nodiscard]] virtual VethMutationResult move_peer(
        const NamespaceProof& namespace_proof,
        HostVethProof veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual OperationResult assign_host_address(
        const PlacedVethProof& veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual OperationResult bring_host_link_up(
        const PlacedVethProof& veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual OperationResult bring_loopback_up(
        const NamespaceProof& namespace_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual OperationResult assign_namespace_address(
        const NamespaceProof& namespace_proof,
        const PlacedVethProof& veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual OperationResult bring_namespace_link_up(
        const NamespaceProof& namespace_proof,
        const PlacedVethProof& veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual OperationResult add_default_route(
        const NamespaceProof& namespace_proof,
        const PlacedVethProof& veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual VethRemovalResult remove_veth(
        ProvenVeth proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual NamespaceRemovalResult remove_namespace(
        NamespaceProof proof,
        TimePoint deadline) = 0;
};

struct ScriptState {
    TimePoint now;
    std::deque<ScriptStep> steps;
    std::shared_ptr<testing::SharedTrace> trace;
    std::size_t next_identity{1};
};

class ScriptedClock final : public MonotonicClock {
public:
    explicit ScriptedClock(std::shared_ptr<ScriptState> state)
        : state_{std::move(state)}
    {
    }

    [[nodiscard]] TimePoint now() const override { return state_->now; }

private:
    std::shared_ptr<ScriptState> state_;
};

class ScriptedAdapter final : public SemanticAdapter {
public:
    explicit ScriptedAdapter(std::shared_ptr<ScriptState> state)
        : state_{std::move(state)}
    {
    }

    [[nodiscard]] OperationResult preflight(const TimePoint deadline) override
    {
        return operation(Operation::preflight, deadline);
    }

    [[nodiscard]] NamespaceMutationResult create_namespace(
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::create_namespace, deadline)};
        NamespaceState state;
        if (step.outcome == Outcome::success
            || step.outcome == Outcome::fail_new_state) {
            state.emplace<NamespaceProof>(identity());
        } else if (step.outcome == Outcome::fail_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.outcome == Outcome::success, step.cause, std::move(state)};
    }

    [[nodiscard]] VethMutationResult create_veth(
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::create_veth, deadline)};
        VethState state;
        if (step.outcome == Outcome::success
            || step.outcome == Outcome::fail_new_state) {
            state.emplace<HostVethProof>(identity());
        } else if (step.outcome == Outcome::fail_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.outcome == Outcome::success, step.cause, std::move(state)};
    }

    [[nodiscard]] VethMutationResult move_peer(
        const NamespaceProof&,
        HostVethProof proof,
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::move_peer, deadline)};
        VethState state;
        if (step.outcome == Outcome::success
            || step.outcome == Outcome::fail_new_state) {
            state.emplace<PlacedVethProof>(proof.release_identity());
        } else if (step.outcome == Outcome::fail_unchanged) {
            state.emplace<HostVethProof>(std::move(proof));
        } else if (step.outcome == Outcome::fail_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.outcome == Outcome::success, step.cause, std::move(state)};
    }

    [[nodiscard]] OperationResult assign_host_address(
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        return operation(Operation::assign_host_address, deadline);
    }

    [[nodiscard]] OperationResult bring_host_link_up(
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        return operation(Operation::bring_host_link_up, deadline);
    }

    [[nodiscard]] OperationResult bring_loopback_up(
        const NamespaceProof&,
        const TimePoint deadline) override
    {
        return operation(Operation::bring_loopback_up, deadline);
    }

    [[nodiscard]] OperationResult assign_namespace_address(
        const NamespaceProof&,
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        return operation(Operation::assign_namespace_address, deadline);
    }

    [[nodiscard]] OperationResult bring_namespace_link_up(
        const NamespaceProof&,
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        return operation(Operation::bring_namespace_link_up, deadline);
    }

    [[nodiscard]] OperationResult add_default_route(
        const NamespaceProof&,
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        return operation(Operation::add_default_route, deadline);
    }

    [[nodiscard]] VethRemovalResult remove_veth(
        ProvenVeth proof,
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::remove_veth, deadline)};
        VethState state;
        if (step.outcome == Outcome::cleanup_retained) {
            std::visit(
                [&state](auto retained) {
                    state.emplace<std::decay_t<decltype(retained)>>(
                        std::move(retained));
                },
                std::move(proof));
        } else if (step.outcome == Outcome::cleanup_identity_mismatch
                   || step.outcome == Outcome::cleanup_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.cause, std::move(state)};
    }

    [[nodiscard]] NamespaceRemovalResult remove_namespace(
        NamespaceProof proof,
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::remove_namespace, deadline)};
        NamespaceState state;
        if (step.outcome == Outcome::cleanup_retained) {
            state.emplace<NamespaceProof>(std::move(proof));
        } else if (step.outcome == Outcome::cleanup_identity_mismatch
                   || step.outcome == Outcome::cleanup_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.cause, std::move(state)};
    }

private:
    [[nodiscard]] std::unique_ptr<std::size_t> identity()
    {
        return std::make_unique<std::size_t>(state_->next_identity++);
    }

    [[nodiscard]] ScriptStep next(
        const Operation operation,
        const TimePoint deadline)
    {
        state_->trace->operations.push_back(operation);
        state_->trace->deadlines.push_back(deadline);
        if (state_->steps.empty() || state_->steps.front().operation != operation) {
            return {operation, Outcome::fail_unchanged, Cause::system_failure};
        }
        ScriptStep step{state_->steps.front()};
        state_->steps.pop_front();
        state_->now += step.elapsed;
        return step;
    }

    [[nodiscard]] OperationResult operation(
        const Operation operation,
        const TimePoint deadline)
    {
        const ScriptStep step{next(operation, deadline)};
        return {step.outcome == Outcome::success, step.cause};
    }

    std::shared_ptr<ScriptState> state_;
};

class ProductionAdapter final : public SemanticAdapter {
public:
    explicit ProductionAdapter(
        std::unique_ptr<detail::PreflightPlatform> platform,
        std::shared_ptr<testing::ProductionTrace> trace = {})
        : platform_{std::move(platform)}
        , trace_{std::move(trace)}
    {
    }

    [[nodiscard]] OperationResult preflight(const TimePoint deadline) override
    {
        detail::PreflightResult result{
            detail::run_preflight(*platform_, deadline)};
        if (result.succeeded) {
            ip_path_ = std::move(result.ip_path);
        }
        return {result.succeeded, result.cause};
    }

    [[nodiscard]] NamespaceMutationResult create_namespace(TimePoint) override
    {
        if (trace_) {
            ++trace_->semantic_mutation_requests;
        }
        return {false, Cause::system_failure, {}};
    }

    [[nodiscard]] VethMutationResult create_veth(TimePoint) override
    {
        return {false, Cause::system_failure, {}};
    }

    [[nodiscard]] VethMutationResult move_peer(
        const NamespaceProof&,
        HostVethProof proof,
        TimePoint) override
    {
        VethState state;
        state.emplace<HostVethProof>(std::move(proof));
        return {false, Cause::system_failure, std::move(state)};
    }

    [[nodiscard]] OperationResult assign_host_address(
        const PlacedVethProof&,
        TimePoint) override
    {
        return {false, Cause::system_failure};
    }

    [[nodiscard]] OperationResult bring_host_link_up(
        const PlacedVethProof&,
        TimePoint) override
    {
        return {false, Cause::system_failure};
    }

    [[nodiscard]] OperationResult bring_loopback_up(
        const NamespaceProof&,
        TimePoint) override
    {
        return {false, Cause::system_failure};
    }

    [[nodiscard]] OperationResult assign_namespace_address(
        const NamespaceProof&,
        const PlacedVethProof&,
        TimePoint) override
    {
        return {false, Cause::system_failure};
    }

    [[nodiscard]] OperationResult bring_namespace_link_up(
        const NamespaceProof&,
        const PlacedVethProof&,
        TimePoint) override
    {
        return {false, Cause::system_failure};
    }

    [[nodiscard]] OperationResult add_default_route(
        const NamespaceProof&,
        const PlacedVethProof&,
        TimePoint) override
    {
        return {false, Cause::system_failure};
    }

    [[nodiscard]] VethRemovalResult remove_veth(
        ProvenVeth proof,
        TimePoint) override
    {
        VethState state;
        std::visit(
            [&state](auto retained) {
                state.emplace<std::decay_t<decltype(retained)>>(
                    std::move(retained));
            },
            std::move(proof));
        return {Cause::system_failure, std::move(state)};
    }

    [[nodiscard]] NamespaceRemovalResult remove_namespace(
        NamespaceProof proof,
        TimePoint) override
    {
        NamespaceState state;
        state.emplace<NamespaceProof>(std::move(proof));
        return {Cause::system_failure, std::move(state)};
    }

private:
    std::unique_ptr<detail::PreflightPlatform> platform_;
    std::shared_ptr<testing::ProductionTrace> trace_;
    std::string ip_path_;
};

class TrackedHostLock final : public detail::HostLock {
public:
    explicit TrackedHostLock(std::shared_ptr<testing::SharedTrace> trace)
        : trace_{std::move(trace)}
    {
        trace_->host_lock_alive = true;
    }

    ~TrackedHostLock() override { trace_->host_lock_alive = false; }

private:
    std::shared_ptr<testing::SharedTrace> trace_;
};

[[nodiscard]] TimePoint operation_deadline(
    const TimePoint now,
    const TimePoint transaction_deadline)
{
    return std::min(now + operation_limit, transaction_deadline);
}

[[nodiscard]] bool is_unconfirmed(const NamespaceState& state)
{
    return std::holds_alternative<IdentityUnconfirmed>(state);
}

[[nodiscard]] bool is_unconfirmed(const VethState& state)
{
    return std::holds_alternative<IdentityUnconfirmed>(state);
}

[[nodiscard]] bool is_proven(const NamespaceState& state)
{
    return std::holds_alternative<NamespaceProof>(state);
}

[[nodiscard]] bool is_proven(const VethState& state)
{
    return std::holds_alternative<HostVethProof>(state)
        || std::holds_alternative<PlacedVethProof>(state);
}

} // namespace

namespace detail {

struct PreparationRuntime {
    std::unique_ptr<SemanticAdapter> adapter;
    std::unique_ptr<MonotonicClock> clock;
    std::unique_ptr<HostLock> host_lock;
};

struct OwnerState {
    std::unique_ptr<PreparationRuntime> runtime;
    NamespaceState namespace_root;
    VethState veth_root;
};

struct PreparationAccess {
    [[nodiscard]] static PreparationInput make(
        std::unique_ptr<PreparationRuntime> runtime)
    {
        return PreparationInput{std::move(runtime)};
    }

    [[nodiscard]] static std::unique_ptr<PreparationRuntime> take(
        PreparationInput& input)
    {
        return std::move(input.runtime_);
    }
};

struct OwnerAccess {
    [[nodiscard]] static PreparedNetworkEnvironment prepared(
        std::unique_ptr<OwnerState> state)
    {
        return PreparedNetworkEnvironment{std::move(state)};
    }

    [[nodiscard]] static ResidualCleanup residual(
        std::unique_ptr<OwnerState> state)
    {
        return ResidualCleanup{std::move(state)};
    }

    [[nodiscard]] static std::unique_ptr<OwnerState> take(
        PreparedNetworkEnvironment& owner)
    {
        return std::move(owner.state_);
    }

    [[nodiscard]] static std::unique_ptr<OwnerState> take(ResidualCleanup& owner)
    {
        return std::move(owner.state_);
    }
};

} // namespace detail

namespace {

[[nodiscard]] bool has_residual(const detail::OwnerState& state)
{
    return !std::holds_alternative<std::monostate>(state.namespace_root)
        || !std::holds_alternative<std::monostate>(state.veth_root);
}

[[nodiscard]] ProvenVeth take_proven_veth(VethState& state)
{
    if (auto* proof{std::get_if<HostVethProof>(&state)}) {
        ProvenVeth result{std::in_place_type<HostVethProof>, std::move(*proof)};
        state.emplace<std::monostate>();
        return result;
    }
    auto* proof{std::get_if<PlacedVethProof>(&state)};
    ProvenVeth result{std::in_place_type<PlacedVethProof>, std::move(*proof)};
    state.emplace<std::monostate>();
    return result;
}

[[nodiscard]] CleanupResult cleanup_state(
    std::unique_ptr<detail::OwnerState> state,
    const bool return_residual = true)
{
    CleanupResult result;
    if (!state) {
        return result;
    }

    const bool began_with_unconfirmed{
        is_unconfirmed(state->namespace_root) || is_unconfirmed(state->veth_root)};
    const TimePoint transaction_deadline{
        state->runtime->clock->now() + transaction_limit};
    const auto remove_veth = [&]() {
        if (state->runtime->clock->now() >= transaction_deadline) {
            result.failures.push_back({Stage::cleanup, Cause::timeout});
            return;
        }
        VethRemovalResult removal{state->runtime->adapter->remove_veth(
            take_proven_veth(state->veth_root),
            operation_deadline(
                state->runtime->clock->now(), transaction_deadline))};
        state->veth_root = std::move(removal.state);
        if (!std::holds_alternative<std::monostate>(state->veth_root)) {
            result.failures.push_back({Stage::cleanup, removal.cause});
        }
    };

    if (is_proven(state->veth_root)) {
        remove_veth();
    }
    bool namespace_removed{};
    if (is_proven(state->namespace_root)) {
        if (state->runtime->clock->now() >= transaction_deadline) {
            result.failures.push_back({Stage::cleanup, Cause::timeout});
        } else {
            NamespaceProof proof{
                std::move(std::get<NamespaceProof>(state->namespace_root))};
            state->namespace_root.emplace<std::monostate>();
            NamespaceRemovalResult removal{
                state->runtime->adapter->remove_namespace(
                    std::move(proof),
                    operation_deadline(
                        state->runtime->clock->now(), transaction_deadline))};
            state->namespace_root = std::move(removal.state);
            namespace_removed =
                std::holds_alternative<std::monostate>(state->namespace_root);
            if (!namespace_removed) {
                result.failures.push_back({Stage::cleanup, removal.cause});
            }
        }
    }
    if (namespace_removed && is_proven(state->veth_root)) {
        remove_veth();
    }
    if (return_residual && began_with_unconfirmed) {
        result.failures.push_back({Stage::cleanup, Cause::incomplete_cleanup});
    }
    if (return_residual && has_residual(*state)) {
        result.residual.emplace(detail::OwnerAccess::residual(std::move(state)));
    }
    return result;
}

void best_effort_cleanup(std::unique_ptr<detail::OwnerState> state) noexcept
{
    try {
        (void)cleanup_state(std::move(state), false);
    } catch (...) {
    }
}

[[nodiscard]] PreparationResult preparation_failure(
    std::unique_ptr<detail::OwnerState> state,
    const Stage stage,
    const Cause cause)
{
    CleanupResult rollback{cleanup_state(std::move(state))};
    return PreparationFailure{
        {stage, cause},
        std::move(rollback.failures),
        std::move(rollback.residual),
    };
}

} // namespace

PreparationInput::PreparationInput(
    std::unique_ptr<detail::PreparationRuntime> runtime)
    : runtime_{std::move(runtime)}
{
}

PreparationInput::PreparationInput(PreparationInput&&) noexcept = default;
PreparationInput::~PreparationInput() = default;

PreparedNetworkEnvironment::PreparedNetworkEnvironment(
    std::unique_ptr<detail::OwnerState> state)
    : state_{std::move(state)}
{
}

PreparedNetworkEnvironment::PreparedNetworkEnvironment(
    PreparedNetworkEnvironment&&) noexcept = default;

PreparedNetworkEnvironment::~PreparedNetworkEnvironment() noexcept
{
    best_effort_cleanup(std::move(state_));
}

CleanupResult PreparedNetworkEnvironment::cleanup() &&
{
    return cleanup_state(detail::OwnerAccess::take(*this));
}

ResidualCleanup::ResidualCleanup(std::unique_ptr<detail::OwnerState> state)
    : state_{std::move(state)}
{
}

ResidualCleanup::ResidualCleanup(ResidualCleanup&&) noexcept = default;

ResidualCleanup::~ResidualCleanup() noexcept
{
    best_effort_cleanup(std::move(state_));
}

CleanupResult ResidualCleanup::retry() &&
{
    return cleanup_state(detail::OwnerAccess::take(*this));
}

PreparationResult prepare_network_environment(PreparationInput input)
{
    auto state{std::make_unique<detail::OwnerState>()};
    state->runtime = detail::PreparationAccess::take(input);
    const TimePoint setup_deadline{
        state->runtime->clock->now() + transaction_limit};
    const auto deadline = [&]() {
        return operation_deadline(state->runtime->clock->now(), setup_deadline);
    };
    const auto setup_expired = [&]() {
        return state->runtime->clock->now() >= setup_deadline;
    };

    if (setup_expired()) {
        return preparation_failure(std::move(state), Stage::preflight, Cause::timeout);
    }
    const OperationResult preflight{state->runtime->adapter->preflight(deadline())};
    if (!preflight.succeeded) {
        return preparation_failure(
            std::move(state), Stage::preflight, preflight.cause);
    }

    if (setup_expired()) {
        return preparation_failure(
            std::move(state), Stage::namespace_creation, Cause::timeout);
    }
    NamespaceMutationResult namespace_result{
        state->runtime->adapter->create_namespace(deadline())};
    state->namespace_root = std::move(namespace_result.state);
    if (!namespace_result.succeeded) {
        return preparation_failure(
            std::move(state), Stage::namespace_creation, namespace_result.cause);
    }
    if (!is_proven(state->namespace_root)) {
        return preparation_failure(
            std::move(state), Stage::namespace_creation, Cause::identity_unavailable);
    }

    if (setup_expired()) {
        return preparation_failure(
            std::move(state), Stage::veth_creation, Cause::timeout);
    }
    VethMutationResult veth_result{
        state->runtime->adapter->create_veth(deadline())};
    state->veth_root = std::move(veth_result.state);
    if (!veth_result.succeeded) {
        return preparation_failure(
            std::move(state), Stage::veth_creation, veth_result.cause);
    }
    if (!std::holds_alternative<HostVethProof>(state->veth_root)) {
        return preparation_failure(
            std::move(state), Stage::veth_creation, Cause::identity_unavailable);
    }

    if (setup_expired()) {
        return preparation_failure(
            std::move(state), Stage::veth_creation, Cause::timeout);
    }
    HostVethProof host_veth{
        std::move(std::get<HostVethProof>(state->veth_root))};
    state->veth_root.emplace<std::monostate>();
    VethMutationResult move_result{state->runtime->adapter->move_peer(
        std::get<NamespaceProof>(state->namespace_root),
        std::move(host_veth),
        deadline())};
    state->veth_root = std::move(move_result.state);
    if (!move_result.succeeded) {
        return preparation_failure(
            std::move(state), Stage::veth_creation, move_result.cause);
    }
    if (!std::holds_alternative<PlacedVethProof>(state->veth_root)) {
        return preparation_failure(
            std::move(state), Stage::veth_creation, Cause::identity_unavailable);
    }

    NamespaceProof& namespace_proof{
        std::get<NamespaceProof>(state->namespace_root)};
    PlacedVethProof& placed_veth{
        std::get<PlacedVethProof>(state->veth_root)};
    const auto run_configuration = [&](
                                       const Stage stage,
                                       auto operation) -> std::optional<PreparationResult> {
        if (setup_expired()) {
            return preparation_failure(std::move(state), stage, Cause::timeout);
        }
        const OperationResult result{operation(deadline())};
        if (!result.succeeded) {
            return preparation_failure(std::move(state), stage, result.cause);
        }
        return std::nullopt;
    };

    if (auto failure{run_configuration(
            Stage::host_configuration,
            [&](const TimePoint value) {
                return state->runtime->adapter->assign_host_address(
                    placed_veth, value);
            })}) {
        return std::move(*failure);
    }
    if (auto failure{run_configuration(
            Stage::host_configuration,
            [&](const TimePoint value) {
                return state->runtime->adapter->bring_host_link_up(
                    placed_veth, value);
            })}) {
        return std::move(*failure);
    }
    if (auto failure{run_configuration(
            Stage::namespace_configuration,
            [&](const TimePoint value) {
                return state->runtime->adapter->bring_loopback_up(
                    namespace_proof, value);
            })}) {
        return std::move(*failure);
    }
    if (auto failure{run_configuration(
            Stage::namespace_configuration,
            [&](const TimePoint value) {
                return state->runtime->adapter->assign_namespace_address(
                    namespace_proof, placed_veth, value);
            })}) {
        return std::move(*failure);
    }
    if (auto failure{run_configuration(
            Stage::namespace_configuration,
            [&](const TimePoint value) {
                return state->runtime->adapter->bring_namespace_link_up(
                    namespace_proof, placed_veth, value);
            })}) {
        return std::move(*failure);
    }
    if (auto failure{run_configuration(
            Stage::route_configuration,
            [&](const TimePoint value) {
                return state->runtime->adapter->add_default_route(
                    namespace_proof, placed_veth, value);
            })}) {
        return std::move(*failure);
    }
    return detail::OwnerAccess::prepared(std::move(state));
}

namespace {

[[nodiscard]] PreparationResult prepare_production_network_environment(
    std::unique_ptr<detail::PreflightPlatform> platform,
    std::shared_ptr<testing::ProductionTrace> trace = {})
{
    detail::HostLockResult lock{platform->acquire_host_lock()};
    if (!lock.lock) {
        return PreparationFailure{
            {Stage::preflight, lock.cause},
            {},
            std::nullopt,
        };
    }
    auto runtime{std::make_unique<detail::PreparationRuntime>(
        detail::PreparationRuntime{
            std::make_unique<ProductionAdapter>(
                std::move(platform), std::move(trace)),
            std::make_unique<SystemClock>(),
            std::move(lock.lock),
        })};
    return prepare_network_environment(
        detail::PreparationAccess::make(std::move(runtime)));
}

} // namespace

PreparationResult prepare_network_environment()
{
    return prepare_production_network_environment(
        detail::make_linux_preflight_platform());
}

namespace testing {

PreparationResult prepare_scripted_network_environment(
    std::vector<ScriptStep> script,
    std::shared_ptr<SharedTrace> trace,
    const TimePoint start,
    const bool track_host_lock)
{
    auto script_state{std::make_shared<ScriptState>(ScriptState{
        start,
        std::deque<ScriptStep>{script.begin(), script.end()},
        std::move(trace),
    })};
    auto runtime{std::make_unique<detail::PreparationRuntime>(
        detail::PreparationRuntime{
            std::make_unique<ScriptedAdapter>(script_state),
            std::make_unique<ScriptedClock>(script_state),
            track_host_lock
                ? std::make_unique<TrackedHostLock>(script_state->trace)
                : nullptr,
        })};
    return prepare_network_environment(
        detail::PreparationAccess::make(std::move(runtime)));
}

PreparationResult prepare_with_preflight_platform(
    std::unique_ptr<detail::PreflightPlatform> platform,
    std::shared_ptr<ProductionTrace> trace)
{
    return prepare_production_network_environment(
        std::move(platform), std::move(trace));
}

} // namespace testing
} // namespace netlaglab::network_environment
