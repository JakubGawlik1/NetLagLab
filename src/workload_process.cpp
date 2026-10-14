#include "workload_process.hpp"

#include "file_descriptor.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <string_view>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
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

enum class ResolverSetupStep { none, mount_namespace, private_storage, snapshot_copy, resolver_mount, nsswitch_mount };

[[nodiscard]] const char* resolver_step_name(const ResolverSetupStep step)
{
    switch (step) {
    case ResolverSetupStep::mount_namespace: return "private mount namespace";
    case ResolverSetupStep::private_storage: return "private DNS storage";
    case ResolverSetupStep::snapshot_copy: return "DNS snapshot copy";
    case ResolverSetupStep::resolver_mount: return "resolv.conf mount";
    case ResolverSetupStep::nsswitch_mount: return "nsswitch.conf mount";
    case ResolverSetupStep::none: return "unknown";
    }
    return "unknown";
}

[[noreturn]] void report_child_failure_and_exit(
    const int descriptor,
    const int exit_code,
    const ResolverSetupStep step = ResolverSetupStep::none,
    const int error_number = errno)
{
    const std::array<int, 3> failure{exit_code, error_number, static_cast<int>(step)};
    const char* data{reinterpret_cast<const char*>(failure.data())};
    std::size_t written{};
    while (written < sizeof(failure)) {
        const ssize_t result{write(descriptor, data + written, sizeof(failure) - written)};
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

[[nodiscard]] bool mount_read_only_snapshot(
    const int descriptor,
    const char* const target)
{
    struct stat status {};
    if (stat(target, &status) == -1) return false;
    if (!S_ISREG(status.st_mode)) { errno = EINVAL; return false; }
    const std::string source{"/proc/self/fd/" + std::to_string(descriptor)};
    if (mount(source.c_str(), target, nullptr, MS_BIND, nullptr) == -1) {
        return false;
    }
    return mount(
               nullptr,
               target,
               nullptr,
               MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
               nullptr)
           == 0;
}

[[nodiscard]] bool install_resolver_view(
    const WorkloadResolverView& view, ResolverSetupStep& step, int& failure_errno)
{
    const auto fail = [&]() { failure_errno = errno; return false; };
    step = ResolverSetupStep::mount_namespace;
    if (unshare(CLONE_NEWNS) == -1
        || mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == -1) return fail();

    step = ResolverSetupStep::private_storage;
    constexpr const char* storage_path{"/run/netlaglab"};
    FileDescriptor original{open(storage_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (original.get() == -1) return fail();
    struct stat status{};
    if (fstat(original.get(), &status) == -1) return fail();
    if (status.st_uid != 0 || (status.st_mode & 0777) != 0755) {
        errno = EACCES;
        return fail();
    }
    // This shadows only the helper's runtime directory, in the child's private
    // mount namespace. No source pathname is ever created on the host filesystem.
    if (mount("tmpfs", storage_path, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              "size=64k,nr_inodes=8,mode=0700") == -1) return fail();
    FileDescriptor storage{open(storage_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (storage.get() == -1) return fail();
    step = ResolverSetupStep::snapshot_copy;
    FileDescriptor resolver{materialize_resolver_snapshot(
        view.resolv_conf_descriptor(), storage.get(), ResolverSnapshotFile::resolv_conf)};
    if (resolver.get() == -1) return fail();
    FileDescriptor nsswitch{materialize_resolver_snapshot(
        view.nsswitch_conf_descriptor(), storage.get(), ResolverSnapshotFile::nsswitch_conf)};
    if (nsswitch.get() == -1) return fail();
    step = ResolverSetupStep::resolver_mount;
    if (!mount_read_only_snapshot(resolver.get(), "/etc/resolv.conf")) return fail();
    if (unlinkat(storage.get(), "resolv.conf", 0) == -1) return fail();
    step = ResolverSetupStep::nsswitch_mount;
    if (!mount_read_only_snapshot(nsswitch.get(), "/etc/nsswitch.conf")) return fail();
    if (unlinkat(storage.get(), "nsswitch.conf", 0) == -1) return fail();
    return true;
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
    const WorkloadStandardDescriptors& standard_descriptors,
    const network_environment::WorkloadNamespaceEntry* namespace_entry,
    const WorkloadResolverView* resolver_view)
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

        if (namespace_entry != nullptr && !namespace_entry->enter()) {
            report_child_failure_and_exit(error_writer.get(), 125);
        }
        ResolverSetupStep resolver_step{ResolverSetupStep::none};
        int resolver_errno{};
        if (resolver_view != nullptr
            && !install_resolver_view(*resolver_view, resolver_step, resolver_errno)) {
            report_child_failure_and_exit(error_writer.get(), 125, resolver_step, resolver_errno);
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
    std::array<int, 3> child_failure{};
    char* data{reinterpret_cast<char*>(child_failure.data())};
    std::size_t received{};
    while (received < sizeof(child_failure)) {
        const ssize_t result{read(
            error_reader.get(), data + received, sizeof(child_failure) - received)};
        if (result > 0) {
            received += static_cast<std::size_t>(result);
        } else if (result == 0) {
            break;
        } else if (errno != EINTR) {
            received = 1;
            child_failure[0] = 125;
            break;
        }
    }

    if (received == 0) {
        return {.process = WorkloadProcess{child_pid}};
    }

    int status{};
    while (waitpid(child_pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (received != sizeof(child_failure)) {
        child_failure[0] = 125;
    }
    std::string failure;
    if (received == sizeof(child_failure) && child_failure[2] != 0) {
        failure = std::string{"resolver view setup at "}
            + resolver_step_name(static_cast<ResolverSetupStep>(child_failure[2]))
            + ": " + std::strerror(child_failure[1])
            + " (errno " + std::to_string(child_failure[1]) + ")";
    }
    return {.process = std::nullopt, .failure_exit_code = child_failure[0],
            .failure = std::move(failure)};
}

} // namespace netlaglab
