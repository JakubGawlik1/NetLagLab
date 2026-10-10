#include "workload_resolver_view.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace netlaglab {
namespace {

constexpr std::size_t maximum_file_size{16U * 1024U};
constexpr std::size_t maximum_line_size{1024U};

[[nodiscard]] std::string_view trim(std::string_view value)
{
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> result;
    while (!line.empty()) {
        line = trim(line);
        if (line.empty()) {
            break;
        }
        const std::size_t end{line.find_first_of(" \t")};
        result.push_back(line.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        line.remove_prefix(end + 1);
    }
    return result;
}

[[nodiscard]] bool valid_domain(std::string_view domain)
{
    if (domain.empty() || domain.size() > 253U || domain.front() == '.'
        || domain.back() == '.') {
        return false;
    }
    std::size_t label_begin{};
    while (label_begin < domain.size()) {
        const std::size_t separator{domain.find('.', label_begin)};
        const std::size_t label_end{
            separator == std::string_view::npos ? domain.size() : separator};
        const std::string_view label{domain.substr(label_begin, label_end - label_begin)};
        if (label.empty() || label.size() > 63U || label.front() == '-'
            || label.back() == '-') {
            return false;
        }
        for (const unsigned char character : label) {
            if (!(std::isalnum(character) != 0 || character == '-')) {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        label_begin = separator + 1;
    }
    return true;
}

[[nodiscard]] bool valid_option(std::string_view option)
{
    if (option.empty() || option.size() > 64U) {
        return false;
    }
    return std::all_of(option.begin(), option.end(), [](const unsigned char value) {
        return std::isalnum(value) != 0 || value == '_' || value == '-'
               || value == '.' || value == ':';
    });
}

[[nodiscard]] bool routable_resolver_address(const in_addr address)
{
    const std::uint32_t value{ntohl(address.s_addr)};
    const std::uint8_t first{static_cast<std::uint8_t>(value >> 24U)};
    return first != 0U && first != 127U && first < 224U
           && value != 0xffffffffU;
}

[[nodiscard]] std::optional<std::string> read_trusted_file(
    const char* const path,
    std::string& failure)
{
    const int descriptor{open(path, O_RDONLY | O_CLOEXEC)};
    if (descriptor == -1) {
        failure = std::string{"cannot open "} + path + ": " + std::strerror(errno);
        return std::nullopt;
    }
    FileDescriptor file{descriptor};
    struct stat status {};
    if (fstat(file.get(), &status) == -1 || !S_ISREG(status.st_mode)
        || status.st_uid != 0 || (status.st_mode & 0022) != 0
        || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_size) > maximum_file_size) {
        failure = std::string{path}
                  + " must be a root-owned, non-writable regular file no larger than 16 KiB";
        return std::nullopt;
    }

    std::string content;
    content.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 2048> buffer{};
    while (true) {
        const ssize_t count{read(file.get(), buffer.data(), buffer.size())};
        if (count > 0) {
            if (content.size() + static_cast<std::size_t>(count) > maximum_file_size) {
                failure = std::string{path} + " grew beyond the 16 KiB limit";
                return std::nullopt;
            }
            content.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            return content;
        } else if (errno != EINTR) {
            failure = std::string{"cannot read "} + path + ": " + std::strerror(errno);
            return std::nullopt;
        }
    }
}

[[nodiscard]] FileDescriptor create_snapshot_file(
    const char* const name,
    const std::string& content)
{
    const int descriptor{static_cast<int>(syscall(
        SYS_memfd_create,
        name,
        static_cast<unsigned int>(MFD_CLOEXEC | MFD_ALLOW_SEALING)))};
    if (descriptor == -1) {
        return FileDescriptor{-1};
    }
    FileDescriptor file{descriptor};
    std::size_t written{};
    while (written < content.size()) {
        const ssize_t count{write(
            file.get(), content.data() + written, content.size() - written)};
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return FileDescriptor{-1};
        }
    }
    if (lseek(file.get(), 0, SEEK_SET) == -1
        || fcntl(file.get(), F_ADD_SEALS,
                 F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)
               == -1) {
        return FileDescriptor{-1};
    }
    return file;
}

} // namespace

std::optional<std::string> make_resolver_snapshot(
    const std::string_view source,
    std::string& failure)
{
    if (source.size() > maximum_file_size) {
        failure = "resolver configuration exceeds 16 KiB";
        return std::nullopt;
    }

    std::string snapshot;
    std::size_t nameservers{};
    bool search_seen{};
    std::size_t line_begin{};
    while (line_begin < source.size()) {
        const std::size_t line_end{source.find('\n', line_begin)};
        const std::string_view raw_line{source.substr(
            line_begin,
            line_end == std::string_view::npos ? source.size() - line_begin
                                               : line_end - line_begin)};
        if (raw_line.size() > maximum_line_size) {
            failure = "resolver configuration contains a line longer than 1 KiB";
            return std::nullopt;
        }
        std::string_view line{trim(raw_line)};
        const std::size_t comment{line.find_first_of("#;")};
        if (comment != std::string_view::npos) {
            line = trim(line.substr(0, comment));
        }
        const std::vector<std::string_view> tokens{words(line)};
        if (!tokens.empty()) {
            if (tokens.front() == "nameserver") {
                in_addr address{};
                if (tokens.size() != 2U
                    || inet_pton(AF_INET,
                                 std::string{tokens[1]}.c_str(), &address)
                           != 1
                    || !routable_resolver_address(address) || nameservers == 3U) {
                    failure = "resolver must list one to three reachable IPv4 nameservers; "
                              "host-local and unsupported addresses are refused";
                    return std::nullopt;
                }
                snapshot += "nameserver ";
                snapshot.append(tokens[1]);
                snapshot.push_back('\n');
                ++nameservers;
            } else if (tokens.front() == "search" || tokens.front() == "domain") {
                if (search_seen || tokens.size() < 2U || tokens.size() > 7U) {
                    failure = "resolver search configuration must contain at most six domains";
                    return std::nullopt;
                }
                std::size_t total_length{};
                for (std::size_t index{1}; index < tokens.size(); ++index) {
                    if (!valid_domain(tokens[index])) {
                        failure = "resolver search configuration contains an invalid domain";
                        return std::nullopt;
                    }
                    total_length += tokens[index].size() + 1U;
                }
                if (total_length > 256U) {
                    failure = "resolver search configuration exceeds 256 bytes";
                    return std::nullopt;
                }
                snapshot += "search";
                for (std::size_t index{1}; index < tokens.size(); ++index) {
                    snapshot.push_back(' ');
                    snapshot.append(tokens[index]);
                }
                snapshot.push_back('\n');
                search_seen = true;
            } else if (tokens.front() == "options") {
                if (tokens.size() < 2U || tokens.size() > 17U) {
                    failure = "resolver options must contain between one and sixteen values";
                    return std::nullopt;
                }
                snapshot += "options";
                for (std::size_t index{1}; index < tokens.size(); ++index) {
                    if (!valid_option(tokens[index])) {
                        failure = "resolver options contain unsupported characters";
                        return std::nullopt;
                    }
                    snapshot.push_back(' ');
                    snapshot.append(tokens[index]);
                }
                snapshot.push_back('\n');
            } else {
                failure = "resolver configuration uses an unsupported directive: ";
                failure.append(tokens.front());
                return std::nullopt;
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1U;
    }
    if (nameservers == 0U) {
        failure = "resolver configuration contains no usable IPv4 nameserver";
        return std::nullopt;
    }
    return snapshot;
}

std::optional<std::string> make_nsswitch_snapshot(
    const std::string_view source,
    std::string& failure)
{
    if (source.empty() || source.size() > maximum_file_size) {
        failure = "NSS configuration must be non-empty and no larger than 16 KiB";
        return std::nullopt;
    }
    std::string snapshot;
    bool hosts_seen{};
    std::size_t line_begin{};
    while (line_begin < source.size()) {
        const std::size_t line_end{source.find('\n', line_begin)};
        const std::string_view raw_line{source.substr(
            line_begin,
            line_end == std::string_view::npos ? source.size() - line_begin
                                               : line_end - line_begin)};
        if (raw_line.size() > maximum_line_size) {
            failure = "NSS configuration contains a line longer than 1 KiB";
            return std::nullopt;
        }
        std::string_view line{trim(raw_line)};
        const std::size_t comment{line.find('#')};
        line = comment == std::string_view::npos ? line : line.substr(0, comment);
        const std::vector<std::string_view> tokens{words(line)};
        if (!tokens.empty() && tokens.front().starts_with("hosts:")) {
            if (hosts_seen || tokens.front() != "hosts:") {
                failure = "NSS configuration has an invalid or duplicate hosts database";
                return std::nullopt;
            }
            hosts_seen = true;
            snapshot += "hosts: files dns";
            if (comment != std::string_view::npos) {
                snapshot.push_back(' ');
                snapshot.append(trim(raw_line.substr(raw_line.find('#'))));
            }
        } else {
            snapshot.append(raw_line);
        }
        if (line_end != std::string_view::npos) {
            snapshot.push_back('\n');
            line_begin = line_end + 1U;
        } else {
            break;
        }
    }
    if (!hosts_seen) {
        failure = "NSS configuration does not define a hosts database";
        return std::nullopt;
    }
    return snapshot;
}

ResolverViewReadResult read_host_resolver_view()
{
    std::string failure;
    const auto resolver_source{read_trusted_file("/etc/resolv.conf", failure)};
    if (!resolver_source.has_value()) {
        return {.configuration = std::nullopt, .failure = std::move(failure)};
    }
    const auto resolver_snapshot{make_resolver_snapshot(*resolver_source, failure)};
    if (!resolver_snapshot.has_value()) {
        return {.configuration = std::nullopt, .failure = std::move(failure)};
    }

    const auto nsswitch_source{read_trusted_file("/etc/nsswitch.conf", failure)};
    if (!nsswitch_source.has_value()) {
        return {.configuration = std::nullopt, .failure = std::move(failure)};
    }
    const auto nsswitch_snapshot{make_nsswitch_snapshot(*nsswitch_source, failure)};
    if (!nsswitch_snapshot.has_value()) {
        return {.configuration = std::nullopt, .failure = std::move(failure)};
    }
    return {
        .configuration = ResolverViewConfiguration{
            *resolver_snapshot,
            *nsswitch_snapshot,
        },
        .failure = {},
    };
}

WorkloadResolverView::WorkloadResolverView(
    FileDescriptor resolv_conf,
    FileDescriptor nsswitch_conf) noexcept
    : resolv_conf_{std::move(resolv_conf)}
    , nsswitch_conf_{std::move(nsswitch_conf)}
{
}

int WorkloadResolverView::resolv_conf_descriptor() const noexcept
{
    return resolv_conf_.get();
}

int WorkloadResolverView::nsswitch_conf_descriptor() const noexcept
{
    return nsswitch_conf_.get();
}

WorkloadResolverViewResult create_workload_resolver_view(
    const ResolverViewConfiguration& configuration)
{
    FileDescriptor resolver{create_snapshot_file(
        "netlaglab-resolv.conf", configuration.resolv_conf)};
    if (resolver.get() == -1) {
        return {.view = std::nullopt,
                .failure = "cannot create sealed resolver snapshot"};
    }
    FileDescriptor nsswitch{create_snapshot_file(
        "netlaglab-nsswitch.conf", configuration.nsswitch_conf)};
    if (nsswitch.get() == -1) {
        return {.view = std::nullopt,
                .failure = "cannot create sealed NSS snapshot"};
    }
    return {
        .view = WorkloadResolverView{std::move(resolver), std::move(nsswitch)},
        .failure = {},
    };
}

} // namespace netlaglab
