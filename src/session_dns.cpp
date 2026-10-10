#include "session_dns.hpp"

#include "file_descriptor.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab::session_dns {
namespace {

constexpr std::size_t max_configuration_size{64U * 1024U};
constexpr std::size_t max_nameservers{3};
constexpr std::size_t max_search_domains{6};
constexpr std::size_t max_search_line_size{256};

[[nodiscard]] FileDescriptor create_snapshot_file(
    const char* const name,
    const std::string& contents,
    int& error_number)
{
    FileDescriptor descriptor{memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (descriptor.get() == -1) {
        error_number = errno;
        return FileDescriptor{-1};
    }

    std::size_t written{};
    while (written < contents.size()) {
        const ssize_t count{write(
            descriptor.get(), contents.data() + written, contents.size() - written)};
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        error_number = count == 0 ? EIO : errno;
        return FileDescriptor{-1};
    }

    if (fchmod(descriptor.get(), S_IRUSR | S_IRGRP | S_IROTH) == -1) {
        error_number = errno;
        return FileDescriptor{-1};
    }
    constexpr int seals{F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE};
    if (fcntl(descriptor.get(), F_ADD_SEALS, seals) == -1) {
        error_number = errno;
        return FileDescriptor{-1};
    }
    return descriptor;
}

[[nodiscard]] std::string proc_descriptor_path(const int descriptor)
{
    return "/proc/self/fd/" + std::to_string(descriptor);
}

[[nodiscard]] bool bind_read_only_file(
    const std::string& source,
    const char* const target,
    int& error_number) noexcept
{
    if (mount(source.c_str(), target, nullptr, MS_BIND, nullptr) == -1) {
        error_number = errno;
        return false;
    }
    if (mount(nullptr, target, nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) == -1) {
        error_number = errno;
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> result;
    std::size_t position{};
    while (position < line.size()) {
        while (position < line.size()
               && std::isspace(static_cast<unsigned char>(line[position])) != 0) {
            ++position;
        }
        if (position == line.size() || line[position] == '#' || line[position] == ';') {
            break;
        }
        const std::size_t start{position};
        while (position < line.size()
               && std::isspace(static_cast<unsigned char>(line[position])) == 0
               && line[position] != '#' && line[position] != ';') {
            ++position;
        }
        result.push_back(line.substr(start, position - start));
        if (position < line.size() && (line[position] == '#' || line[position] == ';')) {
            break;
        }
    }
    return result;
}

[[nodiscard]] bool valid_domain(std::string_view value)
{
    if (value.empty() || value.size() > 253) {
        return false;
    }

    std::size_t label_size{};
    for (const char character : value) {
        if (character == '.') {
            if (label_size == 0 || label_size > 63) {
                return false;
            }
            label_size = 0;
            continue;
        }
        const unsigned char byte{static_cast<unsigned char>(character)};
        if ((std::isalnum(byte) == 0 && character != '-' && character != '_')
            || ++label_size > 63) {
            return false;
        }
    }
    return label_size != 0;
}

[[nodiscard]] bool valid_nameserver(const std::string_view text)
{
    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (text.empty() || text.size() >= buffer.size()) {
        return false;
    }
    std::copy(text.begin(), text.end(), buffer.begin());

    in_addr address{};
    if (inet_pton(AF_INET, buffer.data(), &address) != 1) {
        return false;
    }
    const std::uint32_t host_address{ntohl(address.s_addr)};
    const std::uint32_t first_octet{host_address >> 24U};
    const bool loopback{first_octet == 127U};
    const bool link_local{(host_address & 0xffff0000U) == 0xa9fe0000U};
    const bool multicast{first_octet >= 224U && first_octet <= 239U};
    const bool reserved{first_octet >= 240U};
    return host_address != 0U && host_address != 0xffffffffU && !loopback
        && !link_local && !multicast && !reserved && first_octet != 0U;
}

[[nodiscard]] bool valid_integer_option(
    const std::string_view token,
    const std::string_view prefix,
    const unsigned minimum,
    const unsigned maximum)
{
    if (!token.starts_with(prefix) || token.size() == prefix.size()) {
        return false;
    }
    unsigned value{};
    for (const char character : token.substr(prefix.size())) {
        if (character < '0' || character > '9') {
            return false;
        }
        const unsigned digit{static_cast<unsigned>(character - '0')};
        if (value > (std::numeric_limits<unsigned>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    return value >= minimum && value <= maximum;
}

[[nodiscard]] bool supported_option(const std::string_view token)
{
    if (valid_integer_option(token, "ndots:", 0U, 15U)
        || valid_integer_option(token, "timeout:", 1U, 30U)
        || valid_integer_option(token, "attempts:", 1U, 5U)) {
        return true;
    }
    constexpr std::array<std::string_view, 11> options{
        "rotate", "edns0", "no-edns0", "single-request",
        "single-request-reopen", "use-vc", "no-tld-query", "trust-ad",
        "no-trust-ad", "no-aaaa", "no-check-names"};
    for (const std::string_view option : options) {
        if (token == option) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] SnapshotResult failure(std::string message)
{
    return {.snapshot = std::nullopt, .error = std::move(message)};
}

[[nodiscard]] SnapshotResult parse_resolver(
    const std::string_view contents,
    std::string& output)
{
    if (contents.size() > max_configuration_size) {
        return failure("/etc/resolv.conf exceeds the 64 KiB snapshot limit.");
    }

    std::vector<std::string> nameservers;
    std::vector<std::string> search_domains;
    std::vector<std::string> options;
    bool has_search_directive{};
    std::size_t position{};
    while (position <= contents.size()) {
        const std::size_t end{contents.find('\n', position)};
        const std::string_view line{contents.substr(
            position,
            end == std::string_view::npos ? contents.size() - position : end - position)};
        const std::vector<std::string_view> tokens{words(line)};
        if (!tokens.empty()) {
            if (tokens[0] == "nameserver") {
                if (tokens.size() != 2 || !valid_nameserver(tokens[1])) {
                    return failure(
                        "Unsupported or invalid DNS nameserver; use a reachable IPv4 "
                        "resolver instead of a loopback, link-local, or IPv6 proxy.");
                }
                if (std::find(nameservers.begin(), nameservers.end(), tokens[1])
                    == nameservers.end()) {
                    nameservers.emplace_back(tokens[1]);
                }
                if (nameservers.size() > max_nameservers) {
                    return failure("More than three DNS nameservers are configured.");
                }
            } else if (tokens[0] == "search" || tokens[0] == "domain") {
                if (has_search_directive) {
                    return failure(
                        "Multiple DNS search/domain directives are unsupported.");
                }
                has_search_directive = true;
                const std::size_t first_domain{1};
                const std::size_t expected_count{
                    tokens[0] == "domain" ? 1U : tokens.size() - 1U};
                if (tokens.size() < 2
                    || (tokens[0] == "domain" && tokens.size() != 2)
                    || expected_count > max_search_domains) {
                    return failure("DNS search configuration is outside supported bounds.");
                }
                for (std::size_t index{first_domain}; index < tokens.size(); ++index) {
                    if (tokens[index].starts_with('~')) {
                        return failure(
                            "Split-DNS search routing is unsupported by a Session "
                            "resolver snapshot.");
                    }
                    if (!valid_domain(tokens[index])) {
                        return failure("DNS search configuration contains an invalid domain.");
                    }
                    search_domains.emplace_back(tokens[index]);
                }
            } else if (tokens[0] == "options") {
                for (std::size_t index{1}; index < tokens.size(); ++index) {
                    if (!supported_option(tokens[index])) {
                        return failure(
                            "Unsupported resolver option '" + std::string{tokens[index]}
                            + "'; remove it or use a supported IPv4 resolver configuration.");
                    }
                    if (std::find(options.begin(), options.end(), tokens[index])
                        == options.end()) {
                        options.emplace_back(tokens[index]);
                    }
                }
            } else {
                return failure(
                    "Unsupported resolver directive '" + std::string{tokens[0]}
                    + "'; only nameserver, search, domain, and options are supported.");
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1;
    }

    if (nameservers.empty()) {
        return failure("No supported IPv4 nameserver is configured in /etc/resolv.conf.");
    }
    std::string normalized;
    for (const std::string& nameserver : nameservers) {
        normalized += "nameserver " + nameserver + '\n';
    }
    if (!search_domains.empty()) {
        std::string search_line{"search"};
        for (const std::string& domain : search_domains) {
            search_line += ' ' + domain;
        }
        if (search_line.size() > max_search_line_size) {
            return failure("DNS search configuration exceeds the resolver line limit.");
        }
        normalized += search_line + '\n';
    }
    if (!options.empty()) {
        normalized += "options";
        for (const std::string& option : options) {
            normalized += ' ' + option;
        }
        normalized += '\n';
    }
    output = std::move(normalized);
    return {.snapshot = Snapshot{}, .error = {}};
}

[[nodiscard]] SnapshotResult parse_nsswitch(
    const std::string_view contents,
    std::string& output)
{
    if (contents.size() > max_configuration_size) {
        return failure("/etc/nsswitch.conf exceeds the 64 KiB snapshot limit.");
    }

    bool found_hosts{};
    std::size_t position{};
    while (position < contents.size()) {
        const std::size_t end{contents.find('\n', position)};
        const bool has_newline{end != std::string_view::npos};
        const std::string_view line{contents.substr(
            position,
            has_newline ? end - position : contents.size() - position)};
        const std::vector<std::string_view> tokens{words(line)};
        if (!tokens.empty() && tokens[0] == "include") {
            return failure(
                "NSS include directives are unsupported because they can change "
                "host lookup routing.");
        }
        if (!tokens.empty() && tokens[0] == "hosts:") {
            if (found_hosts) {
                return failure("Multiple hosts entries in /etc/nsswitch.conf are unsupported.");
            }
            found_hosts = true;
            if (std::find(tokens.begin() + 1, tokens.end(), "resolve") != tokens.end()) {
                return failure(
                    "The host uses systemd-resolved for host lookups; split-DNS or "
                    "proxy-only behavior is unsupported for a Session DNS snapshot.");
            }
            output += "hosts: files dns";
        } else {
            output.append(line);
        }
        if (has_newline) {
            output += '\n';
            position = end + 1;
        } else {
            position = contents.size();
        }
    }
    if (!found_hosts) {
        if (!output.empty() && output.back() != '\n') {
            output += '\n';
        }
        output += "hosts: files dns\n";
    }
    if (output.size() > max_configuration_size) {
        return failure("Private /etc/nsswitch.conf exceeds the 64 KiB snapshot limit.");
    }
    return {.snapshot = Snapshot{}, .error = {}};
}

[[nodiscard]] std::optional<std::string> read_root_configuration(
    const char* const path,
    const std::string_view display_path,
    const std::size_t maximum,
    std::string& error)
{
    struct stat path_status {};
    if (lstat(path, &path_status) == 0) {
        if (S_ISLNK(path_status.st_mode)) {
            error = std::string{display_path}
                + " is a symlink; generated resolver/NSS routing can hide "
                  "split-DNS or proxy-only behavior. Use a supported direct file.";
            return std::nullopt;
        }
    } else if (errno != ENOENT) {
        error = "Could not inspect " + std::string{display_path} + ": "
            + std::string{std::strerror(errno)} + '.';
        return std::nullopt;
    }
    FileDescriptor descriptor{
        open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (descriptor.get() == -1) {
        error = "Could not open " + std::string{display_path} + ": "
            + std::string{std::strerror(errno)} + '.';
        return std::nullopt;
    }
    struct stat status {};
    if (fstat(descriptor.get(), &status) == -1 || !S_ISREG(status.st_mode)
        || status.st_uid != 0 || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        error = std::string{display_path}
            + " must be a root-owned regular file not writable by group or others.";
        return std::nullopt;
    }

    std::string contents;
    std::array<char, 4096> buffer{};
    while (contents.size() <= maximum) {
        const std::size_t remaining{maximum + 1U - contents.size()};
        const std::size_t requested{std::min(remaining, buffer.size())};
        const ssize_t count{read(descriptor.get(), buffer.data(), requested)};
        if (count > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        error = "Could not read " + std::string{display_path} + ": "
            + std::string{std::strerror(errno)} + '.';
        return std::nullopt;
    }
    if (contents.size() > maximum) {
        error = std::string{display_path} + " exceeds the 64 KiB snapshot limit.";
        return std::nullopt;
    }
    return contents;
}

} // namespace

SnapshotResult parse_host_dns_snapshot(
    const std::string_view resolver_contents,
    const std::string_view nsswitch_contents)
{
    std::string normalized_resolver;
    const SnapshotResult resolver_result{
        parse_resolver(resolver_contents, normalized_resolver)};
    if (!resolver_result.snapshot.has_value()) {
        return resolver_result;
    }

    std::string normalized_nsswitch;
    const SnapshotResult nsswitch_result{
        parse_nsswitch(nsswitch_contents, normalized_nsswitch)};
    if (!nsswitch_result.snapshot.has_value()) {
        return nsswitch_result;
    }
    return {
        .snapshot = Snapshot{
            std::move(normalized_resolver), std::move(normalized_nsswitch)},
        .error = {},
    };
}

SnapshotResult read_host_dns_snapshot()
{
    std::string error;
    const std::optional<std::string> resolver{read_root_configuration(
        "/etc/resolv.conf", "/etc/resolv.conf", max_configuration_size, error)};
    if (!resolver.has_value()) {
        return failure(std::move(error));
    }
    const std::optional<std::string> nsswitch{read_root_configuration(
        "/etc/nsswitch.conf", "/etc/nsswitch.conf", max_configuration_size, error)};
    if (!nsswitch.has_value()) {
        return failure(std::move(error));
    }
    return parse_host_dns_snapshot(*resolver, *nsswitch);
}

PrivateDnsMount::PrivateDnsMount(
    FileDescriptor resolver_file,
    FileDescriptor nsswitch_file,
    std::string resolver_source,
    std::string nsswitch_source) noexcept
    : resolver_file_{std::move(resolver_file)}
    , nsswitch_file_{std::move(nsswitch_file)}
    , resolver_source_{std::move(resolver_source)}
    , nsswitch_source_{std::move(nsswitch_source)}
{
}

std::optional<PrivateDnsMount> PrivateDnsMount::prepare(
    const Snapshot& snapshot,
    int& error_number)
{
    error_number = 0;
    if (snapshot.resolver_contents.size() > max_configuration_size
        || snapshot.nsswitch_contents.size() > max_configuration_size) {
        error_number = EFBIG;
        return std::nullopt;
    }
    FileDescriptor resolver_file{
        create_snapshot_file("netlaglab-resolv.conf", snapshot.resolver_contents,
                             error_number)};
    if (resolver_file.get() == -1) {
        return std::nullopt;
    }
    FileDescriptor nsswitch_file{
        create_snapshot_file("netlaglab-nsswitch.conf", snapshot.nsswitch_contents,
                             error_number)};
    if (nsswitch_file.get() == -1) {
        return std::nullopt;
    }
    std::string resolver_source{proc_descriptor_path(resolver_file.get())};
    std::string nsswitch_source{proc_descriptor_path(nsswitch_file.get())};
    return PrivateDnsMount{
        std::move(resolver_file), std::move(nsswitch_file),
        std::move(resolver_source), std::move(nsswitch_source)};
}

bool PrivateDnsMount::install_in_child(int& error_number) const noexcept
{
    error_number = 0;
    if (unshare(CLONE_NEWNS) == -1) {
        error_number = errno;
        return false;
    }
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == -1) {
        error_number = errno;
        return false;
    }
    return bind_read_only_file(resolver_source_, "/etc/resolv.conf", error_number)
        && bind_read_only_file(nsswitch_source_, "/etc/nsswitch.conf", error_number);
}

} // namespace netlaglab::session_dns
