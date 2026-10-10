#include "file_descriptor.hpp"
#include "helper_session.hpp"
#include "helper_protocol.hpp"
#include "network_environment/network_environment.hpp"
#include "process_status.hpp"
#include "session_paths.hpp"
#include "session_socket.hpp"
#include "session_validation.hpp"
#include "socket_io.hpp"
#include "workload_context.hpp"
#include "workload_process.hpp"
#include "workload_resolver_view.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <ostream>
#include <poll.h>
#include <sys/file.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <variant>
#include <vector>

#include <cstdlib>
#include <iostream>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view usage{
    "Usage: netlaglab-helper --runtime-dir <absolute-path> "
    "--supervisor-pid <pid> --supervisor-start-time <ticks>\n"};
constexpr int usage_error_exit_code{2};
constexpr int supervisor_connection_poll_timeout_ms{100};

struct ValidatedSession {
    netlaglab::FileDescriptor directory;
    netlaglab::FileDescriptor supervisor_lock;
};

[[nodiscard]] bool send_helper_error(
    const int supervisor_descriptor,
    const std::string_view message)
{
    return netlaglab::send_socket_text(
        supervisor_descriptor, netlaglab::helper_error_message(message));
}

[[nodiscard]] bool cleanup_helper_listener(
    netlaglab::FileDescriptor& listening_socket,
    netlaglab::SocketPathOwner& socket_path_owner,
    std::ostream& error)
{
    listening_socket.reset();
    const int cleanup_error{socket_path_owner.remove_owned()};
    if (cleanup_error == 0) {
        return true;
    }
    error << "NetLagLab helper: failed to remove helper.sock: "
          << std::strerror(cleanup_error) << '\n';
    return false;
}

[[nodiscard]] std::optional<ValidatedSession> validate_session(
    const netlaglab::SessionPaths& paths,
    const uid_t expected_owner,
    std::ostream& error)
{
    std::optional<netlaglab::FileDescriptor> runtime_directory{
        netlaglab::open_and_validate_runtime_directory(
            paths.xdg_runtime_directory(), expected_owner, error)};
    if (!runtime_directory.has_value()) {
        return std::nullopt;
    }
    std::optional<netlaglab::FileDescriptor> session_directory{
        netlaglab::open_and_validate_session_directory(
            runtime_directory->get(), expected_owner, error)};
    if (!session_directory.has_value()) {
        return std::nullopt;
    }

    const int lock_descriptor{openat(
        session_directory->get(),
        netlaglab::SessionPaths::lock_file_name.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (lock_descriptor == -1) {
        error << "NetLagLab helper: failed to open session.lock: "
              << std::strerror(errno) << '\n';
        return std::nullopt;
    }
    netlaglab::FileDescriptor session_lock{lock_descriptor};
    struct stat lock_status {};
    if (fstat(session_lock.get(), &lock_status) == -1
        || !S_ISREG(lock_status.st_mode) || lock_status.st_uid != expected_owner
        || (lock_status.st_mode & 0777) != 0600) {
        error << "NetLagLab helper: session.lock has invalid type, owner, or permissions\n";
        return std::nullopt;
    }
    if (flock(session_lock.get(), LOCK_EX | LOCK_NB) == 0) {
        error << "NetLagLab helper: session.lock is not held by an active supervisor\n";
        return std::nullopt;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        error << "NetLagLab helper: failed to inspect the session lock: "
              << std::strerror(errno) << '\n';
        return std::nullopt;
    }
    return ValidatedSession{
        std::move(*session_directory),
        std::move(session_lock),
    };
}

[[nodiscard]] std::optional<netlaglab::FileDescriptor> accept_supervisor(
    const int listening_descriptor,
    const int supervisor_process_descriptor,
    std::ostream& error)
{
    while (true) {
        std::array<struct pollfd, 2> descriptors{{
            {listening_descriptor, POLLIN, 0},
            {supervisor_process_descriptor, POLLIN, 0},
        }};
        int poll_result{};
        do {
            poll_result = poll(
                descriptors.data(), descriptors.size(),
                supervisor_connection_poll_timeout_ms);
        } while (poll_result == -1 && errno == EINTR);

        if (poll_result == -1
            || (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0
            || (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            error << "NetLagLab helper: failed while waiting for Supervisor connection\n";
            return std::nullopt;
        }

        if ((descriptors[1].revents & POLLIN) != 0) {
            error << "NetLagLab helper: Supervisor exited before connecting\n";
            return std::nullopt;
        }
        if (poll_result == 0) {
            continue;
        }

        int supervisor_descriptor{};
        do {
            supervisor_descriptor = accept4(
                listening_descriptor, nullptr, nullptr, SOCK_CLOEXEC);
        } while (supervisor_descriptor == -1 && errno == EINTR);
        if (supervisor_descriptor == -1) {
            error << "NetLagLab helper: failed to accept Supervisor: "
                  << std::strerror(errno) << '\n';
            return std::nullopt;
        }

        return netlaglab::FileDescriptor{supervisor_descriptor};
    }
}

[[nodiscard]] std::optional<pid_t> parse_supervisor_pid(
    const std::string_view text)
{
    pid_t pid{};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), pid)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || pid <= 1) {
        return std::nullopt;
    }
    return pid;
}

[[nodiscard]] std::optional<std::uint64_t> parse_supervisor_start_time(
    const std::string_view text)
{
    std::uint64_t start_time{};
    const auto result{
        std::from_chars(text.data(), text.data() + text.size(), start_time)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || start_time == 0) {
        return std::nullopt;
    }
    return start_time;
}

template<typename Id>
[[nodiscard]] std::optional<Id> read_sudo_id(
    const char* const variable,
    std::ostream& error)
{
    const char* const text_value{std::getenv(variable)};
    if (text_value == nullptr || text_value[0] == '\0') {
        error << "NetLagLab helper: " << variable << " is not set\n";
        return std::nullopt;
    }
    const std::string_view text{text_value};
    Id parsed{};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), parsed)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        error << "NetLagLab helper: failed to parse " << variable << '\n';
        return std::nullopt;
    }
    return parsed;
}

