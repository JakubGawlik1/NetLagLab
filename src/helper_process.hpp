#pragma once

#include "file_descriptor.hpp"

#include <iosfwd>
#include <optional>
#include <sys/types.h>

namespace netlaglab {

class SessionPaths;

[[nodiscard]] std::optional<pid_t> spawn_helper(
    const SessionPaths& paths,
    std::ostream& error);

[[nodiscard]] std::optional<FileDescriptor> wait_for_helper_connection(
    const SessionPaths& paths,
    pid_t helper_launcher_pid,
    std::ostream& error);

[[nodiscard]] bool wait_for_helper_ready(
    int helper_socket_descriptor,
    std::ostream& error);

} // namespace netlaglab
