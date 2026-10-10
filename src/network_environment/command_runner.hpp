#pragma once

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
    CommandResult(
        CommandResultKind result_kind,
        int result_code,
        std::string error_output,
        std::string captured_output = {})
        : kind{result_kind}
        , code{result_code}
        , standard_error{std::move(error_output)}
        , standard_output{std::move(captured_output)}
    {
    }

    CommandResultKind kind;
    // Exit code, signal number, or errno according to kind; zero otherwise.
    int code;
    std::string standard_error;
    std::string standard_output;
};

[[nodiscard]] CommandResult run_command(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    std::chrono::steady_clock::time_point deadline);

[[nodiscard]] CommandResult run_command_capturing_stdout(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    std::chrono::steady_clock::time_point deadline);

[[nodiscard]] CommandResult run_command_with_inherited_descriptor(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    int inherited_descriptor,
    std::chrono::steady_clock::time_point deadline);

[[nodiscard]] CommandResult run_command_in_network_namespace(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    int namespace_descriptor,
    std::chrono::steady_clock::time_point deadline);

} // namespace netlaglab::network_environment
