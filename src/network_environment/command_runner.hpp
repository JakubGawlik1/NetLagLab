#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace netlaglab::network_environment {

enum class CommandResultKind {
    success,
    nonzero_exit,
    signal,
    timeout,
    exec_failure,
    system_failure,
};

struct CommandResult {
    CommandResultKind kind;
    // Exit code, signal number, or errno according to kind; zero otherwise.
    int code;
    std::string standard_error;
};

[[nodiscard]] CommandResult run_command(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    std::chrono::steady_clock::time_point deadline);

[[nodiscard]] CommandResult run_command_with_inherited_descriptor(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    int inherited_descriptor,
    std::chrono::steady_clock::time_point deadline);

} // namespace netlaglab::network_environment
