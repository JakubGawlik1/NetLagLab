#include "session.hpp"

#include "controller_control_plane.hpp"
#include "controller_session_outcome.hpp"
#include "file_descriptor.hpp"
#include "helper_process.hpp"
#include "helper_protocol.hpp"
#include "session_lifecycle.hpp"
#include "session_paths.hpp"
#include "session_presentation.hpp"
#include "session_socket.hpp"
#include "session_validation.hpp"
#include "socket_io.hpp"
#include "workload_context.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <optional>
#include <ostream>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab {
namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t signal_pipe_write_descriptor{-1};

extern "C" void handle_terminal_interrupt(const int)
{
    const int saved_errno{errno};
    if (signal_pipe_write_descriptor != -1) {
        constexpr unsigned char marker{1};
        (void)write(
            static_cast<int>(signal_pipe_write_descriptor), &marker, sizeof(marker));
    }
    errno = saved_errno;
}

class SignalPipe {
public:
    static std::optional<SignalPipe> create(std::ostream& error)
    {
        int descriptors[2]{};
        if (pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == -1) {
            error << "NetLagLab: failed to create signal self-pipe: "
                  << std::strerror(errno) << '\n';
            return std::nullopt;
        }
        FileDescriptor read_descriptor{descriptors[0]};
        FileDescriptor write_descriptor{descriptors[1]};

        struct sigaction action {};
        action.sa_handler = handle_terminal_interrupt;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        struct sigaction previous_action {};
        if (sigaction(SIGINT, &action, &previous_action) == -1) {
            error << "NetLagLab: failed to install SIGINT handler: "
                  << std::strerror(errno) << '\n';
            return std::nullopt;
        }

        signal_pipe_write_descriptor = write_descriptor.get();
        return SignalPipe{
            std::move(read_descriptor),
            std::move(write_descriptor),
            previous_action};
    }

    ~SignalPipe()
    {
        if (installed_) {
            signal_pipe_write_descriptor = -1;
            (void)sigaction(SIGINT, &previous_action_, nullptr);
        }
    }

    SignalPipe(const SignalPipe&) = delete;
    SignalPipe& operator=(const SignalPipe&) = delete;

    SignalPipe(SignalPipe&& other) noexcept
        : read_descriptor_{std::move(other.read_descriptor_)},
          write_descriptor_{std::move(other.write_descriptor_)},
          previous_action_{other.previous_action_},
          installed_{std::exchange(other.installed_, false)}
    {
        if (installed_) {
            signal_pipe_write_descriptor = write_descriptor_.get();
        }
    }

    [[nodiscard]] int descriptor() const noexcept
    {
        return read_descriptor_.get();
    }

