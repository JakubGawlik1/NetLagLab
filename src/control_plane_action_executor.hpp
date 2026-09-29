#pragma once

#include "controller_control_plane.hpp"
#include "file_descriptor.hpp"
#include "helper_protocol.hpp"
#include "session_lifecycle.hpp"

#include <optional>
#include <vector>

namespace netlaglab {

[[nodiscard]] std::vector<LifecycleEvent> execute_control_plane_actions(
    const std::vector<ControlPlaneAction>& actions,
    ControllerControlPlane& control_plane,
    std::optional<FileDescriptor>& controller,
    int helper_descriptor,
    SupervisorHelperConversation& helper_conversation);

} // namespace netlaglab