struct InvokingIds {
    uid_t uid;
    gid_t gid;
};

[[nodiscard]] std::optional<InvokingIds> read_invoking_ids(
    std::ostream& error)
{
    const std::optional<uid_t> uid{read_sudo_id<uid_t>("SUDO_UID", error)};
    const std::optional<gid_t> gid{read_sudo_id<gid_t>("SUDO_GID", error)};
    if (!uid.has_value() || !gid.has_value()) {
        return std::nullopt;
    }
    return InvokingIds{*uid, *gid};
}

[[nodiscard]] std::optional<std::vector<gid_t>> read_peer_groups(
    const pid_t peer_pid,
    std::ostream& error)
{
    const std::string path{"/proc/" + std::to_string(peer_pid) + "/status"};
    std::ifstream status{path};
    if (!status) {
        error << "NetLagLab helper: failed to open authenticated Supervisor status\n";
        return std::nullopt;
    }

    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view prefix{"Groups:"};
        if (!line.starts_with(prefix)) {
            continue;
        }

        std::vector<gid_t> groups;
        std::string_view values{line};
        values.remove_prefix(prefix.size());
        std::size_t position{};
        while (position < values.size()) {
            while (position < values.size()
                   && (values[position] == ' ' || values[position] == '\t')) {
                ++position;
            }
            if (position == values.size()) {
                break;
            }
            const std::size_t beginning{position};
            while (position < values.size() && values[position] != ' '
                   && values[position] != '\t') {
                ++position;
            }
            gid_t group{};
            const std::string_view token{values.substr(beginning, position - beginning)};
            const auto parsed{
                std::from_chars(token.data(), token.data() + token.size(), group)};
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()
                || groups.size() >= 65'536) {
                error << "NetLagLab helper: invalid authenticated Supervisor group list\n";
                return std::nullopt;
            }
            groups.push_back(group);
        }
        return groups;
    }

    error << "NetLagLab helper: authenticated Supervisor group list is unavailable\n";
    return std::nullopt;
}

