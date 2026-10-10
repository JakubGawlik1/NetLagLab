#include "recovery_journal.hpp"

#include "command_runner.hpp"

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace netlaglab::network_environment::detail {
namespace {

constexpr std::array<std::string_view, 2> ufw_paths{
    "/usr/bin/ufw",
    "/usr/sbin/ufw",
};
constexpr auto command_limit{std::chrono::seconds{5}};

[[nodiscard]] bool trusted_ufw(std::string_view path)
{
    const std::string owned_path{path};
    struct stat status {};
    return stat(owned_path.c_str(), &status) == 0 && S_ISREG(status.st_mode)
        && status.st_uid == 0 && (status.st_mode & 0111) != 0
        && (status.st_mode & 0022) == 0;
}

[[nodiscard]] std::string_view find_ufw()
{
    for (const std::string_view path : ufw_paths) {
        if (trusted_ufw(path)) {
            return path;
        }
    }
    return {};
}

[[nodiscard]] std::string normalize_columns(const std::string_view line)
{
    std::string normalized;
    normalized.reserve(line.size());
    bool previous_space{};
    for (const char character : line) {
        if (character == ' ') {
            if (!normalized.empty() && !previous_space) {
                normalized.push_back(' ');
            }
            previous_space = true;
        } else {
            normalized.push_back(character);
            previous_space = false;
        }
    }
    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

class LinuxPersistentFirewallBackend final : public PersistentFirewallBackend {
public:
    [[nodiscard]] LiveRuleState inspect(
        const std::string_view transaction_id) override
    {
        const std::string_view path{find_ufw()};
        if (path.empty()) {
            return LiveRuleState::failure;
        }
        const std::vector<std::string> arguments{"status", "numbered"};
        const CommandResult result{run_command_capturing_stdout(
            path,
            arguments,
            std::chrono::steady_clock::now() + command_limit)};
        if (result.kind != CommandResultKind::success
            || !result.standard_error.empty()) {
            return LiveRuleState::failure;
        }
        return decode_ufw_route_status(result.standard_output, transaction_id);
    }

    [[nodiscard]] bool remove_exact(
        const std::string_view transaction_id) override
    {
        const std::string_view path{find_ufw()};
        if (path.empty()) {
            return false;
        }
        const std::vector<std::string> arguments{
            "--force",
            "route",
            "delete",
            "allow",
            "in",
            "on",
            "nll-host",
            "from",
            "10.200.0.2/32",
            "to",
            "any",
            "comment",
            "netlaglab-" + std::string{transaction_id},
        };
        const CommandResult result{run_command(
            path,
            arguments,
            std::chrono::steady_clock::now() + command_limit)};
        return result.kind == CommandResultKind::success;
    }
};

} // namespace

LiveRuleState decode_ufw_route_status(
    const std::string_view output,
    const std::string_view transaction_id)
{
    if (transaction_id.size() != 32) {
        return LiveRuleState::failure;
    }
    for (const char digit : transaction_id) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return LiveRuleState::failure;
        }
    }
    bool active{};
    std::size_t matching_marker_count{};
    std::size_t exact_count{};
    std::size_t same_source_count{};
    std::size_t offset{};
    while (offset < output.size()) {
        const std::size_t end{output.find('\n', offset)};
        std::string_view line{output.substr(
            offset,
            end == std::string_view::npos ? output.size() - offset
                                          : end - offset)};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line == "Status: active") {
            active = true;
        }
        const std::size_t close_bracket{line.find(']')};
        if (line.starts_with('[') && close_bracket != std::string_view::npos) {
            line.remove_prefix(close_bracket + 1);
            const std::string normalized{normalize_columns(line)};
            const std::size_t comment_separator{normalized.find(" # ")};
            const bool has_matching_marker{
                comment_separator != std::string::npos
                && std::string_view{normalized}.substr(comment_separator + 3)
                    == "netlaglab-" + std::string{transaction_id}};
            if (has_matching_marker) {
                ++matching_marker_count;
            }
            if (line.find("10.200.0.2") != std::string_view::npos) {
                ++same_source_count;
                const std::string normalized_rule{
                    normalized.substr(0, comment_separator)};
                if (has_matching_marker
                    && (normalized_rule
                            == "Anywhere ALLOW FWD 10.200.0.2 on nll-host"
                        || normalized_rule
                            == "Anywhere ALLOW FWD 10.200.0.2/32 on nll-host")) {
                    ++exact_count;
                }
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    if (!active) {
        return LiveRuleState::failure;
    }
    if (matching_marker_count > 1) {
        return LiveRuleState::ambiguous;
    }
    if (matching_marker_count == 1 && exact_count != 1) {
        return LiveRuleState::mismatch;
    }
    if (same_source_count > exact_count) {
        return LiveRuleState::mismatch;
    }
    return exact_count == 1 ? LiveRuleState::exact : LiveRuleState::absent;
}

std::unique_ptr<PersistentFirewallBackend>
make_linux_persistent_firewall_backend()
{
    return std::make_unique<LinuxPersistentFirewallBackend>();
}

} // namespace netlaglab::network_environment::detail
