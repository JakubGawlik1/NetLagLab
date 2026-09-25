#include "workload_context.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <limits>

extern char** environ;

namespace netlaglab {
namespace {

[[nodiscard]] bool contains_nul(const std::string_view value)
{
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool add_size(
    std::size_t& total,
    const std::size_t value_size)
{
    if (value_size > maximum_context_value_size
        || value_size == std::numeric_limits<std::size_t>::max()
        || total > maximum_context_total_size - (value_size + 1)) {
        return false;
    }

    total += value_size + 1;
    return true;
}

[[nodiscard]] std::string encode_hex(const std::string_view value)
{
    constexpr std::string_view digits{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0F]);
    }
    return encoded;
}

[[nodiscard]] std::optional<unsigned char> decode_nibble(const char character)
{
    if (character >= '0' && character <= '9') {
        return static_cast<unsigned char>(character - '0');
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<unsigned char>(character - 'A' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> decode_hex(const std::string_view encoded)
{
    if (encoded.size() % 2 != 0 || encoded.size() / 2 > maximum_context_value_size) {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (std::size_t index{}; index < encoded.size(); index += 2) {
        const std::optional<unsigned char> high{decode_nibble(encoded[index])};
        const std::optional<unsigned char> low{decode_nibble(encoded[index + 1])};
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }

        const char value{static_cast<char>((*high << 4) | *low)};
        if (value == '\0') {
            return std::nullopt;
        }
        decoded.push_back(value);
    }
    return decoded;
}

[[nodiscard]] bool append_encoded_line(
    std::string& block,
    const std::string_view prefix,
    const std::string_view value,
    std::size_t& total)
{
    if (contains_nul(value) || !add_size(total, value.size())) {
        return false;
    }
    block.append(prefix);
    block.append(encode_hex(value));
    block.push_back('\n');
    return true;
}

[[nodiscard]] bool take_value(
    const std::string& line,
    const std::string_view prefix,
    std::string& destination,
    std::size_t& total)
{
    if (!line.starts_with(prefix)) {
        return false;
    }

    const std::optional<std::string> decoded{
        decode_hex(std::string_view{line}.substr(prefix.size()))};
    if (!decoded.has_value() || !add_size(total, decoded->size())) {
        return false;
    }
    destination = *decoded;
    return true;
}

} // namespace

std::optional<std::string> serialize_start_block(const WorkloadContext& context)
{
    if (context.working_directory.empty()
        || context.working_directory.front() != '/'
        || context.arguments.empty()
        || context.arguments.size() > maximum_context_entry_count
        || context.environment.size() > maximum_context_entry_count) {
        return std::nullopt;
    }

    std::size_t total{};
    std::string block{"START_BEGIN\n"};
    if (!append_encoded_line(block, "CWD ", context.working_directory, total)) {
        return std::nullopt;
    }
    for (const std::string& argument : context.arguments) {
        if (!append_encoded_line(block, "ARG ", argument, total)) {
            return std::nullopt;
        }
    }
    for (const std::string& entry : context.environment) {
        if (!append_encoded_line(block, "ENV ", entry, total)) {
            return std::nullopt;
        }
    }
    block.append("START_END\n");
    return block;
}

std::optional<WorkloadContext> parse_start_block(
    const std::vector<std::string>& lines)
{
    if (lines.size() < 4 || lines.front() != "START_BEGIN"
        || lines.back() != "START_END") {
        return std::nullopt;
    }

    WorkloadContext context;
    std::size_t total{};
    std::size_t index{1};
    if (!take_value(lines[index], "CWD ", context.working_directory, total)
        || context.working_directory.empty()
        || context.working_directory.front() != '/') {
        return std::nullopt;
    }
    ++index;

    while (index + 1 < lines.size() && lines[index].starts_with("ARG ")) {
        if (context.arguments.size() == maximum_context_entry_count) {
            return std::nullopt;
        }
        std::string value;
        if (!take_value(lines[index], "ARG ", value, total)) {
            return std::nullopt;
        }
        context.arguments.push_back(std::move(value));
        ++index;
    }
    if (context.arguments.empty()) {
        return std::nullopt;
    }

    while (index + 1 < lines.size() && lines[index].starts_with("ENV ")) {
        if (context.environment.size() == maximum_context_entry_count) {
            return std::nullopt;
        }
        std::string value;
        if (!take_value(lines[index], "ENV ", value, total)) {
            return std::nullopt;
        }
        context.environment.push_back(std::move(value));
        ++index;
    }

    if (index != lines.size() - 1) {
        return std::nullopt;
    }
    return context;
}

std::optional<WorkloadContext> capture_workload_context(char* const arguments[])
{
    if (arguments == nullptr || arguments[0] == nullptr) {
        return std::nullopt;
    }

    std::error_code path_error;
    const std::filesystem::path current_path{std::filesystem::current_path(path_error)};
    if (path_error || !current_path.is_absolute()) {
        return std::nullopt;
    }

    WorkloadContext context;
    context.working_directory = current_path.string();
    for (std::size_t index{}; arguments[index] != nullptr; ++index) {
        context.arguments.emplace_back(arguments[index]);
    }
    if (environ != nullptr) {
        for (std::size_t index{}; environ[index] != nullptr; ++index) {
            context.environment.emplace_back(environ[index]);
        }
    }

    if (!serialize_start_block(context).has_value()) {
        return std::nullopt;
    }
    return context;
}

} // namespace netlaglab
