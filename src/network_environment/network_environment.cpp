#include "network_environment.hpp"

#include "preflight.hpp"
#include "production_adapter.hpp"
#include "production_test_support.hpp"
#include "transaction_test_support.hpp"
#include "recovery_journal.hpp"
#include "file_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <unistd.h>

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
    explicit NamespaceProof(std::unique_ptr<detail::NamespaceHandle> exact_handle)
        : exact_handle_{std::move(exact_handle)}
    {
    }

    NamespaceProof(NamespaceProof&&) noexcept = default;
    NamespaceProof& operator=(NamespaceProof&&) noexcept = default;
    NamespaceProof(const NamespaceProof&) = delete;
    NamespaceProof& operator=(const NamespaceProof&) = delete;

private:
    std::unique_ptr<detail::NamespaceHandle> exact_handle_;

    friend class ProductionAdapter;
    friend struct detail::OwnerAccess;
};

struct VethIdentityPair {
    detail::LinkIdentity host;
    detail::LinkIdentity session;

    friend bool operator==(const VethIdentityPair&, const VethIdentityPair&) = default;
};

class HostVethProof {
public:
    explicit HostVethProof(std::unique_ptr<VethIdentityPair> identity)
        : identity_{std::move(identity)}
    {
    }

    HostVethProof(HostVethProof&&) noexcept = default;
    HostVethProof& operator=(HostVethProof&&) noexcept = default;
    HostVethProof(const HostVethProof&) = delete;
    HostVethProof& operator=(const HostVethProof&) = delete;

    [[nodiscard]] std::unique_ptr<VethIdentityPair> release_identity()
    {
        return std::move(identity_);
    }

private:
    std::unique_ptr<VethIdentityPair> identity_;

    friend class ProductionAdapter;
};

class PlacedVethProof {
public:
    explicit PlacedVethProof(std::unique_ptr<VethIdentityPair> identity)
        : identity_{std::move(identity)}
    {
    }

    PlacedVethProof(PlacedVethProof&&) noexcept = default;
    PlacedVethProof& operator=(PlacedVethProof&&) noexcept = default;
    PlacedVethProof(const PlacedVethProof&) = delete;
    PlacedVethProof& operator=(const PlacedVethProof&) = delete;

private:
    std::unique_ptr<VethIdentityPair> identity_;

    friend class ProductionAdapter;
};

class ConnectivityProof {
public:
    explicit ConnectivityProof(
        std::unique_ptr<detail::ConnectivityHandle> handle)
        : handle_{std::move(handle)}
    {
    }

    ConnectivityProof(ConnectivityProof&&) noexcept = default;
    ConnectivityProof& operator=(ConnectivityProof&&) noexcept = default;
    ConnectivityProof(const ConnectivityProof&) = delete;
    ConnectivityProof& operator=(const ConnectivityProof&) = delete;

private:
    std::unique_ptr<detail::ConnectivityHandle> handle_;

    friend class ProductionAdapter;
};

using NamespaceState =
    std::variant<std::monostate, NamespaceProof, IdentityUnconfirmed>;
using VethState = std::variant<
    std::monostate,
    HostVethProof,
    PlacedVethProof,
    IdentityUnconfirmed>;
using ProvenVeth = std::variant<HostVethProof, PlacedVethProof>;
using ConnectivityState =
    std::variant<std::monostate, ConnectivityProof, IdentityUnconfirmed>;

struct ConnectivityMutationResult {
    bool succeeded;
    Cause cause;
    ConnectivityState state;
};

