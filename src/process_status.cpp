#include "process_status.hpp"

#include <charconv>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace netlaglab {
namespace {

template<typename Value>
[[nodiscard]] std::optional<Value> parse_number(const std::string_view text)
{
    Value value{};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<ProcessStatus> parse_process_status(const std::string_view stat_line)
{
    const std::size_t closing_parenthesis{stat_line.rfind(") ")};
    if (closing_parenthesis == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view remaining{stat_line.substr(closing_parenthesis + 2)};
    std::vector<std::string_view> fields;
    while (!remaining.empty()) {
        const std::size_t separator{remaining.find(' ')};
        const std::string_view field{remaining.substr(0, separator)};
        if (!field.empty()) {
            fields.push_back(field);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }

    // The suffix starts at field 3 (state); ppid is field 4 and starttime is 22.
    if (fields.size() < 20) {
        return std::nullopt;
    }
    const std::optional<pid_t> parent_pid{parse_number<pid_t>(fields[1])};
    const std::optional<std::uint64_t> start_time{
        parse_number<std::uint64_t>(fields[19])};
    if (!parent_pid.has_value() || !start_time.has_value()) {
        return std::nullopt;
    }
    return ProcessStatus{*parent_pid, *start_time};
}

std::optional<ProcessStatus> read_process_status(const pid_t pid)
{
    std::ifstream stat{std::string{"/proc/"} + std::to_string(pid) + "/stat"};
    std::string line;
    if (!std::getline(stat, line)) {
        return std::nullopt;
    }
    return parse_process_status(line);
}

} // namespace netlaglab