[[nodiscard]] std::optional<netlaglab::WorkloadContext> read_start_block(
    const int supervisor_descriptor,
    std::ostream& error)
{
    netlaglab::HelperStartConversation conversation;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    while (true) {
        int timeout{-1};
        if (deadline.has_value()) {
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now())};
            if (remaining <= 0ms) {
                (void)send_helper_error(supervisor_descriptor, "Workload start block timed out.");
                error << "NetLagLab helper: Workload start block timed out\n";
                return std::nullopt;
            }
            timeout = static_cast<int>(remaining.count());
        }

        struct pollfd descriptor {supervisor_descriptor, POLLIN, 0};
        int poll_result{};
        do {
            poll_result = poll(&descriptor, 1, timeout);
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result == 0) {
            (void)send_helper_error(supervisor_descriptor, "Workload start block timed out.");
            error << "NetLagLab helper: Workload start block timed out\n";
            return std::nullopt;
        }
        if (poll_result == -1 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            error << "NetLagLab helper: failed while waiting for Workload context\n";
            return std::nullopt;
        }

        std::string bytes;
        const netlaglab::SocketReadResult read_result{
            netlaglab::read_socket_data(supervisor_descriptor, bytes)};
        if (read_result.status != netlaglab::SocketReadStatus::data_received) {
            error << "NetLagLab helper: supervisor disconnected during Workload context\n";
            return std::nullopt;
        }
        const netlaglab::StartBlockFeedResult result{
            conversation.receive_bytes(bytes)};
        if (!deadline.has_value() && conversation.started()) {
            deadline = std::chrono::steady_clock::now() + 30s;
        }
        if (result.state == netlaglab::StartBlockState::invalid) {
            (void)send_helper_error(supervisor_descriptor, "Invalid Workload start block.");
            error << "NetLagLab helper: invalid Workload start block\n";
            return std::nullopt;
        }
        if (result.state == netlaglab::StartBlockState::complete) {
            return result.context;
        }
    }
}

[[nodiscard]] bool send_workload_status(
    const int supervisor_descriptor,
    const netlaglab::WorkloadStatus status)
{
    const netlaglab::HelperConversationEvent event{
        status.exited
            ? netlaglab::HelperConversationEvent{
                  netlaglab::WorkloadExitedEvent{status.value}}
            : netlaglab::HelperConversationEvent{
                  netlaglab::WorkloadSignaledEvent{status.value}}};
    return netlaglab::send_socket_text(
        supervisor_descriptor,
        netlaglab::helper_conversation_event_message(event));
}

[[nodiscard]] bool stop_after_supervisor_loss(netlaglab::WorkloadProcess& workload)
{
    (void)workload.send_signal(SIGTERM);
    const auto deadline{std::chrono::steady_clock::now() + 5s};
    while (std::chrono::steady_clock::now() < deadline) {
        const netlaglab::WorkloadPollResult result{workload.poll()};
        if (result.state == netlaglab::WorkloadPollState::finished) {
            return true;
        }
        if (result.state == netlaglab::WorkloadPollState::error) {
            return false;
        }
        (void)poll(nullptr, 0, 50);
    }
    if (!workload.send_signal(SIGKILL)) {
        return false;
    }
    return workload.wait().has_value();
}

class SessionProfileChangeAdapter final
    : public netlaglab::ProfileChangeAdapter {
public:
    explicit SessionProfileChangeAdapter(
        netlaglab::network_environment::PreparedNetworkEnvironment& environment)
        : environment_{environment}
    {
    }

    netlaglab::ProfileChangeCompletion apply(
        const netlaglab::ProfileChange& change) override
    {
        switch (environment_.apply_change(change)) {
        case netlaglab::network_environment::ProfileApplyResult::applied:
            return netlaglab::ProfileChangeCompletion::applied;
        case netlaglab::network_environment::ProfileApplyResult::restored_after_failure:
            return netlaglab::ProfileChangeCompletion::restored_after_failure;
        case netlaglab::network_environment::ProfileApplyResult::state_unknown:
            return netlaglab::ProfileChangeCompletion::state_unknown;
        }
        return netlaglab::ProfileChangeCompletion::state_unknown;
    }

private:
    netlaglab::network_environment::PreparedNetworkEnvironment& environment_;
};

