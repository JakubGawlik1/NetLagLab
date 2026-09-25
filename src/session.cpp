#include "session.hpp"

#include "file_descriptor.hpp"
#include "helper_process.hpp"
#include "helper_protocol.hpp"
#include "session_lifecycle.hpp"
#include "session_paths.hpp"
#include "session_socket.hpp"
#include "session_validation.hpp"
#include "socket_io.hpp"
#include "workload_context.hpp"

#include "netlaglab/network_profile.hpp"

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
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t maximum_command_size{1024};
constexpr std::size_t maximum_status_size{8 * 1024};

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

[[nodiscard]] bool append_escaped_limited(
    std::string& output,
    const std::string_view input,
    const std::size_t limit)
{
    constexpr std::string_view hex_digits{"0123456789ABCDEF"};
    const auto append_piece = [&output, limit](const std::string_view piece) {
        if (output.size() > limit || piece.size() > limit - output.size()) {
            return false;
        }
        output.append(piece);
        return true;
    };

    for (const unsigned char byte : input) {
        if (byte == '\\') {
            if (!append_piece("\\\\")) {
                return false;
            }
        } else if (byte == '\t') {
            if (!append_piece("\\t")) {
                return false;
            }
        } else if (byte == '\n') {
            if (!append_piece("\\n")) {
                return false;
            }
        } else if (byte == '\r') {
            if (!append_piece("\\r")) {
                return false;
            }
        } else if (byte < 0x20 || byte == 0x7F) {
            const std::array<char, 4> escaped{
                '\\', 'x', hex_digits[byte >> 4], hex_digits[byte & 0x0F]};
            if (!append_piece(std::string_view{escaped.data(), escaped.size()})) {
                return false;
            }
        } else {
            const char character{static_cast<char>(byte)};
            if (!append_piece(std::string_view{&character, 1})) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string escape_command(const std::string_view command)
{
    std::string escaped;
    escaped.reserve(command.size());
    (void)append_escaped_limited(escaped, command, maximum_command_size * 4);
    return escaped;
}

void append_direction_status(
    std::ostringstream& output,
    const std::string_view name,
    const DirectionSettings& settings)
{
    output << name << ":\n"
           << "  delay: " << settings.delay.count() << " ms\n"
           << "  jitter: " << settings.jitter.count() << " ms\n"
           << "  packet loss: " << settings.packet_loss_percent << "%\n"
           << "  bandwidth: ";
    if (settings.bandwidth_kbps.has_value()) {
        output << *settings.bandwidth_kbps << " kbps\n";
    } else {
        output << "unlimited\n";
    }
}

[[nodiscard]] std::string build_status(
    const pid_t workload_pid,
    char* const child_arguments[])
{
    const NetworkProfile profile{};
    std::ostringstream tail_stream;
    tail_stream << "shaping: not applied\n";
    append_direction_status(tail_stream, "outbound", profile.outbound);
    append_direction_status(tail_stream, "inbound", profile.inbound);
    tail_stream << "STATUS_END\n";
    const std::string tail{tail_stream.str()};

    constexpr std::string_view truncated_notice{"arguments: truncated\n"};
    const std::size_t content_limit{
        maximum_status_size - tail.size() - truncated_notice.size()};
    std::ostringstream header_stream;
    header_stream << "STATUS_BEGIN\nstate: running\npid: " << workload_pid
                  << "\nprogram: ";
    std::string response{header_stream.str()};

    bool truncated{false};
    if (!append_escaped_limited(response, child_arguments[0], content_limit - 1)) {
        truncated = true;
    }
    response.push_back('\n');
    if (!truncated) {
        for (std::size_t source_index{1}; child_arguments[source_index] != nullptr;
             ++source_index) {
            const std::string prefix{
                "argument[" + std::to_string(source_index - 1) + "]: "};
            if (response.size() + prefix.size() + 1 > content_limit) {
                truncated = true;
                break;
            }
            response.append(prefix);
            if (!append_escaped_limited(
                    response, child_arguments[source_index], content_limit - 1)) {
                truncated = true;
                response.push_back('\n');
                break;
            }
            response.push_back('\n');
        }
    }
    if (truncated) {
        response.append(truncated_notice);
    }
    response.append(tail);
    return response;
}

enum class CommandResult {
    keep_connected,
    disconnect,
    stop_requested,
};

[[nodiscard]] CommandResult handle_command(
    const int client_descriptor,
    const std::string_view command,
    const pid_t workload_pid,
    char* const child_arguments[])
{
    if (command == "help") {
        constexpr std::string_view help_response{
            "HELP_BEGIN\n"
            "help - Show controller commands\n"
            "status - Show the active session\n"
            "stop - Stop the active Workload\n"
            "detach - Disconnect this controller\n"
            "HELP_END\n"};
        return send_socket_text(client_descriptor, help_response)
            ? CommandResult::keep_connected : CommandResult::disconnect;
    }
    if (command == "status") {
        const std::string status{build_status(workload_pid, child_arguments)};
        return send_socket_text(client_descriptor, status)
            ? CommandResult::keep_connected : CommandResult::disconnect;
    }
    if (command == "stop") {
        return send_socket_text(client_descriptor, "STOPPING\n")
            ? CommandResult::stop_requested : CommandResult::disconnect;
    }
    if (command == "detach") {
        (void)send_socket_text(client_descriptor, "DETACHED\n");
        return CommandResult::disconnect;
    }
    const std::string response{"ERROR Unknown command: " + escape_command(command) + '\n'};
    return send_socket_text(client_descriptor, response)
        ? CommandResult::keep_connected : CommandResult::disconnect;
}

enum class ClientReadResult {
    connected,
    disconnected,
    stop_requested,
};

[[nodiscard]] ClientReadResult read_client_commands(
    const int client_descriptor,
    std::string& command_buffer,
    const pid_t workload_pid,
    char* const child_arguments[])
{
    const SocketReadResult read_result{read_socket_data(client_descriptor, command_buffer)};
    if (read_result.status != SocketReadStatus::data_received) {
        return ClientReadResult::disconnected;
    }
    while (true) {
        const std::optional<std::string> command{take_next_line(command_buffer)};
        if (!command.has_value()) {
            break;
        }
        if (command->size() > maximum_command_size) {
            (void)send_socket_text(client_descriptor, "ERROR Command exceeds 1024 bytes.\n");
            return ClientReadResult::disconnected;
        }
        if (command->empty()) {
            continue;
        }
        const CommandResult result{
            handle_command(client_descriptor, *command, workload_pid, child_arguments)};
        if (result == CommandResult::disconnect) {
            return ClientReadResult::disconnected;
        }
        if (result == CommandResult::stop_requested) {
            return ClientReadResult::stop_requested;
        }
    }
    if (command_buffer.size() > maximum_command_size) {
        (void)send_socket_text(client_descriptor, "ERROR Command exceeds 1024 bytes.\n");
        return ClientReadResult::disconnected;
    }
    return ClientReadResult::connected;
}

[[nodiscard]] bool accept_controller(
    const int listening_descriptor,
    std::optional<FileDescriptor>& client,
    std::string& command_buffer,
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
        (void)send_socket_text(
            rejected_client.get(), "ERROR Another controller is already attached.\n");
        return true;
    }
    client.emplace(accepted_descriptor);
    command_buffer.clear();
    if (!send_socket_text(client->get(), "ATTACHED\n")) {
        client.reset();
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

            for (const HelperConversationEvent event : events) {
                if (event.kind == HelperConversationEventKind::ready) {
                    if (!send_standard_descriptors(
                            helper_socket_.get(),
                            {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})) {
                        error_ << "NetLagLab: failed to transfer standard descriptors "
                                  "to helper\n";
                        return false;
                    }
                    if (!send_socket_text(helper_socket_.get(), start_block_)) {
                        error_ << "NetLagLab: failed to send Workload context to helper\n";
                        return false;
                    }
                    return true;
                }
                error_ << "NetLagLab: invalid helper event before READY\n";
                return false;
            }
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
                conversation_failed_ = true;
                return {LifecycleEventKind::conversation_lost};
            }

            if ((descriptors[3].revents & POLLIN) != 0) {
                const std::size_t count{signal_pipe_.take_interrupt_count()};
                for (std::size_t index{}; index < count; ++index) {
                    pending_events_.push_back({LifecycleEventKind::terminal_interrupt});
                }
            }
            if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
                read_helper_events();
            }
            if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
                conversation_failed_ = true;
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
            }

            if (activated_ && (descriptors[1].revents & POLLIN) != 0
                && !accept_controller(
                    listening_socket_->get(), client_, command_buffer_, error_)) {
                conversation_failed_ = true;
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
            }
            if ((descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                conversation_failed_ = true;
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
            }

            if (client_.has_value() && (descriptors[2].revents & POLLIN) != 0) {
                const ClientReadResult result{read_client_commands(
                    client_->get(), command_buffer_, workload_pid_, child_arguments_)};
                if (result == ClientReadResult::disconnected) {
                    client_.reset();
                    command_buffer_.clear();
                    pending_events_.push_back({LifecycleEventKind::controller_lost});
                } else if (result == ClientReadResult::stop_requested) {
                    pending_events_.push_back({LifecycleEventKind::stop_requested});
                }
            }
            if (client_.has_value()
                && (descriptors[2].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                client_.reset();
                command_buffer_.clear();
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
        launcher_clean_ = reap_helper_launcher(helper_launcher_pid_, error_);
        return launcher_clean_;
    }

    bool finalize(const bool session_succeeded) override
    {
        listening_socket_.reset();
        const int cleanup_error{control_socket_owner_.remove_owned()};
        if (cleanup_error != 0) {
            error_ << "NetLagLab: failed to remove control.sock: "
                   << std::strerror(cleanup_error) << '\n';
        }
        const bool session_clean{
            session_succeeded && launcher_clean_ && cleanup_error == 0
            && final_cleanup_succeeded_ && !conversation_failed_};
        if (client_.has_value()) {
            (void)send_socket_text(
                client_->get(),
                session_clean ? "SESSION_ENDED\n" : "SESSION_FAILED\n");
        }
        client_.reset();
        return cleanup_error == 0;
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
            conversation_failed_ = true;
            pending_events_.push_back({LifecycleEventKind::conversation_lost});
            return;
        }

        for (const HelperConversationEvent event : helper_events) {
            if (event.kind == HelperConversationEventKind::ready) {
                conversation_failed_ = true;
                pending_events_.push_back({LifecycleEventKind::conversation_lost});
                continue;
            }
            if (event.kind == HelperConversationEventKind::activated) {
                workload_pid_ = static_cast<pid_t>(event.value);
                const int descriptor{
                    control_socket_owner_.create_listening_socket(error_)};
                if (descriptor == -1) {
                    conversation_failed_ = true;
                    pending_events_.push_back({LifecycleEventKind::conversation_lost});
                    continue;
                }
                listening_socket_.emplace(descriptor);
                activated_ = true;
            } else if (event.kind == HelperConversationEventKind::cleanup_succeeded) {
                final_cleanup_succeeded_ = true;
            } else if (event.kind == HelperConversationEventKind::cleanup_failed
                       || event.kind == HelperConversationEventKind::conversation_lost) {
                conversation_failed_ = true;
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
    std::string command_buffer_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    pid_t workload_pid_{};
    bool activated_{false};
    bool final_cleanup_succeeded_{false};
    bool conversation_failed_{false};
    bool launcher_clean_{false};
};

void report_outcome(const SessionOutcome& outcome, std::ostream& error)
{
    if (!outcome.infrastructure_succeeded()) {
        error << "NetLagLab: Session infrastructure failed";
        if (outcome.workload.has_value()) {
            error << "; Workload result was " << outcome.workload->exit_code();
        }
        error << '\n';
        return;
    }
    if (isatty(STDERR_FILENO) == 1 && outcome.workload.has_value()) {
        if (outcome.workload->kind == WorkloadResultKind::signaled) {
            error << "NetLagLab: Session ended; Workload terminated by signal "
                  << outcome.workload->value << '\n';
        } else if (outcome.workload->kind == WorkloadResultKind::exited) {
            error << "NetLagLab: Session ended; Workload exit code: "
                  << outcome.workload->value << '\n';
        }
    }
}

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
    report_outcome(outcome, error);
    return outcome.exit_code();
}

} // namespace netlaglab
