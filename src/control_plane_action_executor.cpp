#include "control_plane_action_executor.hpp"

#include "socket_io.hpp"

#include <variant>

namespace netlaglab {

std::vector<LifecycleEvent> execute_control_plane_actions(
    const std::vector<ControlPlaneAction>& actions,
    ControllerControlPlane& control_plane,
    std::optional<FileDescriptor>& controller,
    const int helper_descriptor,
    SupervisorHelperConversation& helper_conversation)
{
    std::vector<LifecycleEvent> events;
    for (const ControlPlaneAction& action : actions) {
        if (std::holds_alternative<RequestLifecycleStopAction>(action)) {
            events.push_back({LifecycleEventKind::stop_requested});
            continue;
        }
        if (const auto* send{std::get_if<SendControllerTextAction>(&action)}) {
            if (!controller.has_value()
                || !send_socket_text(controller->get(), send->text)) {
                controller.reset();
                (void)control_plane.handle(ControllerWriteFailedEvent{});
                break;
            }
            continue;
        }
        if (std::holds_alternative<DisconnectControllerAction>(action)) {
            controller.reset();
            continue;
        }
        if (std::holds_alternative<FailSessionForUnknownProfileStateAction>(
                action)) {
            events.push_back({LifecycleEventKind::profile_state_unknown});
            continue;
        }

        const auto& dispatch{std::get<DispatchProfileChangeAction>(action)};
        const std::optional<std::string> frame{
            helper_conversation.begin_profile_change(dispatch.change)};
        if (!frame.has_value()
            || !send_socket_text(helper_descriptor, *frame)) {
            events.push_back({LifecycleEventKind::conversation_lost});
            break;
        }
    }
    return events;
}

} // namespace netlaglab