    [[nodiscard]] std::size_t take_interrupt_count() const noexcept
    {
        std::size_t count{};
        std::array<unsigned char, 64> markers{};
        while (true) {
            const ssize_t result{read(
                read_descriptor_.get(), markers.data(), markers.size())};
            if (result > 0) {
                count += static_cast<std::size_t>(result);
            } else if (result == -1 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        return count;
    }

private:
    SignalPipe(
        FileDescriptor read_descriptor,
        FileDescriptor write_descriptor,
        const struct sigaction previous_action) noexcept
        : read_descriptor_{std::move(read_descriptor)},
          write_descriptor_{std::move(write_descriptor)},
          previous_action_{previous_action}
    {
    }

    FileDescriptor read_descriptor_;
    FileDescriptor write_descriptor_;
    struct sigaction previous_action_ {};
    bool installed_{true};
};

[[nodiscard]] bool accept_controller(
    const int listening_descriptor,
    std::optional<FileDescriptor>& client,
    std::optional<ControllerConversation>& conversation,
    const pid_t workload_pid,
    char* const child_arguments[],
    std::ostream& error)
{
    int accepted_descriptor{};
    do {
        accepted_descriptor = accept4(
            listening_descriptor, nullptr, nullptr, SOCK_CLOEXEC);
    } while (accepted_descriptor == -1 && errno == EINTR);
    if (accepted_descriptor == -1) {
        error << "NetLagLab: failed to accept controller: "
              << std::strerror(errno) << '\n';
        return false;
    }
    if (client.has_value()) {
        FileDescriptor rejected_client{accepted_descriptor};
        reject_additional_controller(rejected_client.get());
        return true;
    }
    client.emplace(accepted_descriptor);
    conversation.emplace(workload_pid, child_arguments);
    if (!send_controller_attached(client->get())) {
        client.reset();
        conversation.reset();
    }
    return true;
}

[[nodiscard]] LifecycleEvent map_helper_event(const HelperConversationEvent event)
{
    switch (event.kind) {
    case HelperConversationEventKind::activated:
        return {LifecycleEventKind::activated, event.value};
    case HelperConversationEventKind::activation_failed:
        return {LifecycleEventKind::activation_failed, event.value};
    case HelperConversationEventKind::workload_exited:
        return {LifecycleEventKind::workload_exited, event.value};
    case HelperConversationEventKind::workload_signaled:
        return {LifecycleEventKind::workload_signaled, event.value};
    case HelperConversationEventKind::cleanup_succeeded:
        return {LifecycleEventKind::cleanup_succeeded};
    case HelperConversationEventKind::cleanup_failed:
        return {LifecycleEventKind::cleanup_failed};
    case HelperConversationEventKind::conversation_lost:
        return {LifecycleEventKind::conversation_lost};
    case HelperConversationEventKind::ready:
        break;
    }
    return {LifecycleEventKind::conversation_lost};
}

class ProductionLifecycleAdapter final : public LifecycleAdapter {
public:
    ProductionLifecycleAdapter(
        const pid_t helper_launcher_pid,
        FileDescriptor helper_socket,
        std::string start_block,
        SocketPathOwner& control_socket_owner,
        char* const child_arguments[],
        const SignalPipe& signal_pipe,
        std::ostream& error)
        : helper_launcher_pid_{helper_launcher_pid},
          helper_socket_{std::move(helper_socket)},
          start_block_{std::move(start_block)},
          control_socket_owner_{control_socket_owner},
          child_arguments_{child_arguments},
          signal_pipe_{signal_pipe},
          error_{error}
    {
    }

    bool begin() override
    {
        while (true) {
            std::string bytes;
            const SocketReadResult result{read_socket_data(helper_socket_.get(), bytes)};
            std::vector<HelperConversationEvent> events;
            if (result.status == SocketReadStatus::data_received) {
                events = conversation_.receive_bytes(bytes);
            } else if (result.status == SocketReadStatus::peer_closed) {
                events = conversation_.peer_closed();
            } else {
                error_ << "NetLagLab: failed to read from privileged helper: "
                       << std::strerror(result.error_code) << '\n';
                return false;
            }

            if (events.empty()) {
                continue;
            }
            if (events.size() != 1
                || events.front().kind != HelperConversationEventKind::ready) {
                error_ << "NetLagLab: invalid helper event before READY\n";
                return false;
            }

            if (!send_standard_descriptors(
                    helper_socket_.get(),
                    {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})) {
                error_ << "NetLagLab: failed to transfer standard descriptors to helper\n";
                return false;
            }
            if (!send_socket_text(helper_socket_.get(), start_block_)) {
                error_ << "NetLagLab: failed to send Workload context to helper\n";
                return false;
            }
            return true;
        }
    }

    LifecycleEvent wait(const LifecycleWait wait_mode) override
    {
        if (!pending_events_.empty()) {
            return take_pending_event();
        }

        if (wait_mode == LifecycleWait::indefinitely) {
            deadline_.reset();
        } else if (!deadline_.has_value()) {
            deadline_ = std::chrono::steady_clock::now() + 5s;
        }

        while (true) {
            int timeout{-1};
            if (deadline_.has_value()) {
                const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(
                    *deadline_ - std::chrono::steady_clock::now())};
                if (remaining <= 0ms) {
                    deadline_.reset();
                    return {LifecycleEventKind::deadline_expired};
                }
                timeout = static_cast<int>(remaining.count());
            }

            std::array<struct pollfd, 4> descriptors{{
                {helper_socket_.get(), POLLIN, 0},
                {listening_socket_.has_value() ? listening_socket_->get() : -1, POLLIN, 0},
                {client_.has_value() ? client_->get() : -1, POLLIN, 0},
                {signal_pipe_.descriptor(), POLLIN, 0},
            }};
            int poll_result{};
            do {
                poll_result = poll(descriptors.data(), descriptors.size(), timeout);
            } while (poll_result == -1 && errno == EINTR);
            if (poll_result == 0) {
                deadline_.reset();
                return {LifecycleEventKind::deadline_expired};
            }
            if (poll_result == -1) {
                error_ << "NetLagLab: lifecycle poll failed: "
                       << std::strerror(errno) << '\n';
                return {LifecycleEventKind::conversation_lost};
            }

            if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
                read_helper_events();
            }
            if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
            }

            if ((descriptors[3].revents & POLLIN) != 0) {
                const std::size_t count{signal_pipe_.take_interrupt_count()};
                if (!workload_finished_) {
                    for (std::size_t index{}; index < count; ++index) {
                        pending_events_.push_back(
                            {LifecycleEventKind::terminal_interrupt});
                    }
                }
            }

            if (activated_ && (descriptors[1].revents & POLLIN) != 0
                && !accept_controller(
                    listening_socket_->get(),
                    client_,
                    controller_conversation_,
                    workload_pid_,
                    child_arguments_,
                    error_)) {
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
            }

            if (!workload_finished_ && client_.has_value()
                && (descriptors[2].revents & POLLIN) != 0) {
                const ControllerReadResult result{
                    controller_conversation_->receive(client_->get())};
                if (result == ControllerReadResult::disconnected) {
                    client_.reset();
                    controller_conversation_.reset();
                    pending_events_.push_back({LifecycleEventKind::controller_lost});
                } else if (result == ControllerReadResult::stop_requested) {
                    pending_events_.push_back({LifecycleEventKind::stop_requested});
                }
            }
            if (client_.has_value()
                && (descriptors[2].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                client_.reset();
                controller_conversation_.reset();
                pending_events_.push_back({LifecycleEventKind::controller_lost});
            }

            if (!pending_events_.empty()) {
                return take_pending_event();
            }
        }
    }

