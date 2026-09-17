#include "helper_protocol.hpp"

namespace netlaglab {

std::optional<HelperCommand> parse_helper_command(const std::string_view line)
{
    if (line == "SHUTDOWN") {
        return HelperCommand::shutdown;
    }

    return std::nullopt;
}

std::string_view helper_command_message(const HelperCommand command)
{
    switch (command) {
    case HelperCommand::shutdown:
        return "SHUTDOWN\n";
    }

    return {};
}

std::string_view helper_event_message(const HelperEvent event)
{
    switch (event) {
    case HelperEvent::ready:
        return "READY\n";
    case HelperEvent::stopped:
        return "STOPPED\n";
    }

    return {};
}

} // namespace netlaglab