[[nodiscard]] int supervise_workload(
    const int supervisor_descriptor,
    netlaglab::WorkloadProcess workload,
    netlaglab::network_environment::PreparedNetworkEnvironment& environment,
    std::ostream& error)
{
    netlaglab::HelperRuntimeConversation conversation;
    SessionProfileChangeAdapter profile_change_adapter{environment};
    auto next_connectivity_check{std::chrono::steady_clock::now() + 1s};
    bool connectivity_checks_enabled{true};
    while (true) {
        const netlaglab::WorkloadPollResult workload_result{workload.poll()};
        if (workload_result.state == netlaglab::WorkloadPollState::error) {
            (void)stop_after_supervisor_loss(workload);
            error << "NetLagLab helper: failed to reap Workload\n";
            return 125;
        }
        if (workload_result.state == netlaglab::WorkloadPollState::finished) {
            conversation.workload_finished();
            if (!send_workload_status(supervisor_descriptor, *workload_result.status)) {
                error << "NetLagLab helper: failed to report Workload completion\n";
                return 125;
            }
            return 0;
        }

        if (connectivity_checks_enabled
            && std::chrono::steady_clock::now() >= next_connectivity_check) {
            const std::optional<netlaglab::network_environment::Failure> failure{
                environment.validate_connectivity()};
            if (failure.has_value()) {
                error << "NetLagLab helper: required Session firewall or NAT state "
                         "was lost or could not be verified\n";
                conversation.connectivity_failed();
                if (!netlaglab::send_socket_text(
                        supervisor_descriptor,
                        netlaglab::helper_conversation_event_message(
                            netlaglab::ConnectivityFailedEvent{}))) {
                    (void)stop_after_supervisor_loss(workload);
                    return 125;
                }
                connectivity_checks_enabled = false;
            } else {
                next_connectivity_check = std::chrono::steady_clock::now() + 1s;
            }
        }

        struct pollfd descriptor {supervisor_descriptor, POLLIN, 0};
        int poll_result{};
        do {
            poll_result = poll(&descriptor, 1, 100);
        } while (poll_result == -1 && errno == EINTR);
        if (poll_result == -1 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            error << "NetLagLab helper: supervisor conversation failed\n";
            (void)stop_after_supervisor_loss(workload);
            return 125;
        }
        if (poll_result == 0) {
            continue;
        }

        std::string bytes;
        const netlaglab::SocketReadResult read_result{
            netlaglab::read_socket_data(supervisor_descriptor, bytes)};
        if (read_result.status != netlaglab::SocketReadStatus::data_received) {
            error << "NetLagLab helper: supervisor disconnected; stopping Workload\n";
            (void)stop_after_supervisor_loss(workload);
            return 125;
        }
        const netlaglab::RuntimeCommandFeedResult commands{
            conversation.receive_bytes(bytes)};
        if (!commands.valid) {
            if (commands.response.has_value()) {
                (void)netlaglab::send_socket_text(
                    supervisor_descriptor, *commands.response);
            }
            (void)stop_after_supervisor_loss(workload);
            return 125;
        }
        for (const netlaglab::HelperRuntimeCommand& command : commands.commands) {
            if (std::holds_alternative<netlaglab::ProfileChange>(command)) {
                const auto& change{std::get<netlaglab::ProfileChange>(command)};
                const std::optional<std::string> response{
                    netlaglab::complete_profile_change(
                        conversation, change, profile_change_adapter)};
                if (!response.has_value()
                    || !netlaglab::send_socket_text(
                        supervisor_descriptor, *response)) {
                    error << "NetLagLab helper: failed to complete Profile Change\n";
                    (void)stop_after_supervisor_loss(workload);
                    return 125;
                }
                continue;
            }
            const int signal_number{
                std::holds_alternative<netlaglab::StopKillCommand>(command)
                    ? SIGKILL : SIGTERM};
            if (!workload.send_signal(signal_number)) {
                (void)send_helper_error(supervisor_descriptor, "Failed to stop Workload.");
                (void)stop_after_supervisor_loss(workload);
                return 125;
            }
        }
    }
}

