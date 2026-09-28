#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <sys/types.h>

namespace netlaglab {

struct ProcessStatus {
    pid_t parent_pid;
    std::uint64_t start_time_ticks;
};

[[nodiscard]] std::optional<ProcessStatus> parse_process_status(
    std::string_view stat_line);

[[nodiscard]] std::optional<ProcessStatus> read_process_status(pid_t pid);

} // namespace netlaglab
