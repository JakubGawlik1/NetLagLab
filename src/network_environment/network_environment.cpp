#include "network_environment.hpp"

#include "preflight.hpp"
#include "production_adapter.hpp"
#include "production_test_support.hpp"
#include "transaction_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <sys/random.h>
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

class NatProof {
public:
    explicit NatProof(std::string ownership_comment)
        : ownership_comment_{std::move(ownership_comment)}
    {
    }

    NatProof(NatProof&&) noexcept = default;
    NatProof& operator=(NatProof&&) noexcept = default;
    NatProof(const NatProof&) = delete;
    NatProof& operator=(const NatProof&) = delete;

private:
    std::string ownership_comment_;

    friend class ProductionAdapter;
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

using NamespaceState =
    std::variant<std::monostate, NamespaceProof, IdentityUnconfirmed>;
using VethState = std::variant<
    std::monostate,
    HostVethProof,
    PlacedVethProof,
    IdentityUnconfirmed>;
using NatState = std::variant<std::monostate, NatProof, IdentityUnconfirmed>;
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

struct NatMutationResult {
    bool succeeded;
    Cause cause;
    NatState state;
};

struct NamespaceRemovalResult {
    Cause cause;
    NamespaceState state;
};

struct VethRemovalResult {
    Cause cause;
    VethState state;
};

struct NatRemovalResult {
    Cause cause;
    NatState state;
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
    [[nodiscard]] virtual NatMutationResult configure_nat(
        const PlacedVethProof& veth_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual VethRemovalResult remove_veth(
        ProvenVeth proof,
        const NamespaceProof* namespace_proof,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual NatRemovalResult remove_nat(
        NatProof proof,
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

    [[nodiscard]] NatMutationResult configure_nat(
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::configure_nat, deadline)};
        NatState state;
        if (step.outcome == Outcome::success
            || step.outcome == Outcome::fail_new_state) {
            state.emplace<NatProof>("scripted-owner");
        } else if (step.outcome == Outcome::fail_identity_unconfirmed) {
            state.emplace<IdentityUnconfirmed>();
        }
        return {step.outcome == Outcome::success, step.cause, std::move(state)};
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

    [[nodiscard]] NatRemovalResult remove_nat(
        NatProof proof,
        const TimePoint deadline) override
    {
        const ScriptStep step{next(Operation::remove_nat, deadline)};
        NatState state;
        if (step.outcome == Outcome::cleanup_retained) {
            state.emplace<NatProof>(std::move(proof));
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
};

class ProductionAdapter final : public SemanticAdapter {
public:
    explicit ProductionAdapter(
        std::unique_ptr<detail::PreflightPlatform> platform,
        std::unique_ptr<detail::ProductionPlatform> production,
        std::shared_ptr<testing::ProductionTrace> trace = {})
        : platform_{std::move(platform)}
        , production_{std::move(production)}
        , trace_{std::move(trace)}
    {
    }

    [[nodiscard]] OperationResult preflight(const TimePoint deadline) override
    {
        detail::PreflightResult result{
            detail::run_preflight(*platform_, deadline)};
        if (!result.succeeded) {
            return {false, result.cause};
        }
        ip_path_ = std::move(result.ip_path);

        for (const std::string_view path : detail::trusted_nft_paths()) {
            const detail::ToolQuery query{platform_->query_tool(path)};
            if (query.status == detail::QueryStatus::failure) {
                return {false, Cause::system_failure};
            }
            if (query.status == detail::QueryStatus::present
                && detail::is_trusted_executable(query.metadata)) {
                nft_path_ = path;
                break;
            }
        }
        if (nft_path_.empty()) {
            return {false, Cause::unavailable_or_invalid_tool};
        }

        const std::vector<std::string> list_tables{
            "-n", "list", "tables", "ip"};
        const CommandResult tables{production_->run_nft(
            nft_path_, list_tables, true, deadline)};
        if (tables.kind == CommandResultKind::timeout) {
            return {false, Cause::timeout};
        }
        if (tables.kind != CommandResultKind::success) {
            return {false, command_cause(tables)};
        }
        if (contains_table(tables.standard_output, "netlaglab")) {
            return {false, Cause::collision};
        }

        std::uint64_t random_value{};
        ssize_t random_bytes{};
        do {
            random_bytes = getrandom(
                &random_value, sizeof(random_value), GRND_NONBLOCK);
        } while (random_bytes == -1 && errno == EINTR);
        if (random_bytes != static_cast<ssize_t>(sizeof(random_value))) {
            return {false, Cause::system_failure};
        }
        std::ostringstream comment;
        comment << "nll-session-" << std::hex << std::setfill('0')
                << std::setw(16) << random_value;
        ownership_comment_ = comment.str();
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

    [[nodiscard]] NatMutationResult configure_nat(
        const PlacedVethProof&,
        const TimePoint deadline) override
    {
        const std::string comment{"\"" + ownership_comment_ + "\""};
        const std::vector<std::string> table_arguments{
            "add", "table", "ip", "netlaglab", "{", "comment",
            comment + ";", "}"};
        const CommandResult table{production_->run_nft(
            nft_path_, table_arguments, false, deadline)};
        if (table.kind != CommandResultKind::success) {
            const NatObservation observation{
                observe_nat(deadline, ownership_comment_)};
            if (observation == NatObservation::owned) {
                NatState state;
                state.emplace<NatProof>(ownership_comment_);
                return {false, command_cause(table), std::move(state)};
            }
            NatState state;
            if (observation == NatObservation::foreign
                || observation == NatObservation::failure) {
                state.emplace<IdentityUnconfirmed>();
            }
            const Cause cause{observation == NatObservation::failure
                    ? Cause::identity_unavailable
                    : observation == NatObservation::foreign
                    ? Cause::identity_mismatch
                    : command_cause(table)};
            return {false, cause, std::move(state)};
        }

        NatProof proof{ownership_comment_};
        const std::vector<std::string> chain_arguments{
            "add", "chain", "ip", "netlaglab", "postrouting", "{",
            "type", "nat", "hook", "postrouting", "priority", "srcnat;",
            "policy", "accept;", "comment", comment + ";", "}"};
        const CommandResult chain{production_->run_nft(
            nft_path_, chain_arguments, false, deadline)};
        if (chain.kind != CommandResultKind::success) {
            return {false, command_cause(chain), owned_nat_state(std::move(proof))};
        }

        const std::vector<std::string> rule_arguments{
            "add", "rule", "ip", "netlaglab", "postrouting", "iifname",
            "nll-host", "ip", "saddr", "10.200.0.2", "masquerade",
            "comment", comment};
        const CommandResult rule{production_->run_nft(
            nft_path_, rule_arguments, false, deadline)};
        if (rule.kind != CommandResultKind::success) {
            return {false, command_cause(rule), owned_nat_state(std::move(proof))};
        }
        return {true, Cause::system_failure, owned_nat_state(std::move(proof))};
    }

    [[nodiscard]] VethRemovalResult remove_veth(
        ProvenVeth proof,
        const NamespaceProof* namespace_proof,
        const TimePoint deadline) override
    {
        return remove_proven_veth(
            std::move(proof), namespace_proof, deadline);
    }

    [[nodiscard]] NatRemovalResult remove_nat(
        NatProof proof,
        const TimePoint deadline) override
    {
        const NatObservation before{
            observe_nat(deadline, proof.ownership_comment_)};
        if (before == NatObservation::absent) {
            return {Cause::system_failure, {}};
        }
        if (before == NatObservation::foreign) {
            NatState state;
            state.emplace<IdentityUnconfirmed>();
            return {Cause::identity_mismatch, std::move(state)};
        }
        if (before != NatObservation::owned) {
            NatState state;
            state.emplace<IdentityUnconfirmed>();
            return {Cause::identity_unavailable, std::move(state)};
        }

        const std::vector<std::string> arguments{
            "delete", "table", "ip", "netlaglab"};
        const CommandResult command{production_->run_nft(
            nft_path_, arguments, false, deadline)};
        const NatObservation after{
            observe_nat(deadline, proof.ownership_comment_)};
        if (after == NatObservation::absent) {
            return {Cause::system_failure, {}};
        }
        if (after == NatObservation::owned) {
            return {
                command.kind == CommandResultKind::success
                    ? Cause::identity_mismatch
                    : command_cause(command),
                owned_nat_state(std::move(proof)),
            };
        }
        NatState state;
        state.emplace<IdentityUnconfirmed>();
        return {
            after == NatObservation::foreign
                ? Cause::identity_mismatch
                : Cause::identity_unavailable,
            std::move(state),
        };
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
    enum class NatObservation {
        absent,
        owned,
        foreign,
        failure,
    };

    [[nodiscard]] static bool contains_table(
        const std::string_view output,
        const std::string_view expected)
    {
        std::istringstream lines{std::string{output}};
        std::string family;
        std::string table;
        std::string name;
        while (lines >> family >> table >> name) {
            if (family == "table" && table == "ip" && name == expected) {
                return true;
            }
            std::string remainder;
            std::getline(lines, remainder);
        }
        return false;
    }

    [[nodiscard]] static bool is_owned_nat_table(
        const std::string_view output,
        const std::string_view expected_comment)
    {
        std::istringstream lines{std::string{output}};
        std::vector<std::string> content;
        std::string line;
        while (std::getline(lines, line)) {
            const std::size_t first{line.find_first_not_of(" \t\r")};
            if (first == std::string::npos) {
                continue;
            }
            const std::size_t last{line.find_last_not_of(" \t\r")};
            content.push_back(line.substr(first, last - first + 1));
        }
        const std::string marker{"comment \"" + std::string{expected_comment} + "\""};
        if (content.size() != 8U
            || content[0] != "table ip netlaglab {"
            || content[1] != marker
            || content[2] != "chain postrouting {"
            || content[3] != "type nat hook postrouting priority srcnat; policy accept;"
            || content[4] != marker
            || content[6] != "}"
            || content[7] != "}") {
            return false;
        }
        const std::array<std::string, 2> owned_rules{
            "iifname \"nll-host\" ip saddr 10.200.0.2 masquerade " + marker,
            "ip saddr 10.200.0.2 iifname \"nll-host\" masquerade " + marker,
        };
        return content[5] == owned_rules[0] || content[5] == owned_rules[1];
    }

    [[nodiscard]] NatObservation observe_nat(
        const TimePoint deadline,
        const std::string_view expected_comment)
    {
        const std::vector<std::string> list_tables{
            "-n", "list", "tables", "ip"};
        const CommandResult tables{production_->run_nft(
            nft_path_, list_tables, true, deadline)};
        if (tables.kind != CommandResultKind::success) {
            return NatObservation::failure;
        }
        if (!contains_table(tables.standard_output, "netlaglab")) {
            return NatObservation::absent;
        }
        const std::vector<std::string> list_table{
            "-n", "list", "table", "ip", "netlaglab"};
        const CommandResult table{production_->run_nft(
            nft_path_, list_table, true, deadline)};
        if (table.kind != CommandResultKind::success) {
            return NatObservation::failure;
        }
        return is_owned_nat_table(table.standard_output, expected_comment)
            ? NatObservation::owned
            : NatObservation::foreign;
    }

    [[nodiscard]] static NatState owned_nat_state(NatProof proof)
    {
        NatState state;
        state.emplace<NatProof>(std::move(proof));
        return state;
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
    std::string nft_path_;
    std::string ownership_comment_;
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

    [[nodiscard]] CommandResult run_nft(
        std::string_view,
        std::span<const std::string>,
        bool,
        TimePoint) override
    {
        return {CommandResultKind::success, 0, {}, {}};
    }

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

[[nodiscard]] bool is_unconfirmed(const NatState& state)
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

[[nodiscard]] bool is_proven(const NatState& state)
{
    return std::holds_alternative<NatProof>(state);
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
    NatState nat_root;
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

    [[nodiscard]] static const NamespaceHandle* borrow_namespace_handle(
        const PreparedNetworkEnvironment& owner) noexcept
    {
        if (!owner.state_) {
            return nullptr;
        }
        const auto* proof{
            std::get_if<NamespaceProof>(&owner.state_->namespace_root)};
        return proof == nullptr ? nullptr : proof->exact_handle_.get();
    }
};

} // namespace detail

namespace {

[[nodiscard]] bool has_residual(const detail::OwnerState& state)
{
    return !std::holds_alternative<std::monostate>(state.nat_root)
        || !std::holds_alternative<std::monostate>(state.namespace_root)
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
        is_unconfirmed(state->nat_root) || is_unconfirmed(state->namespace_root)
        || is_unconfirmed(state->veth_root)};
    const TimePoint transaction_deadline{
        state->runtime->clock->now() + transaction_limit};
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

    if (is_proven(state->nat_root)) {
        if (state->runtime->clock->now() >= transaction_deadline) {
            result.failures.push_back({Stage::cleanup, Cause::timeout});
        } else {
            NatRemovalResult removal{state->runtime->adapter->remove_nat(
                std::move(std::get<NatProof>(state->nat_root)),
                operation_deadline(
                    state->runtime->clock->now(), transaction_deadline))};
            state->nat_root = std::move(removal.state);
            if (!std::holds_alternative<std::monostate>(state->nat_root)) {
                result.failures.push_back({Stage::cleanup, removal.cause});
            }
        }
    }

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

WorkloadNamespaceEntry::WorkloadNamespaceEntry(
    const detail::NamespaceHandle* handle) noexcept
    : handle_{handle}
{
}

bool WorkloadNamespaceEntry::enter() const noexcept
{
    return handle_ != nullptr && detail::enter_network_namespace(*handle_);
}

WorkloadNamespaceEntry PreparedNetworkEnvironment::workload_namespace() const noexcept
{
    return WorkloadNamespaceEntry{
        detail::OwnerAccess::borrow_namespace_handle(*this)};
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
    if (auto failure{run_configuration(
            Stage::nat_configuration,
            [&](const TimePoint value) {
                NatMutationResult result{
                    state->runtime->adapter->configure_nat(placed_veth, value)};
                state->nat_root = std::move(result.state);
                return OperationResult{result.succeeded, result.cause};
            })}) {
        return std::move(*failure);
    }
    return detail::OwnerAccess::prepared(std::move(state));
}

namespace {

[[nodiscard]] PreparationResult prepare_production_network_environment(
    std::unique_ptr<detail::PreflightPlatform> platform,
    std::unique_ptr<detail::ProductionPlatform> production,
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
                std::move(platform), std::move(production), std::move(trace)),
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
        std::move(trace));
}

PreparationResult prepare_with_production_platform(
    std::unique_ptr<detail::PreflightPlatform> preflight,
    std::unique_ptr<detail::ProductionPlatform> production)
{
    return prepare_production_network_environment(
        std::move(preflight), std::move(production));
}

int borrow_prepared_namespace_descriptor(
    const PreparedNetworkEnvironment& environment) noexcept
{
    const detail::NamespaceHandle* handle{
        detail::OwnerAccess::borrow_namespace_handle(environment)};
    return handle == nullptr
        ? -1
        : detail::borrow_linux_namespace_descriptor_for_testing(*handle);
}

} // namespace testing
} // namespace netlaglab::network_environment