struct ConnectivityRemovalResult {
    Cause cause;
    ConnectivityState state;
};

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
    [[nodiscard]] virtual ConnectivityMutationResult configure_connectivity(
        TimePoint deadline) = 0;
    [[nodiscard]] virtual ConnectivityRemovalResult remove_connectivity(
        ConnectivityProof proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual VethRemovalResult remove_veth(
        ProvenVeth proof,
        const NamespaceProof* namespace_proof,
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

class ScriptedNamespaceHandle final : public detail::NamespaceHandle {
public:
    explicit ScriptedNamespaceHandle(const std::uint64_t identity)
        : identity_{identity}
    {
    }

    [[nodiscard]] detail::NamespaceIdentity identity() const override
    {
        return {1, identity_};
    }

private:
    std::uint64_t identity_;
};

class ScriptedConnectivityHandle final : public detail::ConnectivityHandle {
};

class ProductionConnectivityHandle final : public detail::ConnectivityHandle {
public:
    ProductionConnectivityHandle(
        netlaglab::FileDescriptor journal_directory,
        const detail::RecoveryRecord recovery_record,
        std::string table)
        : journal_directory_{std::move(journal_directory)}
        , recovery_record_{recovery_record}
        , table_{std::move(table)}
    {
    }

    netlaglab::FileDescriptor journal_directory_;
    detail::RecoveryRecord recovery_record_;
    std::string table_;
    std::string nft_path;
    std::string firewall_path;
    std::string firewall_zone;
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
            state.emplace<NamespaceProof>(std::make_unique<ScriptedNamespaceHandle>(
                state_->next_identity++));
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

    [[nodiscard]] ConnectivityMutationResult configure_connectivity(
        const TimePoint deadline) override
    {
        if (state_->steps.empty()
            || state_->steps.front().operation != Operation::configure_connectivity) {
            configured_connectivity_ = true;
            return {
                true,
                Cause::system_failure,
                ConnectivityProof{std::make_unique<ScriptedConnectivityHandle>()},
            };
        }
        const ScriptStep step{next(Operation::configure_connectivity, deadline)};
        ConnectivityState state;
        if (step.outcome == Outcome::success
            || step.outcome == Outcome::fail_new_state) {
            configured_connectivity_ = true;
            state.emplace<ConnectivityProof>(
                std::make_unique<ScriptedConnectivityHandle>());
        } else if (step.outcome == Outcome::fail_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.outcome == Outcome::success, step.cause, std::move(state)};
    }

    [[nodiscard]] ConnectivityRemovalResult remove_connectivity(
        ConnectivityProof,
        const TimePoint deadline) override
    {
        if (!configured_connectivity_) {
            return {Cause::system_failure, {}};
        }
        if (state_->steps.empty()
            || state_->steps.front().operation != Operation::remove_connectivity) {
            configured_connectivity_ = false;
            return {Cause::system_failure, {}};
        }
        const ScriptStep step{next(Operation::remove_connectivity, deadline)};
        ConnectivityState state;
        if (step.outcome == Outcome::cleanup_retained) {
            state.emplace<ConnectivityProof>(
                std::make_unique<ScriptedConnectivityHandle>());
        } else if (step.outcome == Outcome::cleanup_identity_mismatch
                   || step.outcome == Outcome::cleanup_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        } else {
            configured_connectivity_ = false;
        }
        return {step.cause, std::move(state)};
    }

    [[nodiscard]] VethRemovalResult remove_veth(
        ProvenVeth proof,
        const NamespaceProof*,
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
    [[nodiscard]] std::unique_ptr<VethIdentityPair> identity()
    {
        const std::uint32_t host{
            static_cast<std::uint32_t>(state_->next_identity++)};
        const std::uint32_t session{
            static_cast<std::uint32_t>(state_->next_identity++)};
        return std::make_unique<VethIdentityPair>(VethIdentityPair{
            {host, session, std::nullopt},
            {session, host, std::nullopt},
        });
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
    bool configured_connectivity_{};
};

struct TrustedToolLookup {
    bool query_succeeded{true};
    std::optional<std::string> path;
};

[[nodiscard]] TrustedToolLookup find_trusted_tool(
    detail::PreflightPlatform& platform,
    const std::string_view tool)
{
    std::array<std::string_view, 4> paths{};
    if (tool == "nft") {
        paths = {"/usr/sbin/nft", "/usr/bin/nft", "/sbin/nft", "/bin/nft"};
    } else if (tool == "ufw") {
        paths = {"/usr/sbin/ufw", "/usr/bin/ufw", "/sbin/ufw", "/bin/ufw"};
    } else if (tool == "firewall-cmd") {
        paths = {"/usr/bin/firewall-cmd", "/usr/sbin/firewall-cmd",
                 "/bin/firewall-cmd", "/sbin/firewall-cmd"};
    } else {
        return {};
    }
    for (const std::string_view path : paths) {
        const detail::ToolQuery query{platform.query_tool(path)};
        if (query.status == detail::QueryStatus::failure) {
            return {false, std::nullopt};
        }
        if (query.status == detail::QueryStatus::present
            && detail::is_trusted_executable(query.metadata)) {
            return {true, std::string{path}};
        }
    }
    return {true, std::nullopt};
}

class ProductionAdapter final : public SemanticAdapter {
public:
    explicit ProductionAdapter(
        std::unique_ptr<detail::PreflightPlatform> platform,
        std::unique_ptr<detail::ProductionPlatform> production,
        std::shared_ptr<testing::ProductionTrace> trace = {},
        const bool reconcile_recovery = true)
        : platform_{std::move(platform)}
        , production_{std::move(production)}
        , trace_{std::move(trace)}
        , reconcile_recovery_{reconcile_recovery}
    {
    }

    [[nodiscard]] OperationResult preflight(const TimePoint deadline) override
    {
        if (!platform_->privileged()) {
            return {false, Cause::system_failure};
        }
        if (reconcile_recovery_) {
            const OperationResult reconciled{reconcile_pending_state(deadline)};
            if (!reconciled.succeeded) {
                return reconciled;
            }
        }

        detail::PreflightResult result{
            detail::run_preflight(*platform_, deadline)};
        if (!result.succeeded) {
            return {false, result.cause};
        }
        ip_path_ = std::move(result.ip_path);
        return {true, Cause::system_failure};
    }

    [[nodiscard]] NamespaceMutationResult create_namespace(
        const TimePoint deadline) override
    {
        if (trace_) {
            ++trace_->semantic_mutation_requests;
        }
        const std::vector<std::string> arguments{"netns", "add", "netlaglab"};
        const CommandResult command{production_->run_ip(
            ip_path_, arguments, nullptr, deadline)};
        detail::NamespaceQuery query{production_->query_namespace()};
        NamespaceState state;
        if (query.status == detail::InventoryStatus::present && query.handle) {
            state.emplace<NamespaceProof>(std::move(query.handle));
            return {
                command.kind == CommandResultKind::success,
                command_cause(command),
                std::move(state),
            };
        }
        if (query.status != detail::InventoryStatus::absent) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {
            false,
            query.status == detail::InventoryStatus::absent
                    && command.kind != CommandResultKind::success
                ? command_cause(command)
                : Cause::identity_unavailable,
            std::move(state),
        };
    }

    [[nodiscard]] VethMutationResult create_veth(
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "link", "add", "nll-host", "type", "veth", "peer", "name", "nll-app"};
        const CommandResult command{production_->run_ip(
            ip_path_, arguments, nullptr, deadline)};
        const detail::LinkQuery host{
            production_->query_host_link("nll-host", deadline)};
        const detail::LinkQuery session{
            production_->query_host_link("nll-app", deadline)};
        VethState state;
        if (valid_host_pair(host, session)) {
            state.emplace<HostVethProof>(std::make_unique<VethIdentityPair>(
                VethIdentityPair{*host.identity, *session.identity}));
            return {
                command.kind == CommandResultKind::success,
                command_cause(command),
                std::move(state),
            };
        }
        if (host.status != detail::InventoryStatus::absent
            || session.status != detail::InventoryStatus::absent) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {
            false,
            host.status == detail::InventoryStatus::absent
                    && session.status == detail::InventoryStatus::absent
                    && command.kind != CommandResultKind::success
                ? command_cause(command)
                : Cause::identity_unavailable,
            std::move(state),
        };
    }

    [[nodiscard]] VethMutationResult move_peer(
        const NamespaceProof& namespace_proof,
        HostVethProof proof,
        const TimePoint deadline) override
    {
        const std::string namespace_file{
            production_->namespace_file_argument(*namespace_proof.exact_handle_)};
        const std::vector<std::string> arguments{
            "link", "set", "dev", "nll-app", "netns", namespace_file};
        const CommandResult command{production_->run_ip(
            ip_path_, arguments, namespace_proof.exact_handle_.get(), deadline)};
        const detail::LinkQuery host{
            production_->query_host_link("nll-host", deadline)};
        const detail::LinkQuery old_session{
            production_->query_host_link("nll-app", deadline)};
        const detail::LinkQuery placed_session{
            production_->query_namespace_link(
                *namespace_proof.exact_handle_, "nll-app", deadline)};
        VethState state;
        if (valid_placed_pair(host, old_session, placed_session)) {
            state.emplace<PlacedVethProof>(std::make_unique<VethIdentityPair>(
                VethIdentityPair{*host.identity, *placed_session.identity}));
            return {
                command.kind == CommandResultKind::success,
                command_cause(command),
                std::move(state),
            };
        }
        if (valid_host_pair(host, old_session)
            && placed_session.status == detail::InventoryStatus::absent
            && proof_matches(proof, host, old_session)) {
            state.emplace<HostVethProof>(std::move(proof));
            return {
                false,
                command.kind == CommandResultKind::success
                    ? Cause::identity_mismatch
                    : command_cause(command),
                std::move(state),
            };
        }
        if (host.status != detail::InventoryStatus::absent
            || old_session.status != detail::InventoryStatus::absent
            || placed_session.status != detail::InventoryStatus::absent) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {
            false,
            all_absent(host, old_session, placed_session)
                    && command.kind != CommandResultKind::success
                ? command_cause(command)
                : Cause::identity_unavailable,
            std::move(state),
        };
    }

    [[nodiscard]] OperationResult assign_host_address(
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "address", "add", "10.200.0.1/30", "dev", "nll-host"};
        return command_result(
            production_->run_ip(ip_path_, arguments, nullptr, deadline));
    }

    [[nodiscard]] OperationResult bring_host_link_up(
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "link", "set", "dev", "nll-host", "up"};
        return command_result(
            production_->run_ip(ip_path_, arguments, nullptr, deadline));
    }

    [[nodiscard]] OperationResult bring_loopback_up(
        const NamespaceProof& namespace_proof,
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "link", "set", "dev", "lo", "up"};
        return command_result(production_->run_ip_in_namespace(
            ip_path_, arguments, *namespace_proof.exact_handle_, deadline));
    }

    [[nodiscard]] OperationResult assign_namespace_address(
        const NamespaceProof& namespace_proof,
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "address", "add", "10.200.0.2/30", "dev", "nll-app"};
        return command_result(production_->run_ip_in_namespace(
            ip_path_, arguments, *namespace_proof.exact_handle_, deadline));
    }

    [[nodiscard]] OperationResult bring_namespace_link_up(
        const NamespaceProof& namespace_proof,
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "link", "set", "dev", "nll-app", "up"};
        return command_result(production_->run_ip_in_namespace(
            ip_path_, arguments, *namespace_proof.exact_handle_, deadline));
    }

    [[nodiscard]] OperationResult add_default_route(
        const NamespaceProof& namespace_proof,
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const std::vector<std::string> arguments{
            "route",
            "add",
            "default",
            "via",
            "10.200.0.1",
            "dev",
            "nll-app",
        };
        return command_result(production_->run_ip_in_namespace(
            ip_path_, arguments, *namespace_proof.exact_handle_, deadline));
    }

    [[nodiscard]] ConnectivityMutationResult configure_connectivity(
        const TimePoint deadline) override
    {
        const std::optional<bool> forwarding{
            production_->ipv4_forwarding_enabled()};
        const std::optional<bool> legacy_rules{
            production_->legacy_iptables_rules_present()};
        if (!forwarding.has_value() || !legacy_rules.has_value()) {
            return {false, Cause::system_failure, {}};
        }
        if (!*forwarding) {
            return {false, Cause::unsupported_host_configuration, {}};
        }

        const TrustedToolLookup nft{find_trusted_tool(*platform_, "nft")};
        if (!nft.query_succeeded || !nft.path.has_value()) {
            return {false, Cause::unavailable_or_invalid_tool, {}};
        }
        const TrustedToolLookup ufw{find_trusted_tool(*platform_, "ufw")};
        const TrustedToolLookup firewalld{
            find_trusted_tool(*platform_, "firewall-cmd")};
        if (!ufw.query_succeeded || !firewalld.query_succeeded) {
            return {false, Cause::system_failure, {}};
        }
        const auto& nft_path{nft.path};
        const auto& ufw_path{ufw.path};
        const auto& firewalld_path{firewalld.path};
        const std::optional<bool> ufw_status{ufw_path.has_value()
                ? inspect_ufw(*ufw_path, deadline)
                : std::optional<bool>{false}};
        const std::optional<bool> firewalld_status{firewalld_path.has_value()
                ? inspect_firewalld(*firewalld_path, deadline)
                : std::optional<bool>{false}};
        if (!ufw_status.has_value() || !firewalld_status.has_value()
            || (*ufw_status && *firewalld_status)) {
            return {false, Cause::unsupported_host_configuration, {}};
        }
        const bool ufw_active{*ufw_status};
        const bool firewalld_active{*firewalld_status};
        const std::vector<std::string> list_ruleset_arguments{"list", "ruleset"};
        const CommandResult nft_ruleset{production_->run_tool(
            *nft_path, list_ruleset_arguments, deadline)};
        if (nft_ruleset.kind != CommandResultKind::success) {
            return {false, command_cause(nft_ruleset), {}};
        }
        if (!ufw_active && !firewalld_active
            && (*legacy_rules
                || nft_ruleset.standard_output.find("hook forward")
                    != std::string::npos)) {
            return {false, Cause::unsupported_host_configuration, {}};
        }

        const auto token{detail::create_recovery_token()};
        if (!token.has_value()) {
            return {false, Cause::system_failure, {}};
        }
        const detail::RecoveryBackend backend{ufw_active
                ? detail::RecoveryBackend::ufw
                : firewalld_active ? detail::RecoveryBackend::firewalld
                                   : detail::RecoveryBackend::nftables};
        const detail::RecoveryRecord record{
            backend, detail::RecoveryPhase::intent, *token};
        const std::string table{"netlaglab_" + *token};
        const std::string ufw_rule{
            "route allow in on nll-host from 10.200.0.2 comment netlaglab-" + *token};
        std::string consent_description{ufw_rule};
        std::optional<std::string> firewalld_zone;
        if (firewalld_active) {
            const std::string policy{"netlaglab-" + *token};
            firewalld_zone = firewalld_ingress_zone(*firewalld_path, deadline);
            if (!firewalld_zone.has_value()) {
                return {false, Cause::unsupported_host_configuration, {}};
            }
            consent_description =
                "firewalld policy " + policy + " from zone " + *firewalld_zone
                + " for IPv4 source 10.200.0.2/32 to any routed destination";
        }
        if ((ufw_active || firewalld_active)
            && !production_->request_firewall_consent(
                consent_description, deadline)) {
            return {false, Cause::unsupported_host_configuration, {}};
        }

        const int raw_directory{production_->open_recovery_directory()};
        if (raw_directory == -1) {
            return {false, Cause::system_failure, {}};
        }
        netlaglab::FileDescriptor directory{raw_directory};
        const detail::RecoveryReadResult existing{
            detail::read_recovery_record(directory.get(), ::geteuid())};
        if (existing.status != detail::RecoveryReadStatus::empty) {
            return {false, Cause::unsupported_host_configuration, {}};
        }
        if (!detail::write_recovery_record(directory.get(), ::geteuid(), record)) {
            return {false, Cause::system_failure, {}};
        }
        auto handle{std::make_unique<ProductionConnectivityHandle>(
            std::move(directory), record, table)};
        const auto state_with_handle = [&]() {
            ConnectivityState state;
            state.emplace<ConnectivityProof>(
                std::unique_ptr<detail::ConnectivityHandle>{handle.release()});
            return state;
        };

        const std::vector<std::string> add_table_arguments{
            "add", "table", "ip", table};
        const CommandResult table_result{production_->run_tool(
            *nft_path, add_table_arguments, deadline)};
        if (table_result.kind != CommandResultKind::success) {
            return {false, command_cause(table_result), state_with_handle()};
        }
        handle->nft_path = *nft_path;
        const std::vector<std::string> chain_arguments{
            "add", "chain", "ip", table, "postrouting", "{", "type", "nat",
            "hook", "postrouting", "priority", "srcnat", ";", "policy", "accept", ";", "}"};
        const CommandResult chain_result{
            production_->run_tool(*nft_path, chain_arguments, deadline)};
        if (chain_result.kind != CommandResultKind::success) {
            return {false, command_cause(chain_result), state_with_handle()};
        }
        const std::vector<std::string> rule_arguments{
            "add", "rule", "ip", table, "postrouting", "ip", "saddr",
            "10.200.0.2/32", "counter", "masquerade", "comment",
            "\"netlaglab:" + *token + "\""};
        const CommandResult nat_result{
            production_->run_tool(*nft_path, rule_arguments, deadline)};
        if (nat_result.kind != CommandResultKind::success) {
            return {false, command_cause(nat_result), state_with_handle()};
        }
        if (ufw_active) {
            handle->firewall_path = *ufw_path;
            const std::vector<std::string> arguments{
                "route", "allow", "in", "on", "nll-host", "from",
                "10.200.0.2", "comment", "netlaglab-" + *token};
            const CommandResult firewall_result{
                production_->run_tool(*ufw_path, arguments, deadline)};
            const std::optional<bool> ufw_present{
                ufw_rule_present(*ufw_path, *token, deadline)};
            if (firewall_result.kind != CommandResultKind::success
                || !ufw_present.has_value() || !*ufw_present) {
                return {
                    false,
                    firewall_result.kind == CommandResultKind::success
                        ? Cause::identity_unavailable
                        : command_cause(firewall_result),
                    state_with_handle(),
                };
            }
        } else if (firewalld_active) {
            handle->firewall_path = *firewalld_path;
            handle->firewall_zone = *firewalld_zone;
            const OperationResult firewall_result{
                add_firewalld_policy(*handle, deadline)};
            if (!firewall_result.succeeded) {
                return {false, firewall_result.cause, state_with_handle()};
            }
        }
        detail::RecoveryRecord applied{record};
        applied.phase = detail::RecoveryPhase::applied;
        if (!detail::write_recovery_record(
                handle->journal_directory_.get(), ::geteuid(), applied)) {
            return {false, Cause::system_failure, state_with_handle()};
        }
        handle->recovery_record_ = std::move(applied);
        return {true, Cause::system_failure, state_with_handle()};
    }

    [[nodiscard]] ConnectivityRemovalResult remove_connectivity(
        ConnectivityProof proof,
        const TimePoint deadline) override
    {
        auto* handle{dynamic_cast<ProductionConnectivityHandle*>(proof.handle_.get())};
        if (handle == nullptr) {
            ConnectivityState state;
            state.emplace<IdentityUnconfirmed>();
            return {Cause::identity_unavailable, std::move(state)};
        }
        const detail::RecoveryReadResult current{detail::read_recovery_record(
            handle->journal_directory_.get(), ::geteuid())};
        if (current.status == detail::RecoveryReadStatus::empty) {
            return {Cause::system_failure, {}};
        }
        if (current.status != detail::RecoveryReadStatus::valid
            || !current.record.has_value()
            || current.record->token != handle->recovery_record_.token
            || current.record->backend != handle->recovery_record_.backend) {
            ConnectivityState state;
            state.emplace<IdentityUnconfirmed>();
            return {Cause::identity_mismatch, std::move(state)};
        }
        detail::RecoveryRecord removing{*current.record};
        removing.phase = detail::RecoveryPhase::removing;
        if (!detail::write_recovery_record(
                handle->journal_directory_.get(), ::geteuid(), removing)) {
            ConnectivityState state;
            state.emplace<ConnectivityProof>(std::move(proof));
            return {Cause::system_failure, std::move(state)};
        }

        if (!handle->firewall_path.empty()) {
            const OperationResult firewall_cleanup{remove_firewall(*handle, deadline)};
            if (!firewall_cleanup.succeeded) {
                ConnectivityState state;
                state.emplace<ConnectivityProof>(std::move(proof));
                return {firewall_cleanup.cause, std::move(state)};
            }
        }
        const auto nft_path{handle->nft_path.empty()
                ? tool_path("nft")
                : std::optional<std::string>{handle->nft_path}};
        if (!nft_path.has_value()) {
            ConnectivityState state;
            state.emplace<ConnectivityProof>(std::move(proof));
            return {Cause::unavailable_or_invalid_tool, std::move(state)};
        }
        const std::vector<std::string> list_table_arguments{
            "list", "table", "ip", handle->table_};
        const CommandResult table_query{production_->run_tool(
            *nft_path, list_table_arguments, deadline)};
        if (table_query.kind == CommandResultKind::success) {
            if (table_query.standard_output.find(handle->table_) == std::string::npos) {
                ConnectivityState state;
                state.emplace<IdentityUnconfirmed>();
                return {Cause::identity_mismatch, std::move(state)};
            }
            const std::vector<std::string> delete_table_arguments{
                "delete", "table", "ip", handle->table_};
            const CommandResult deletion{production_->run_tool(
                *nft_path, delete_table_arguments, deadline)};
            if (deletion.kind != CommandResultKind::success) {
                ConnectivityState state;
                state.emplace<ConnectivityProof>(std::move(proof));
                return {command_cause(deletion), std::move(state)};
            }
            const CommandResult after_deletion{production_->run_tool(
                *nft_path, list_table_arguments, deadline)};
            if (!known_nft_table_absence(after_deletion)) {
                ConnectivityState state;
                state.emplace<ConnectivityProof>(std::move(proof));
                return {
                    after_deletion.kind == CommandResultKind::success
                        ? Cause::identity_mismatch
                        : command_cause(after_deletion),
                    std::move(state),
                };
            }
        } else if (!known_nft_table_absence(table_query)) {
            ConnectivityState state;
            state.emplace<ConnectivityProof>(std::move(proof));
            return {command_cause(table_query), std::move(state)};
        }
        if (!detail::clear_recovery_record(
                handle->journal_directory_.get(), ::geteuid())) {
            ConnectivityState state;
            state.emplace<ConnectivityProof>(std::move(proof));
            return {Cause::system_failure, std::move(state)};
        }
        return {Cause::system_failure, {}};
    }

    [[nodiscard]] VethRemovalResult remove_veth(
        ProvenVeth proof,
        const NamespaceProof* namespace_proof,
        const TimePoint deadline) override
    {
        return remove_proven_veth(
            std::move(proof), namespace_proof, deadline);
    }

    [[nodiscard]] NamespaceRemovalResult remove_namespace(
        NamespaceProof proof,
        const TimePoint deadline) override
    {
        detail::NamespaceQuery before{production_->query_namespace()};
        if (before.status == detail::InventoryStatus::absent) {
            return {Cause::system_failure, {}};
        }
        if (before.status != detail::InventoryStatus::present || !before.handle) {
            NamespaceState state;
            state.emplace<IdentityUnconfirmed>();
            return {Cause::identity_unavailable, std::move(state)};
        }
        if (before.handle->identity() != proof.exact_handle_->identity()) {
            NamespaceState state;
            state.emplace<IdentityUnconfirmed>();
            return {Cause::identity_mismatch, std::move(state)};
        }

        const std::vector<std::string> arguments{"netns", "delete", "netlaglab"};
        const CommandResult command{production_->run_ip(
            ip_path_, arguments, nullptr, deadline)};
        detail::NamespaceQuery after{production_->query_namespace()};
        if (after.status == detail::InventoryStatus::absent) {
            return {Cause::system_failure, {}};
        }
        NamespaceState state;
        if (after.status == detail::InventoryStatus::present && after.handle
            && after.handle->identity() == proof.exact_handle_->identity()) {
            state.emplace<NamespaceProof>(std::move(proof));
            return {
                command.kind == CommandResultKind::success
                    ? Cause::identity_mismatch
                    : command_cause(command),
                std::move(state),
            };
        }
        state.emplace<IdentityUnconfirmed>();
        return {
            after.status == detail::InventoryStatus::present
                ? Cause::identity_mismatch
                : Cause::identity_unavailable,
            std::move(state),
        };
    }

