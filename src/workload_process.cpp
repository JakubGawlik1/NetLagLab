#include "workload_process.hpp"

#include "file_descriptor.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <string_view>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab {
namespace {

[[nodiscard]] int exec_failure_exit_code(const int error_code)
{
    if (error_code == ENOENT || error_code == ENOTDIR) {
        return 127;
    }
    if (error_code == EACCES || error_code == ENOEXEC) {
        return 126;
    }
    return 125;
}

[[nodiscard]] std::vector<std::string> executable_candidates(
    const WorkloadContext& context)
{
    const std::string& program{context.arguments.front()};
    if (program.find('/') != std::string::npos) {
        return {program};
    }

    constexpr std::string_view path_prefix{"PATH="};
    std::string_view path;
    for (const std::string& entry : context.environment) {
        if (entry.starts_with(path_prefix)) {
            path = std::string_view{entry}.substr(path_prefix.size());
            break;
        }
    }
    if (path.data() == nullptr) {
        return {};
    }

    std::vector<std::string> candidates;
    std::size_t beginning{};
    while (beginning <= path.size()) {
        const std::size_t ending{path.find(':', beginning)};
        const std::string_view directory{path.substr(
            beginning,
            ending == std::string_view::npos ? path.size() - beginning
                                             : ending - beginning)};
        if (directory.empty()) {
            candidates.push_back("./" + program);
        } else {
            candidates.push_back(std::string{directory} + '/' + program);
        }
        if (ending == std::string_view::npos) {
            break;
        }
        beginning = ending + 1;
    }
    return candidates;
}

[[noreturn]] void report_child_failure_and_exit(
    const int descriptor,
    const int exit_code)
{
    const char* data{reinterpret_cast<const char*>(&exit_code)};
    std::size_t written{};
    while (written < sizeof(exit_code)) {
        const ssize_t result{write(descriptor, data + written, sizeof(exit_code) - written)};
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    _exit(exit_code);
}

[[nodiscard]] WorkloadStatus decode_wait_status(const int status)
{
    if (WIFEXITED(status)) {
        return {true, WEXITSTATUS(status)};
    }
    if (WIFSIGNALED(status)) {
        return {false, WTERMSIG(status)};
    }
    return {false, 0};
}

} // namespace

WorkloadProcess::WorkloadProcess(const pid_t pid) noexcept
    : pid_{pid}
{
}

WorkloadProcess::~WorkloadProcess()
{
    terminate_and_reap();
}

WorkloadProcess::WorkloadProcess(WorkloadProcess&& other) noexcept
    : pid_{std::exchange(other.pid_, -1)}
{
}

WorkloadProcess& WorkloadProcess::operator=(WorkloadProcess&& other) noexcept
{
    if (this != &other) {
        terminate_and_reap();
        pid_ = std::exchange(other.pid_, -1);
    }
    return *this;
}

void WorkloadProcess::terminate_and_reap() noexcept
{
    if (pid_ <= 0) {
        return;
    }
    (void)kill(pid_, SIGKILL);
    int status{};
    while (waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
    }
    pid_ = -1;
}

pid_t WorkloadProcess::pid() const noexcept
{
    return pid_;
}

bool WorkloadProcess::send_signal(const int signal_number) const noexcept
{
    return pid_ > 0 && kill(pid_, signal_number) == 0;
}

std::optional<WorkloadStatus> WorkloadProcess::wait()
{
    if (pid_ <= 0) {
        return std::nullopt;
    }

    int status{};
    pid_t result{};
    do {
        result = waitpid(pid_, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (result != pid_) {
        return std::nullopt;
    }
    pid_ = -1;
    return decode_wait_status(status);
}

WorkloadPollResult WorkloadProcess::poll()
{
    if (pid_ <= 0) {
        return {WorkloadPollState::error, std::nullopt};
    }

    int status{};
    pid_t result{};
    do {
        result = waitpid(pid_, &status, WNOHANG);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
        return {WorkloadPollState::running, std::nullopt};
    }
    if (result != pid_) {
        return {WorkloadPollState::error, std::nullopt};
    }
    pid_ = -1;
    return {WorkloadPollState::finished, decode_wait_status(status)};
}

WorkloadLaunchResult launch_workload(
    const WorkloadContext& context,
    const WorkloadIdentity& identity,
    const WorkloadStandardDescriptors& standard_descriptors)
{
    if (context.arguments.empty()) {
        return {.process = std::nullopt, .failure_exit_code = 125};
    }

    std::vector<std::string> candidates{executable_candidates(context)};
    if (candidates.empty()) {
        return {.process = std::nullopt, .failure_exit_code = 127};
    }

    std::vector<char*> argument_pointers;
    argument_pointers.reserve(context.arguments.size() + 1);
    for (const std::string& argument : context.arguments) {
        argument_pointers.push_back(const_cast<char*>(argument.c_str()));
    }
    argument_pointers.push_back(nullptr);

    std::vector<char*> environment_pointers;
    environment_pointers.reserve(context.environment.size() + 1);
    for (const std::string& entry : context.environment) {
        environment_pointers.push_back(const_cast<char*>(entry.c_str()));
    }
    environment_pointers.push_back(nullptr);

    int error_pipe[2]{};
    if (pipe2(error_pipe, O_CLOEXEC) == -1) {
        return {.process = std::nullopt, .failure_exit_code = 125};
    }
    FileDescriptor error_reader{error_pipe[0]};
    FileDescriptor error_writer{error_pipe[1]};
    const pid_t expected_parent{getpid()};

    const pid_t child_pid{fork()};
    if (child_pid == -1) {
        return {.process = std::nullopt, .failure_exit_code = 125};
    }

    if (child_pid == 0) {
        error_reader.reset();
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1 || getppid() != expected_parent) {
            report_child_failure_and_exit(error_writer.get(), 125);
        }

        struct sigaction default_action {};
        default_action.sa_handler = SIG_DFL;
        sigemptyset(&default_action.sa_mask);
        if (sigaction(SIGINT, &default_action, nullptr) == -1) {
            report_child_failure_and_exit(error_writer.get(), 125);
        }

        for (std::size_t index{}; index < standard_descriptors.sources.size();
             ++index) {
            const int source{standard_descriptors.sources[index]};
            const int target{static_cast<int>(index)};
            if (source == close_standard_descriptor) {
                if (close(target) == -1 && errno != EBADF) {
                    report_child_failure_and_exit(error_writer.get(), 125);
                }
            } else if (source != inherit_standard_descriptor) {
                if (source == target) {
                    if (fcntl(target, F_SETFD, 0) == -1) {
                        report_child_failure_and_exit(error_writer.get(), 125);
                    }
                } else if (dup2(source, target) == -1) {
                    report_child_failure_and_exit(error_writer.get(), 125);
                }
            }
        }

        if (geteuid() == 0) {
            if (setgroups(
                    identity.supplementary_groups.size(),
                    identity.supplementary_groups.data()) == -1
                || setgid(identity.gid) == -1
                || setuid(identity.uid) == -1) {
                report_child_failure_and_exit(error_writer.get(), 125);
            }
        } else if (geteuid() != identity.uid || getegid() != identity.gid) {
            report_child_failure_and_exit(error_writer.get(), 125);
        }

        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1
            || chdir(context.working_directory.c_str()) == -1) {
            report_child_failure_and_exit(error_writer.get(), 125);
        }

        int last_error{ENOENT};
        bool permission_denied{false};
        for (const std::string& executable : candidates) {
            execve(
                executable.c_str(),
                argument_pointers.data(),
                environment_pointers.data());
            last_error = errno;
            if (last_error == EACCES) {
                permission_denied = true;
                continue;
            }
            if (last_error != ENOENT && last_error != ENOTDIR) {
                break;
            }
        }
        if ((last_error == ENOENT || last_error == ENOTDIR) && permission_denied) {
            last_error = EACCES;
        }
        report_child_failure_and_exit(error_writer.get(), exec_failure_exit_code(last_error));
    }

    error_writer.reset();
    int failure_exit_code{};
    char* data{reinterpret_cast<char*>(&failure_exit_code)};
    std::size_t received{};
    while (received < sizeof(failure_exit_code)) {
        const ssize_t result{read(
            error_reader.get(), data + received, sizeof(failure_exit_code) - received)};
        if (result > 0) {
            received += static_cast<std::size_t>(result);
        } else if (result == 0) {
            break;
        } else if (errno != EINTR) {
            received = 1;
            failure_exit_code = 125;
            break;
        }
    }

    if (received == 0) {
        return {.process = WorkloadProcess{child_pid}};
    }

    int status{};
    while (waitpid(child_pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (received != sizeof(failure_exit_code)) {
        failure_exit_code = 125;
    }
    return {.process = std::nullopt, .failure_exit_code = failure_exit_code};
}

} // namespace netlaglab
