#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace netlaglab {

enum class HelperCommand {
    shutdown,
};

enum class HelperEvent {
    ready,
    stopped,
};

inline constexpr std::size_t maximum_helper_message_size{1024};
inline constexpr std::string_view helper_error_prefix{"ERROR "};

[[nodiscard]] std::optional<HelperCommand> parse_helper_command(
    std::string_view line);
[[nodiscard]] std::optional<HelperEvent> parse_helper_event(
    std::string_view line);

[[nodiscard]] std::string_view helper_command_message(HelperCommand command);
[[nodiscard]] std::string_view helper_event_message(HelperEvent event);

} // namespace netlaglab