class ProductionHelperSessionOperations final
    : public netlaglab::HelperSessionOperations {
public:
    ProductionHelperSessionOperations(
        const int supervisor_descriptor,
        std::ostream& error)
        : supervisor_descriptor_{supervisor_descriptor}
        , error_{error}
    {
    }

    [[nodiscard]] netlaglab::network_environment::PreparationResult prepare()
        override
    {
        netlaglab::ResolverViewReadResult resolver_config{
            netlaglab::read_host_resolver_view()};
        if (!resolver_config.configuration.has_value()) {
            error_ << "NetLagLab helper: DNS preflight failed: "
                   << resolver_config.failure << '\n';
            return netlaglab::network_environment::PreparationFailure{
                .primary = {
                    netlaglab::network_environment::Stage::preflight,
                    netlaglab::network_environment::Cause::unsupported_host_configuration,
                },
                .rollback_failures = {},
                .residual = std::nullopt,
            };
        }
        netlaglab::WorkloadResolverViewResult resolver_view{
            netlaglab::create_workload_resolver_view(*resolver_config.configuration)};
        if (!resolver_view.view.has_value()) {
            error_ << "NetLagLab helper: DNS preflight failed: "
                   << resolver_view.failure << '\n';
            return netlaglab::network_environment::PreparationFailure{
                .primary = {
                    netlaglab::network_environment::Stage::preflight,
                    netlaglab::network_environment::Cause::system_failure,
                },
                .rollback_failures = {},
                .residual = std::nullopt,
            };
        }
        resolver_view_.emplace(std::move(*resolver_view.view));
        return netlaglab::network_environment::prepare_network_environment();
    }

    [[nodiscard]] netlaglab::WorkloadLaunchResult launch(
        const netlaglab::WorkloadContext& context,
        const netlaglab::WorkloadIdentity& identity,
        const netlaglab::WorkloadStandardDescriptors& standard_descriptors,
        const netlaglab::network_environment::WorkloadNamespaceEntry& namespace_entry)
        override
    {
        return netlaglab::launch_workload(
            context,
            identity,
            standard_descriptors,
            &namespace_entry,
            resolver_view_.has_value() ? &*resolver_view_ : nullptr);
    }

    [[nodiscard]] bool send(
        const netlaglab::HelperConversationEvent& event) override
    {
        return netlaglab::send_socket_text(
            supervisor_descriptor_,
            netlaglab::helper_conversation_event_message(event));
    }

    [[nodiscard]] int supervise(
        netlaglab::WorkloadProcess workload,
        netlaglab::network_environment::PreparedNetworkEnvironment& environment)
        override
    {
        return supervise_workload(
            supervisor_descriptor_, std::move(workload), environment, error_);
    }

    [[nodiscard]] bool stop_and_reap(
        netlaglab::WorkloadProcess& workload) override
    {
        return stop_after_supervisor_loss(workload);
    }

private:
    int supervisor_descriptor_;
    std::ostream& error_;
    std::optional<netlaglab::WorkloadResolverView> resolver_view_;
};

int usage_error(const std::string_view message)
{
    std::cerr << "NetLagLab helper: " << message << '\n' << usage;
    return usage_error_exit_code;
}

