#pragma once

#include "network_environment.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace netlaglab::network_environment::testing {

enum class Operation {
    preflight,
    create_namespace,
    create_veth,
    move_peer,
    assign_host_address,
    bring_host_link_up,
    bring_loopback_up,
    assign_namespace_address,
    bring_namespace_link_up,
    add_default_route,
    configure_connectivity,
    validate_connectivity,
    remove_connectivity,
    remove_veth,
    remove_namespace,
};

enum class Outcome {
    success,
    fail_unchanged,
    fail_new_state,
    fail_absent,
    fail_identity_unconfirmed,
    removed,
    already_absent,
    cleanup_retained,
    cleanup_identity_mismatch,
    cleanup_identity_unconfirmed,
};

struct ScriptStep {
    Operation operation;
    Outcome outcome{Outcome::success};
    Cause cause{Cause::system_failure};
    std::chrono::steady_clock::duration elapsed{};
};

struct SharedTrace {
    std::vector<Operation> operations;
    std::vector<std::chrono::steady_clock::time_point> deadlines;
    bool host_lock_alive{};
};

[[nodiscard]] PreparationResult prepare_scripted_network_environment(
    std::vector<ScriptStep> script,
    std::shared_ptr<SharedTrace> trace,
    std::chrono::steady_clock::time_point start = {},
    bool track_host_lock = false);

} // namespace netlaglab::network_environment::testing