    bool request_stop(const StopRequest request) override
    {
        return send_socket_text(
            helper_socket_.get(),
            helper_runtime_command_message(
                request == StopRequest::terminate
                    ? HelperRuntimeCommand::stop_terminate
                    : HelperRuntimeCommand::stop_kill));
    }

    bool reap_launcher() override
    {
        helper_socket_.reset();
        return reap_helper_launcher(helper_launcher_pid_, error_);
    }

    bool finalize() override
    {
        listening_socket_.reset();
        const int cleanup_error{control_socket_owner_.remove_owned()};
        if (cleanup_error != 0) {
            error_ << "NetLagLab: failed to remove control.sock: "
                   << std::strerror(cleanup_error) << '\n';
        }
        return cleanup_error == 0;
    }

    void publish_outcome(const SessionOutcome& outcome) override
    {
        if (client_.has_value()) {
            const ControllerSessionOutcomeSerialization serialized{
                serialize_controller_session_outcome(outcome)};
            if (const auto* block{std::get_if<std::string>(&serialized)}) {
                (void)send_socket_text(client_->get(), *block);
            }
        }
        client_.reset();
        controller_conversation_.reset();
    }

private:
    void read_helper_events()
    {
        std::string bytes;
        const SocketReadResult result{read_socket_data(helper_socket_.get(), bytes)};
        std::vector<HelperConversationEvent> helper_events;
        if (result.status == SocketReadStatus::data_received) {
            helper_events = conversation_.receive_bytes(bytes);
        } else if (result.status == SocketReadStatus::peer_closed) {
            helper_events = conversation_.peer_closed();
        } else {
            pending_events_.push_back({LifecycleEventKind::conversation_lost});
            return;
        }

        for (const HelperConversationEvent event : helper_events) {
            if (event.kind == HelperConversationEventKind::ready) {
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
                continue;
            }
            if (event.kind == HelperConversationEventKind::activated) {
                workload_pid_ = static_cast<pid_t>(event.value);
                const int descriptor{
                    control_socket_owner_.create_listening_socket(error_)};
                if (descriptor == -1) {
                    pending_events_.push_back({LifecycleEventKind::conversation_lost});
                    continue;
                }
                listening_socket_.emplace(descriptor);
                activated_ = true;
            } else if (event.kind == HelperConversationEventKind::workload_exited
                       || event.kind
                           == HelperConversationEventKind::workload_signaled) {
                workload_finished_ = true;
                activated_ = false;
            }
            pending_events_.push_back(map_helper_event(event));
        }
    }

    [[nodiscard]] LifecycleEvent take_pending_event()
    {
        const LifecycleEvent event{pending_events_.front()};
        pending_events_.pop_front();
        return event;
    }