int run_helper(const int argc, char* argv[])
{
    if (argc != 7 || std::string_view{argv[1]} != "--runtime-dir"
        || std::string_view{argv[3]} != "--supervisor-pid"
        || std::string_view{argv[5]} != "--supervisor-start-time") {
        return usage_error("expected runtime directory and Supervisor identity");
    }
    const std::string_view runtime_directory{argv[2]};
    if (runtime_directory.empty() || runtime_directory.front() != '/') {
        return usage_error("runtime directory must be an absolute path");
    }
    const std::optional<pid_t> supervisor_pid{parse_supervisor_pid(argv[4])};
    if (!supervisor_pid.has_value()) {
        return usage_error("Supervisor PID must be a positive process ID");
    }
    const std::optional<std::uint64_t> supervisor_start_time{
        parse_supervisor_start_time(argv[6])};
    if (!supervisor_start_time.has_value()) {
        return usage_error("Supervisor start time must be positive");
    }
    if (geteuid() != 0) {
        std::cerr << "NetLagLab helper: root privileges are required\n";
        return EXIT_FAILURE;
    }

    const int supervisor_process_descriptor{
        static_cast<int>(syscall(SYS_pidfd_open, *supervisor_pid, 0))};
    if (supervisor_process_descriptor == -1) {
        std::cerr << "NetLagLab helper: failed to monitor the launching Supervisor: "
                  << std::strerror(errno) << '\n';
        return 125;
    }
    netlaglab::FileDescriptor supervisor_process{supervisor_process_descriptor};
    const std::optional<netlaglab::ProcessStatus> supervisor_status{
        netlaglab::read_process_status(*supervisor_pid)};
    if (!supervisor_status.has_value()
        || supervisor_status->start_time_ticks != *supervisor_start_time) {
        std::cerr << "NetLagLab helper: launching Supervisor identity changed\n";
        return 125;
    }

    const auto invoking_ids{read_invoking_ids(std::cerr)};
    if (!invoking_ids.has_value()) {
        return 125;
    }
    const netlaglab::SessionPaths paths{std::string{runtime_directory}};
    auto session{validate_session(paths, invoking_ids->uid, std::cerr)};
    if (!session.has_value()) {
        return 125;
    }

    netlaglab::SocketPathOwner helper_socket_path_owner{
        session->directory.get(), netlaglab::SessionPaths::helper_socket_name,
        paths.helper_socket(), invoking_ids->uid};
    if (!helper_socket_path_owner.remove_stale(std::cerr)) {
        return 125;
    }
    const int listening_descriptor{
        helper_socket_path_owner.create_listening_socket(std::cerr)};
    if (listening_descriptor == -1) {
        (void)helper_socket_path_owner.remove_owned();
        return 125;
    }
    netlaglab::FileDescriptor listening_socket{listening_descriptor};

    std::optional<netlaglab::FileDescriptor> supervisor_socket{
        accept_supervisor(
            listening_socket.get(), supervisor_process.get(), std::cerr)};
    if (!supervisor_socket.has_value()) {
        (void)cleanup_helper_listener(listening_socket, helper_socket_path_owner, std::cerr);
        return 125;
    }

    struct ucred peer_credentials {};
    socklen_t credentials_size{sizeof(peer_credentials)};
    if (getsockopt(
            supervisor_socket->get(), SOL_SOCKET, SO_PEERCRED,
            &peer_credentials, &credentials_size) == -1
        || credentials_size != static_cast<socklen_t>(sizeof(peer_credentials))
        || peer_credentials.pid != *supervisor_pid
        || peer_credentials.uid != invoking_ids->uid
        || peer_credentials.gid != invoking_ids->gid) {
        std::cerr << "NetLagLab helper: rejected connection from an unexpected user\n";
        (void)cleanup_helper_listener(listening_socket, helper_socket_path_owner, std::cerr);
        return 125;
    }

    const auto peer_groups{read_peer_groups(peer_credentials.pid, std::cerr)};
    if (!peer_groups.has_value()) {
        (void)cleanup_helper_listener(listening_socket, helper_socket_path_owner, std::cerr);
        return 125;
    }
    const netlaglab::WorkloadIdentity identity{
        invoking_ids->uid, invoking_ids->gid, *peer_groups};

    if (!cleanup_helper_listener(listening_socket, helper_socket_path_owner, std::cerr)) {
        return 125;
    }

    struct sigaction ignore_interrupt {};
    ignore_interrupt.sa_handler = SIG_IGN;
    sigemptyset(&ignore_interrupt.sa_mask);
    if (sigaction(SIGINT, &ignore_interrupt, nullptr) == -1) {
        (void)send_helper_error(supervisor_socket->get(), "Failed to install signal policy.");
        return 125;
    }
    if (!netlaglab::send_socket_text(
            supervisor_socket->get(),
            netlaglab::helper_conversation_event_message(
                netlaglab::ReadyEvent{}))) {
        return 125;
    }

    const auto received_standard_descriptors{
        netlaglab::receive_standard_descriptors(supervisor_socket->get())};
    if (!received_standard_descriptors.has_value()) {
        (void)send_helper_error(
            supervisor_socket->get(), "Invalid standard-descriptor transfer.");
        return 125;
    }

    const auto context{read_start_block(supervisor_socket->get(), std::cerr)};
    if (!context.has_value()) {
        return 125;
    }

    netlaglab::WorkloadStandardDescriptors standard_descriptors;
    for (std::size_t index{}; index < standard_descriptors.sources.size(); ++index) {
        const auto disposition{received_standard_descriptors->dispositions[index]};
        if (disposition == netlaglab::StandardDescriptorDisposition::transferred) {
            standard_descriptors.sources[index] =
                received_standard_descriptors->descriptors[index]->get();
        } else if (disposition
                   == netlaglab::StandardDescriptorDisposition::explicitly_closed) {
            standard_descriptors.sources[index] = netlaglab::close_standard_descriptor;
        }
    }
    ProductionHelperSessionOperations operations{supervisor_socket->get(), std::cerr};
    return netlaglab::run_helper_session(
        operations, *context, identity, standard_descriptors, std::cerr);
}

} // namespace

int main(const int argc, char* argv[])
{
    return run_helper(argc, argv);
}