private:
    [[nodiscard]] OperationResult reconcile_pending_state(const TimePoint deadline)
    {
        const int raw_directory{production_->open_recovery_directory()};
        if (raw_directory == -1) {
            return {false, Cause::system_failure};
        }
        netlaglab::FileDescriptor directory{raw_directory};
        const detail::RecoveryReadResult recovery{
            detail::read_recovery_record(directory.get(), ::geteuid())};
        if (recovery.status == detail::RecoveryReadStatus::empty) {
            return {true, Cause::system_failure};
        }
        if (recovery.status != detail::RecoveryReadStatus::valid
            || !recovery.record.has_value()) {
            return {false, Cause::unsupported_host_configuration};
        }
        const TrustedToolLookup nft{find_trusted_tool(*platform_, "nft")};
        if (!nft.query_succeeded || !nft.path.has_value()) {
            return {false, Cause::unavailable_or_invalid_tool};
        }
        const std::string table{"netlaglab_" + recovery.record->token};
        ConnectivityProof proof{std::make_unique<ProductionConnectivityHandle>(
            std::move(directory), *recovery.record, table)};
        auto* handle{
            dynamic_cast<ProductionConnectivityHandle*>(proof.handle_.get())};
        handle->nft_path = *nft.path;
        if (recovery.record->backend == detail::RecoveryBackend::ufw) {
            const TrustedToolLookup ufw{find_trusted_tool(*platform_, "ufw")};
            if (!ufw.query_succeeded || !ufw.path.has_value()) {
                return {false, Cause::unavailable_or_invalid_tool};
            }
            handle->firewall_path = *ufw.path;
        } else if (recovery.record->backend == detail::RecoveryBackend::firewalld) {
            const TrustedToolLookup firewalld{
                find_trusted_tool(*platform_, "firewall-cmd")};
            if (!firewalld.query_succeeded || !firewalld.path.has_value()) {
                return {false, Cause::unavailable_or_invalid_tool};
            }
            handle->firewall_path = *firewalld.path;
        }
        const ConnectivityRemovalResult reconciled{
            remove_connectivity(std::move(proof), deadline)};
        return std::holds_alternative<std::monostate>(reconciled.state)
            ? OperationResult{true, Cause::system_failure}
            : OperationResult{false, reconciled.cause};
    }

    [[nodiscard]] std::optional<std::string> tool_path(
        const std::string_view name)
    {
        TrustedToolLookup result{find_trusted_tool(*platform_, name)};
        return result.query_succeeded ? std::move(result.path) : std::nullopt;
    }

    [[nodiscard]] std::optional<bool> inspect_ufw(
        const std::string_view path,
        const TimePoint deadline)
    {
        const std::vector<std::string> arguments{"status"};
        const CommandResult result{production_->run_tool(path, arguments, deadline)};
        if (result.kind != CommandResultKind::success) {
            return std::nullopt;
        }
        if (result.standard_output.find("Status: active") != std::string::npos) {
            return true;
        }
        if (result.standard_output.find("Status: inactive") != std::string::npos) {
            return false;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<bool> inspect_firewalld(
        const std::string_view path,
        const TimePoint deadline)
    {
        const std::vector<std::string> arguments{"--state"};
        const CommandResult result{production_->run_tool(path, arguments, deadline)};
        if (result.kind == CommandResultKind::success
            && trim_ascii(result.standard_output) == "running") {
            return true;
        }
        if (trim_ascii(result.standard_output) == "not running") {
            return false;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> firewalld_ingress_zone(
        const std::string_view path,
        const TimePoint deadline)
    {
        const std::vector<std::string> arguments{
            "--get-zone-of-interface=nll-host"};
        const CommandResult result{production_->run_tool(path, arguments, deadline)};
        if (result.kind != CommandResultKind::success) {
            return std::nullopt;
        }
        const std::string_view zone{trim_ascii(result.standard_output)};
        if (zone.empty() || zone.size() > 64U) {
            return std::nullopt;
        }
        for (const unsigned char character : zone) {
            if (!(std::isalnum(character) != 0 || character == '_' || character == '-')) {
                return std::nullopt;
            }
        }
        return std::string{zone};
    }

    [[nodiscard]] std::optional<bool> ufw_rule_present(
        const std::string_view path,
        const std::string_view token,
        const TimePoint deadline)
    {
        const std::vector<std::string> arguments{"status", "numbered"};
        const CommandResult result{production_->run_tool(path, arguments, deadline)};
        if (result.kind != CommandResultKind::success) {
            return std::nullopt;
        }
        const std::string marker{"netlaglab-" + std::string{token}};
        const std::string_view output{result.standard_output};
        bool found{};
        std::size_t line_begin{};
        while (line_begin < output.size()) {
            const std::size_t line_end{output.find('\n', line_begin)};
            const std::string_view line{output.substr(
                line_begin,
                line_end == std::string::npos ? output.size() - line_begin
                                              : line_end - line_begin)};
            if (line.find(marker) != std::string_view::npos) {
                if (found || line.find("nll-host") == std::string_view::npos
                    || line.find("10.200.0.2") == std::string_view::npos
                    || line.find("ALLOW FWD") == std::string_view::npos) {
                    return std::nullopt;
                }
                found = true;
            }
            if (line_end == std::string::npos) {
                break;
            }
            line_begin = line_end + 1U;
        }
        return found;
    }

    [[nodiscard]] OperationResult add_firewalld_policy(
        ProductionConnectivityHandle& handle,
        const TimePoint deadline)
    {
        const std::string policy{"netlaglab-" + handle.recovery_record_.token};
        const auto run = [&](std::vector<std::string> arguments) {
            return production_->run_tool(handle.firewall_path, arguments, deadline);
        };
        const CommandResult created{run({"--new-policy=" + policy})};
        if (created.kind != CommandResultKind::success) {
            return {false, command_cause(created)};
        }
        const CommandResult ingress{run({
            "--policy=" + policy,
            "--add-ingress-zone=" + handle.firewall_zone,
        })};
        if (ingress.kind != CommandResultKind::success) {
            return {false, command_cause(ingress)};
        }
        const CommandResult egress{run({
            "--policy=" + policy,
            "--add-egress-zone=ANY",
        })};
        if (egress.kind != CommandResultKind::success) {
            return {false, command_cause(egress)};
        }
        const CommandResult rule{run({
            "--policy=" + policy,
            "--add-rich-rule=rule family=\"ipv4\" source address=\"10.200.0.2/32\" accept",
        })};
        if (rule.kind != CommandResultKind::success) {
            return {false, command_cause(rule)};
        }
        const CommandResult inspection{run({"--info-policy=" + policy})};
        if (inspection.kind != CommandResultKind::success
            || !firewalld_policy_matches(
                inspection.standard_output, handle.firewall_zone)) {
            return {false, Cause::identity_unavailable};
        }
        return {true, Cause::system_failure};
    }

    [[nodiscard]] OperationResult remove_firewall(
        const ProductionConnectivityHandle& handle,
        const TimePoint deadline)
    {
        if (handle.recovery_record_.backend == detail::RecoveryBackend::ufw) {
            const std::optional<bool> present{ufw_rule_present(
                handle.firewall_path,
                handle.recovery_record_.token,
                deadline)};
            if (!present.has_value()) {
                return {false, Cause::identity_unavailable};
            }
            if (!*present) {
                return {true, Cause::system_failure};
            }
            const std::vector<std::string> arguments{
                "route", "delete", "allow", "in", "on", "nll-host", "from",
                "10.200.0.2", "comment",
                "netlaglab-" + handle.recovery_record_.token};
            const CommandResult deletion{production_->run_tool(
                handle.firewall_path, arguments, deadline)};
            if (deletion.kind != CommandResultKind::success) {
                return {false, command_cause(deletion)};
            }
            const std::optional<bool> remains{ufw_rule_present(
                handle.firewall_path,
                handle.recovery_record_.token,
                deadline)};
            return remains.has_value() && !*remains
                ? OperationResult{true, Cause::system_failure}
                : OperationResult{false, Cause::identity_unavailable};
        }
        if (handle.recovery_record_.backend != detail::RecoveryBackend::firewalld) {
            return {true, Cause::system_failure};
        }

        const std::string policy{"netlaglab-" + handle.recovery_record_.token};
        const std::vector<std::string> info_arguments{"--info-policy=" + policy};
        const CommandResult current{production_->run_tool(
            handle.firewall_path, info_arguments, deadline)};
        if (current.kind != CommandResultKind::success) {
            const std::optional<bool> active{
                inspect_firewalld(handle.firewall_path, deadline)};
            if (active.has_value() && !*active) {
                return {true, Cause::system_failure};
            }
            if (current.standard_error.find("INVALID_POLICY") != std::string::npos) {
                return {true, Cause::system_failure};
            }
            return {false, command_cause(current)};
        }
        if (!firewalld_policy_matches(
                current.standard_output, handle.firewall_zone)) {
            return {false, Cause::identity_mismatch};
        }
        const std::vector<std::string> delete_arguments{"--delete-policy=" + policy};
        const CommandResult deletion{production_->run_tool(
            handle.firewall_path, delete_arguments, deadline)};
        if (deletion.kind != CommandResultKind::success) {
            return {false, command_cause(deletion)};
        }
        const CommandResult after_deletion{production_->run_tool(
            handle.firewall_path, info_arguments, deadline)};
        if (after_deletion.kind == CommandResultKind::success) {
            return {false, Cause::identity_mismatch};
        }
        if (after_deletion.standard_error.find("INVALID_POLICY") != std::string::npos) {
            return {true, Cause::system_failure};
        }
        const std::optional<bool> active{
            inspect_firewalld(handle.firewall_path, deadline)};
        return active.has_value() && !*active
            ? OperationResult{true, Cause::system_failure}
            : OperationResult{false, command_cause(after_deletion)};
    }

    [[nodiscard]] static bool firewalld_policy_matches(
        const std::string_view output,
        const std::string_view expected_ingress_zone)
    {
        const auto field_value = [output](const std::string_view field)
            -> std::optional<std::string_view> {
            const std::string key{std::string{field} + ":"};
            const std::size_t field_at{output.find(key)};
            if (field_at == std::string_view::npos) {
                return std::nullopt;
            }
            const std::size_t value_begin{field_at + key.size()};
            const std::size_t value_end{output.find('\n', value_begin)};
            const std::string_view raw{output.substr(
                value_begin,
                value_end == std::string_view::npos
                    ? output.size() - value_begin
                    : value_end - value_begin)};
            return trim_ascii(raw);
        };
        const std::optional<std::string_view> ingress{
            field_value("ingress-zones")};
        const std::optional<std::string_view> egress{field_value("egress-zones")};
        if (!ingress.has_value() || !egress.has_value() || ingress->empty()
            || *egress != "ANY"
            || (!expected_ingress_zone.empty()
                && *ingress != expected_ingress_zone)) {
            return false;
        }
        for (const unsigned char character : *ingress) {
            if (!(std::isalnum(character) != 0 || character == '_' || character == '-')) {
                return false;
            }
        }
        const std::size_t rich_rules_at{output.find("rich rules:")};
        if (rich_rules_at == std::string_view::npos) {
            return false;
        }
        const std::string_view rules{output.substr(rich_rules_at)};
        const std::size_t source_at{rules.find("source address=\"10.200.0.2/32\"")};
        if (source_at == std::string_view::npos) {
            return false;
        }
        const std::size_t rule_begin{rules.rfind("rule ", source_at)};
        const std::size_t rule_end{rules.find('\n', source_at)};
        if (rule_begin == std::string_view::npos) {
            return false;
        }
        const std::string_view owned_rule{rules.substr(
            rule_begin,
            rule_end == std::string_view::npos
                ? rules.size() - rule_begin
                : rule_end - rule_begin)};
        if (owned_rule.find("family=\"ipv4\"") == std::string_view::npos
            || owned_rule.find(" accept") == std::string_view::npos
            || rules.find("rule ", rule_begin + 5U) != std::string_view::npos) {
            return false;
        }
        constexpr std::array<std::string_view, 6> empty_fields{
            "services", "ports", "protocols", "source-ports", "forward-ports",
            "icmp-blocks",
        };
        for (const std::string_view field : empty_fields) {
            const std::optional<std::string_view> value{field_value(field)};
            if (value.has_value() && !value->empty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool known_nft_table_absence(const CommandResult& result)
    {
        return result.kind == CommandResultKind::nonzero_exit
            && (result.standard_error.find("No such file") != std::string::npos
                || result.standard_error.find("does not exist")
                    != std::string::npos);
    }

    [[nodiscard]] static std::string_view trim_ascii(const std::string_view value)
    {
        std::size_t begin{};
        std::size_t end{value.size()};
        while (begin < end && (value[begin] == ' ' || value[begin] == '\t'
                               || value[begin] == '\n' || value[begin] == '\r')) {
            ++begin;
        }
        while (end > begin && (value[end - 1U] == ' ' || value[end - 1U] == '\t'
                               || value[end - 1U] == '\n' || value[end - 1U] == '\r')) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    [[nodiscard]] static OperationResult command_result(
        const CommandResult& result)
    {
        return {
            result.kind == CommandResultKind::success,
            command_cause(result),
        };
    }

    [[nodiscard]] static Cause command_cause(const CommandResult& result)
    {
        switch (result.kind) {
        case CommandResultKind::success:
            return Cause::system_failure;
        case CommandResultKind::nonzero_exit:
            return Cause::command_exit;
        case CommandResultKind::signal:
            return Cause::command_signal;
        case CommandResultKind::timeout:
            return Cause::timeout;
        case CommandResultKind::exec_failure:
            return Cause::unavailable_or_invalid_tool;
        case CommandResultKind::system_failure:
            return Cause::system_failure;
        }
        return Cause::system_failure;
    }

    [[nodiscard]] static bool valid_host_pair(
        const detail::LinkQuery& host,
        const detail::LinkQuery& session)
    {
        return host.status == detail::InventoryStatus::present
            && session.status == detail::InventoryStatus::present
            && host.identity && session.identity
            && host.identity->peer_index == session.identity->index
            && session.identity->peer_index == host.identity->index
            && !host.identity->peer_namespace_id
            && !session.identity->peer_namespace_id;
    }

    [[nodiscard]] static bool valid_placed_pair(
        const detail::LinkQuery& host,
        const detail::LinkQuery& old_session,
        const detail::LinkQuery& placed_session)
    {
        return old_session.status == detail::InventoryStatus::absent
            && host.status == detail::InventoryStatus::present
            && placed_session.status == detail::InventoryStatus::present
            && host.identity && placed_session.identity
            && host.identity->peer_index == placed_session.identity->index
            && placed_session.identity->peer_index == host.identity->index
            && host.identity->peer_namespace_id
            && placed_session.identity->peer_namespace_id
            && *host.identity->peer_namespace_id >= 0
            && *placed_session.identity->peer_namespace_id >= 0;
    }

    [[nodiscard]] static bool proof_matches(
        const HostVethProof& proof,
        const detail::LinkQuery& host,
        const detail::LinkQuery& session)
    {
        return proof.identity_ && host.identity && session.identity
            && proof.identity_->host == *host.identity
            && proof.identity_->session == *session.identity;
    }

    [[nodiscard]] VethRemovalResult remove_proven_veth(
        ProvenVeth proof,
        const NamespaceProof* namespace_proof,
        const TimePoint deadline)
    {
        const bool placed{std::holds_alternative<PlacedVethProof>(proof)};
        if (placed && namespace_proof == nullptr) {
            const detail::LinkQuery host{
                production_->query_host_link("nll-host", deadline)};
            const detail::LinkQuery session{
                production_->query_host_link("nll-app", deadline)};
            if (all_absent(host, session)) {
                return {Cause::system_failure, {}};
            }
            return unconfirmed_veth(inventory_cause(host, session));
        }
        VethObservation before{observe_veth(placed, namespace_proof, deadline)};
        if (before.absent) {
            return {Cause::system_failure, {}};
        }
        if (!before.complete) {
            return unconfirmed_veth(before.cause);
        }
        if (!matches_proof(proof, before)) {
            return unconfirmed_veth(Cause::identity_mismatch);
        }

        const std::vector<std::string> arguments{
            "link", "delete", "dev", "nll-host"};
        const CommandResult command{production_->run_ip(
            ip_path_, arguments, nullptr, deadline)};
        VethObservation after{observe_veth(placed, namespace_proof, deadline)};
        if (after.absent) {
            return {Cause::system_failure, {}};
        }
        if (!after.complete) {
            return unconfirmed_veth(after.cause);
        }
        if (!matches_proof(proof, after)) {
            return unconfirmed_veth(Cause::identity_mismatch);
        }

        VethState state;
        std::visit(
            [&state](auto retained) {
                state.emplace<std::decay_t<decltype(retained)>>(
                    std::move(retained));
            },
            std::move(proof));
        return {
            command.kind == CommandResultKind::success
                ? Cause::identity_mismatch
                : command_cause(command),
            std::move(state),
        };
    }

    struct VethObservation {
        bool absent;
        bool complete;
        Cause cause;
        std::optional<VethIdentityPair> identity;
    };

    [[nodiscard]] VethObservation observe_veth(
        const bool placed,
        const NamespaceProof* namespace_proof,
        const TimePoint deadline)
    {
        const detail::LinkQuery host{
            production_->query_host_link("nll-host", deadline)};
        const detail::LinkQuery host_session{
            production_->query_host_link("nll-app", deadline)};
        if (!placed) {
            if (all_absent(host, host_session)) {
                return {true, true, Cause::system_failure, std::nullopt};
            }
            if (valid_host_pair(host, host_session)) {
                return {
                    false,
                    true,
                    Cause::system_failure,
                    VethIdentityPair{*host.identity, *host_session.identity},
                };
            }
            return {
                false,
                false,
                inventory_cause(host, host_session),
                std::nullopt,
            };
        }

        const detail::LinkQuery session{production_->query_namespace_link(
            *namespace_proof->exact_handle_, "nll-app", deadline)};
        if (all_absent(host, host_session, session)) {
            return {true, true, Cause::system_failure, std::nullopt};
        }
        if (valid_placed_pair(host, host_session, session)) {
            return {
                false,
                true,
                Cause::system_failure,
                VethIdentityPair{*host.identity, *session.identity},
            };
        }
        return {
            false,
            false,
            inventory_cause(host, host_session, session),
            std::nullopt,
        };
    }

    template<typename... Queries>
    [[nodiscard]] static Cause inventory_cause(const Queries&... queries)
    {
        const bool unavailable{
            ((queries.status == detail::InventoryStatus::failure
              || queries.status == detail::InventoryStatus::timeout
              || queries.status == detail::InventoryStatus::malformed)
             || ...)};
        return unavailable ? Cause::identity_unavailable
                           : Cause::identity_mismatch;
    }

    [[nodiscard]] static bool matches_proof(
        const ProvenVeth& proof,
        const VethObservation& observation)
    {
        if (!observation.identity) {
            return false;
        }
        return std::visit(
            [&](const auto& value) {
                return value.identity_
                    && *value.identity_ == *observation.identity;
            },
            proof);
    }

    [[nodiscard]] static VethRemovalResult unconfirmed_veth(const Cause cause)
    {
        VethState state;
        state.emplace<IdentityUnconfirmed>();
        return {cause, std::move(state)};
    }

    template<typename... Queries>
    [[nodiscard]] static bool all_absent(const Queries&... queries)
    {
        return ((queries.status == detail::InventoryStatus::absent) && ...);
    }

    std::unique_ptr<detail::PreflightPlatform> platform_;
    std::unique_ptr<detail::ProductionPlatform> production_;
    std::shared_ptr<testing::ProductionTrace> trace_;
    std::string ip_path_;
    bool reconcile_recovery_;
};

class UnavailableProductionPlatform final : public detail::ProductionPlatform {
public:
    [[nodiscard]] CommandResult run_ip(
        std::string_view,
        std::span<const std::string>,
        const detail::NamespaceHandle*,
        TimePoint) override
    {
        return {CommandResultKind::system_failure, 0, {}};
    }

    [[nodiscard]] CommandResult run_ip_in_namespace(
        std::string_view,
        std::span<const std::string>,
        const detail::NamespaceHandle&,
        TimePoint) override
    {
        return {CommandResultKind::system_failure, 0, {}};
    }

    [[nodiscard]] CommandResult run_tool(
        std::string_view,
        std::span<const std::string>,
        TimePoint) override
    {
        return {CommandResultKind::system_failure, 0, {}};
    }

    [[nodiscard]] std::optional<bool> ipv4_forwarding_enabled() override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<bool> legacy_iptables_rules_present() override
    {
        return std::nullopt;
    }

    [[nodiscard]] bool request_firewall_consent(std::string_view, TimePoint) override
    {
        return false;
    }

    [[nodiscard]] int open_recovery_directory() override { return -1; }

    [[nodiscard]] detail::NamespaceQuery query_namespace() override
    {
        return {detail::InventoryStatus::absent, nullptr};
    }

    [[nodiscard]] std::string namespace_file_argument(
        const detail::NamespaceHandle&) const override
    {
        return {};
    }

    [[nodiscard]] detail::LinkQuery query_host_link(
        std::string_view,
        TimePoint) override
    {
        return {detail::InventoryStatus::absent, std::nullopt};
    }

    [[nodiscard]] detail::LinkQuery query_namespace_link(
        const detail::NamespaceHandle&,
        std::string_view,
        TimePoint) override
    {
        return {detail::InventoryStatus::absent, std::nullopt};
    }
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

[[nodiscard]] bool is_unconfirmed(const ConnectivityState& state)
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
    ConnectivityState connectivity_root;
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

    [[nodiscard]] static const NamespaceHandle* namespace_handle(
        const PreparedNetworkEnvironment& owner)
    {
        if (!owner.state_) {
            return nullptr;
        }
        const auto* proof{std::get_if<NamespaceProof>(&owner.state_->namespace_root)};
        return proof == nullptr ? nullptr : proof->exact_handle_.get();
    }

};

} // namespace detail

WorkloadNamespaceEntry::WorkloadNamespaceEntry(
    const detail::NamespaceHandle* handle) noexcept
    : handle_{handle}
{
}

bool WorkloadNamespaceEntry::enter() const noexcept
{
    return handle_ != nullptr && detail::enter_network_namespace(*handle_);
}

namespace {

[[nodiscard]] bool has_residual(const detail::OwnerState& state)
{
    return !std::holds_alternative<std::monostate>(state.namespace_root)
        || !std::holds_alternative<std::monostate>(state.veth_root)
        || !std::holds_alternative<std::monostate>(state.connectivity_root);
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
        is_unconfirmed(state->namespace_root) || is_unconfirmed(state->veth_root)
        || is_unconfirmed(state->connectivity_root)};
    const TimePoint transaction_deadline{
        state->runtime->clock->now() + transaction_limit};
    if (auto* proof{std::get_if<ConnectivityProof>(&state->connectivity_root)}) {
        if (state->runtime->clock->now() >= transaction_deadline) {
            result.failures.push_back({Stage::cleanup, Cause::timeout});
        } else {
            ConnectivityRemovalResult removal{
                state->runtime->adapter->remove_connectivity(
                    std::move(*proof),
                    operation_deadline(
                        state->runtime->clock->now(), transaction_deadline))};
            state->connectivity_root = std::move(removal.state);
            if (!std::holds_alternative<std::monostate>(state->connectivity_root)) {
                result.failures.push_back({Stage::cleanup, removal.cause});
            }
        }
    }
    const auto remove_veth = [&]() {
        if (state->runtime->clock->now() >= transaction_deadline) {
            result.failures.push_back({Stage::cleanup, Cause::timeout});
            return;
        }
        VethRemovalResult removal{state->runtime->adapter->remove_veth(
            take_proven_veth(state->veth_root),
            is_proven(state->namespace_root)
                ? &std::get<NamespaceProof>(state->namespace_root)
                : nullptr,
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

WorkloadNamespaceEntry PreparedNetworkEnvironment::workload_namespace() const noexcept
{
    return WorkloadNamespaceEntry{detail::OwnerAccess::namespace_handle(*this)};
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
    if (setup_expired()) {
        return preparation_failure(
            std::move(state), Stage::internet_connectivity, Cause::timeout);
    }
    ConnectivityMutationResult connectivity_result{
        state->runtime->adapter->configure_connectivity(deadline())};
    state->connectivity_root = std::move(connectivity_result.state);
    if (!connectivity_result.succeeded) {
        return preparation_failure(
            std::move(state),
            Stage::internet_connectivity,
            connectivity_result.cause);
    }
    if (!std::holds_alternative<ConnectivityProof>(state->connectivity_root)) {
        return preparation_failure(
            std::move(state),
            Stage::internet_connectivity,
            Cause::identity_unavailable);
    }
    return detail::OwnerAccess::prepared(std::move(state));
}

namespace {

[[nodiscard]] PreparationResult prepare_production_network_environment(
    std::unique_ptr<detail::PreflightPlatform> platform,
    std::unique_ptr<detail::ProductionPlatform> production,
    std::shared_ptr<testing::ProductionTrace> trace = {},
    const bool reconcile_recovery = true)
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
                std::move(platform),
                std::move(production),
                std::move(trace),
                reconcile_recovery),
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
        detail::make_linux_preflight_platform(),
        detail::make_linux_production_platform());
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
        std::move(platform),
        std::make_unique<UnavailableProductionPlatform>(),
        std::move(trace),
        false);
}

PreparationResult prepare_with_production_platform(
    std::unique_ptr<detail::PreflightPlatform> preflight,
    std::unique_ptr<detail::ProductionPlatform> production)
{
    return prepare_production_network_environment(
        std::move(preflight), std::move(production));
}

#ifdef NETLAGLAB_BUILD_PRIVILEGED_TESTS
int duplicate_owned_namespace_descriptor(
    const PreparedNetworkEnvironment& environment)
{
    const detail::NamespaceHandle* handle{
        detail::OwnerAccess::namespace_handle(environment)};
    return handle == nullptr
        ? -1
        : detail::duplicate_namespace_descriptor_for_test(*handle);
}
#endif

} // namespace testing
} // namespace netlaglab::network_environment
