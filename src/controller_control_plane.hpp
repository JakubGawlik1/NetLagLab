#pragma once

#include <string>
#include <sys/types.h>

namespace netlaglab {

enum class ControllerReadResult {
    connected,
    disconnected,
    stop_requested,
};

class ControllerConversation {
public:
    ControllerConversation(
        pid_t workload_pid,
        char* const child_arguments[]) noexcept;

    [[nodiscard]] ControllerReadResult receive(int socket_descriptor);

private:
    pid_t workload_pid_;
    char* const* child_arguments_;
    std::string read_buffer_;
};

[[nodiscard]] bool send_controller_attached(int socket_descriptor);
void reject_additional_controller(int socket_descriptor);
void send_controller_session_result(int socket_descriptor, bool session_succeeded);

} // namespace netlaglab
