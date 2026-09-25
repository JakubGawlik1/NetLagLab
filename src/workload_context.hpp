#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace netlaglab {

struct WorkloadContext {
    std::string working_directory;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;

    bool operator==(const WorkloadContext&) const = default;
};

inline constexpr std::size_t maximum_context_value_size{128 * 1024};
inline constexpr std::size_t maximum_context_total_size{1024 * 1024};
inline constexpr std::size_t maximum_context_entry_count{4096};

[[nodiscard]] std::optional<std::string> serialize_start_block(
    const WorkloadContext& context);
[[nodiscard]] std::optional<WorkloadContext> parse_start_block(
    const std::vector<std::string>& lines);
[[nodiscard]] std::optional<WorkloadContext> capture_workload_context(
    char* const arguments[]);

} // namespace netlaglab