    pid_t helper_launcher_pid_;
    FileDescriptor helper_socket_;
    std::string start_block_;
    std::optional<FileDescriptor> listening_socket_;
    SocketPathOwner& control_socket_owner_;
    char* const* child_arguments_;
    const SignalPipe& signal_pipe_;
    std::ostream& error_;
    SupervisorHelperConversation conversation_;
    std::deque<LifecycleEvent> pending_events_;
    std::optional<FileDescriptor> client_;
    std::optional<ControllerConversation> controller_conversation_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    pid_t workload_pid_{};
    bool activated_{false};
    bool workload_finished_{false};
};

} // namespace

int run_session(char* const child_arguments[], std::ostream& error)
{
    const std::optional<WorkloadContext> context{
        capture_workload_context(child_arguments)};
    if (!context.has_value()) {
        error << "NetLagLab: failed to capture bounded Workload execution context\n";
        return 125;
    }
    const std::optional<std::string> start_block{serialize_start_block(*context)};
    if (!start_block.has_value()) {
        error << "NetLagLab: Workload execution context exceeds protocol limits\n";
        return 125;
    }

    const std::optional<SessionPaths> paths{make_session_paths_from_environment(error)};
    if (!paths.has_value()) {
        return 125;
    }
    const std::optional<FileDescriptor> runtime_directory{
        open_and_validate_runtime_directory(
            paths->xdg_runtime_directory(), geteuid(), error)};
    if (!runtime_directory.has_value()) {
        return 125;
    }

    const int mkdir_result{mkdirat(
        runtime_directory->get(), SessionPaths::session_directory_name.data(), 0700)};
    if (mkdir_result == -1 && errno != EEXIST) {
        error << "NetLagLab: failed to create the netlaglab runtime directory: "
              << std::strerror(errno) << '\n';
        return 125;
    }
    if (mkdir_result == 0
        && fchmodat(
               runtime_directory->get(), SessionPaths::session_directory_name.data(), 0700, 0)
            == -1) {
        error << "NetLagLab: failed to set runtime directory permissions: "
              << std::strerror(errno) << '\n';
        return 125;
    }

    const std::optional<FileDescriptor> session_directory{
        open_and_validate_session_directory(runtime_directory->get(), geteuid(), error)};
    if (!session_directory.has_value()) {
        return 125;
    }
    const int lock_descriptor{openat(
        session_directory->get(), SessionPaths::lock_file_name.data(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (lock_descriptor == -1) {
        error << "NetLagLab: failed to open session.lock: "
              << std::strerror(errno) << '\n';
        return 125;
    }
    const FileDescriptor session_lock{lock_descriptor};
    struct stat lock_status {};
    if (fstat(session_lock.get(), &lock_status) == -1
        || !S_ISREG(lock_status.st_mode) || lock_status.st_uid != geteuid()) {
        error << "NetLagLab: session.lock must be a regular file owned by the current user\n";
        return 125;
    }
    if (fchmod(session_lock.get(), 0600) == -1
        || flock(session_lock.get(), LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            error << "NetLagLab: another Session is already active for this user\n";
        } else {
            error << "NetLagLab: failed to acquire session.lock: "
                  << std::strerror(errno) << '\n';
        }
        return 125;
    }

    std::optional<SignalPipe> signal_pipe{SignalPipe::create(error)};
    if (!signal_pipe.has_value()) {
        return 125;
    }

    error << "NetLagLab: root privileges are required for the isolated Session helper. "
             "A future network backend may separately request confirmation for an exact "
             "firewall exception.\n";
    const std::optional<pid_t> helper_launcher_pid{spawn_helper(*paths, error)};
    if (!helper_launcher_pid.has_value()) {
        return 125;
    }

    std::optional<FileDescriptor> helper_socket{
        wait_for_helper_connection(*paths, *helper_launcher_pid, error)};
    if (!helper_socket.has_value()) {
        (void)reap_helper_launcher(*helper_launcher_pid, error);
        return 125;
    }

    SocketPathOwner control_socket_owner{
        session_directory->get(), SessionPaths::control_socket_name,
        paths->control_socket(), geteuid()};
    if (!control_socket_owner.remove_stale(error)) {
        helper_socket.reset();
        (void)reap_helper_launcher(*helper_launcher_pid, error);
        return 125;
    }
    ProductionLifecycleAdapter adapter{
        *helper_launcher_pid,
        std::move(*helper_socket),
        *start_block,
        control_socket_owner,
        child_arguments,
        *signal_pipe,
        error};
    const SessionOutcome outcome{run_session_lifecycle(adapter)};
    report_session_outcome(outcome, error);
    return session_exit_status(outcome);
}

} // namespace netlaglab
