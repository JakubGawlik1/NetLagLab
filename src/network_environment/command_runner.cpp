#include "command_runner.hpp"

#include "file_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab::network_environment {
namespace {

constexpr std::size_t maximum_diagnostic_size{4096};

enum class ChildFailureKind : int {
    exec_failure,
    system_failure,
};

struct ChildFailureMessage {
    ChildFailureKind kind;
    int error_number;
};

[[noreturn]] void report_child_failure_and_exit(
    const int descriptor,
    const ChildFailureKind kind,
    const int error_number)
{
    const ChildFailureMessage message{kind, error_number};
    const char* data{reinterpret_cast<const char*>(&message)};
    std::size_t written{};
    while (written < sizeof(message)) {
        const ssize_t result{
            write(descriptor, data + written, sizeof(message) - written)};
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    _exit(125);
}

[[nodiscard]] bool make_nonblocking(const int descriptor)
{
    const int flags{fcntl(descriptor, F_GETFL)};
    return flags != -1 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != -1;
}

[[nodiscard]] bool drain_standard_error(
    const int descriptor,
    bool& pipe_open,
    std::string& diagnostic,
    const bool drain_completely)
{
    std::array<char, 1024> buffer{};
    std::size_t reads{};
    while (pipe_open) {
        const ssize_t result{read(descriptor, buffer.data(), buffer.size())};
        if (result > 0) {
            const std::size_t available{
                maximum_diagnostic_size - diagnostic.size()};
            const std::size_t count{
                std::min(available, static_cast<std::size_t>(result))};
            diagnostic.append(buffer.data(), count);
            ++reads;
            if (!drain_completely && reads == 16) {
                break;
            }
        } else if (result == 0) {
            pipe_open = false;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool drain_child_failure(
    const int descriptor,
    bool& pipe_open,
    ChildFailureMessage& message,
    std::size_t& received)
{
    char* data{reinterpret_cast<char*>(&message)};
    while (pipe_open && received < sizeof(message)) {
        const ssize_t result{
            read(descriptor, data + received, sizeof(message) - received)};
        if (result > 0) {
            received += static_cast<std::size_t>(result);
        } else if (result == 0) {
            pipe_open = false;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool reap_blocking(const pid_t child_pid, int& status)
{
    pid_t result{};
    do {
        result = waitpid(child_pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    return result == child_pid;
}

[[nodiscard]] int poll_timeout(
    const std::chrono::steady_clock::time_point deadline)
{
    const auto remaining{deadline - std::chrono::steady_clock::now()};
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto milliseconds{
        std::chrono::ceil<std::chrono::milliseconds>(remaining)};
    return milliseconds.count() > INT_MAX
        ? INT_MAX
        : static_cast<int>(milliseconds.count());
}

} // namespace

CommandResult run_command(
    const std::string_view executable_path,
    const std::span<const std::string> arguments,
    const std::chrono::steady_clock::time_point deadline)
{
    if (std::chrono::steady_clock::now() >= deadline) {
        return {CommandResultKind::timeout, 0, {}};
    }

    const std::string executable{executable_path};
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size() + 2);
    argument_pointers.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
        argument_pointers.push_back(const_cast<char*>(argument.c_str()));
    }
    argument_pointers.push_back(nullptr);
    char* environment[]{nullptr};

    int standard_error_pipe[2]{};
    if (pipe2(standard_error_pipe, O_CLOEXEC) == -1) {
        return {CommandResultKind::system_failure, errno, {}};
    }
    FileDescriptor standard_error_reader{standard_error_pipe[0]};
    FileDescriptor standard_error_writer{standard_error_pipe[1]};

    int child_failure_pipe[2]{};
    if (pipe2(child_failure_pipe, O_CLOEXEC) == -1) {
        return {CommandResultKind::system_failure, errno, {}};
    }
    FileDescriptor child_failure_reader{child_failure_pipe[0]};
    FileDescriptor child_failure_writer{child_failure_pipe[1]};

    if (!make_nonblocking(standard_error_reader.get())
        || !make_nonblocking(child_failure_reader.get())) {
        return {CommandResultKind::system_failure, errno, {}};
    }

    if (std::chrono::steady_clock::now() >= deadline) {
        return {CommandResultKind::timeout, 0, {}};
    }

    const pid_t child_pid{fork()};
    if (child_pid == -1) {
        return {CommandResultKind::system_failure, errno, {}};
    }
    if (child_pid == 0) {
        standard_error_reader.reset();
        child_failure_reader.reset();
        if (standard_error_writer.get() == STDERR_FILENO) {
            if (fcntl(STDERR_FILENO, F_SETFD, 0) == -1) {
                report_child_failure_and_exit(
                    child_failure_writer.get(),
                    ChildFailureKind::system_failure,
                    errno);
            }
            (void)standard_error_writer.release();
        } else {
            if (dup2(standard_error_writer.get(), STDERR_FILENO) == -1) {
                report_child_failure_and_exit(
                    child_failure_writer.get(),
                    ChildFailureKind::system_failure,
                    errno);
            }
            standard_error_writer.reset();
        }
        execve(executable.c_str(), argument_pointers.data(), environment);
        report_child_failure_and_exit(
            child_failure_writer.get(),
            ChildFailureKind::exec_failure,
            errno);
    }
    standard_error_writer.reset();
    child_failure_writer.reset();

    std::string diagnostic;
    diagnostic.reserve(maximum_diagnostic_size);
    ChildFailureMessage child_failure{};
    std::size_t child_failure_bytes{};
    bool standard_error_open{true};
    bool child_failure_open{true};
    int status{};

    const auto terminate_and_reap = [&]() -> int {
        int error_number{};
        if (kill(child_pid, SIGKILL) == -1 && errno != ESRCH) {
            error_number = errno;
        }
        if (!reap_blocking(child_pid, status) && error_number == 0) {
            error_number = errno;
        }
        return error_number;
    };

    while (true) {
        if (!drain_standard_error(
                standard_error_reader.get(),
                standard_error_open,
                diagnostic,
                false)
            || !drain_child_failure(
                child_failure_reader.get(),
                child_failure_open,
                child_failure,
                child_failure_bytes)) {
            const int read_error{errno};
            (void)terminate_and_reap();
            return {
                CommandResultKind::system_failure,
                read_error,
                std::move(diagnostic),
            };
        }

        pid_t wait_result{};
        do {
            wait_result = waitpid(child_pid, &status, WNOHANG);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == child_pid) {
            break;
        }
        if (wait_result == -1) {
            return {
                CommandResultKind::system_failure,
                errno,
                std::move(diagnostic),
            };
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            const int termination_error{terminate_and_reap()};
            (void)drain_standard_error(
                standard_error_reader.get(),
                standard_error_open,
                diagnostic,
                true);
            if (termination_error != 0) {
                return {
                    CommandResultKind::system_failure,
                    termination_error,
                    std::move(diagnostic),
                };
            }
            return {
                CommandResultKind::timeout,
                0,
                std::move(diagnostic),
            };
        }

        std::array<pollfd, 2> descriptors{{
            {standard_error_open ? standard_error_reader.get() : -1, POLLIN, 0},
            {child_failure_open ? child_failure_reader.get() : -1, POLLIN, 0},
        }};
        int timeout{poll_timeout(deadline)};
        if (!standard_error_open && !child_failure_open) {
            timeout = std::min(timeout, 1);
        }
        const int poll_result{poll(
            descriptors.data(), descriptors.size(), timeout)};
        if (poll_result == -1 && errno != EINTR) {
            const int poll_error{errno};
            (void)terminate_and_reap();
            return {
                CommandResultKind::system_failure,
                poll_error,
                std::move(diagnostic),
            };
        }
    }

    if (!drain_standard_error(
            standard_error_reader.get(),
            standard_error_open,
            diagnostic,
            true)
        || !drain_child_failure(
            child_failure_reader.get(),
            child_failure_open,
            child_failure,
            child_failure_bytes)) {
        return {
            CommandResultKind::system_failure,
            errno,
            std::move(diagnostic),
        };
    }

    if (child_failure_bytes != 0) {
        if (child_failure_bytes != sizeof(child_failure)) {
            return {
                CommandResultKind::system_failure,
                EIO,
                std::move(diagnostic),
            };
        }
        return {
            child_failure.kind == ChildFailureKind::exec_failure
                ? CommandResultKind::exec_failure
                : CommandResultKind::system_failure,
            child_failure.error_number,
            std::move(diagnostic),
        };
    }
    if (WIFSIGNALED(status)) {
        return {
            CommandResultKind::signal,
            WTERMSIG(status),
            std::move(diagnostic),
        };
    }
    if (!WIFEXITED(status)) {
        return {
            CommandResultKind::system_failure,
            ECHILD,
            std::move(diagnostic),
        };
    }
    const int exit_code{WEXITSTATUS(status)};
    return {
        exit_code == 0 ? CommandResultKind::success
                       : CommandResultKind::nonzero_exit,
        exit_code,
        std::move(diagnostic),
    };
}

} // namespace netlaglab::network_environment
