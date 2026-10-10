#include "recovery_journal.hpp"

#include "file_descriptor.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <string>
#include <utility>

namespace netlaglab::network_environment::detail {
namespace {

constexpr std::string_view record_name{"recovery.state"};
constexpr std::size_t maximum_record_size{256U};

[[nodiscard]] std::string_view backend_name(const RecoveryBackend backend)
{
    switch (backend) {
    case RecoveryBackend::nftables: return "nftables";
    case RecoveryBackend::ufw: return "ufw";
    case RecoveryBackend::firewalld: return "firewalld";
    }
    return {};
}

[[nodiscard]] std::string_view phase_name(const RecoveryPhase phase)
{
    switch (phase) {
    case RecoveryPhase::intent: return "intent";
    case RecoveryPhase::applied: return "applied";
    case RecoveryPhase::removing: return "removing";
    }
    return {};
}

[[nodiscard]] std::optional<RecoveryBackend> parse_backend(
    const std::string_view value)
{
    if (value == "nftables") return RecoveryBackend::nftables;
    if (value == "ufw") return RecoveryBackend::ufw;
    if (value == "firewalld") return RecoveryBackend::firewalld;
    return std::nullopt;
}

[[nodiscard]] std::optional<RecoveryPhase> parse_phase(
    const std::string_view value)
{
    if (value == "intent") return RecoveryPhase::intent;
    if (value == "applied") return RecoveryPhase::applied;
    if (value == "removing") return RecoveryPhase::removing;
    return std::nullopt;
}

[[nodiscard]] bool valid_directory(
    const int descriptor,
    const uid_t expected_owner)
{
    struct stat status {};
    return fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode)
           && status.st_uid == expected_owner
           && (status.st_mode & 0777) == 0700;
}

[[nodiscard]] std::optional<std::string> serialize(
    const RecoveryRecord& record)
{
    const std::string_view backend{backend_name(record.backend)};
    const std::string_view phase{phase_name(record.phase)};
    if (backend.empty() || phase.empty() || !valid_recovery_token(record.token)) {
        return std::nullopt;
    }
    std::string output{"version=1\nbackend="};
    output.append(backend);
    output += "\nphase=";
    output.append(phase);
    output += "\ntoken=";
    output += record.token;
    output.push_back('\n');
    return output;
}

[[nodiscard]] std::optional<RecoveryRecord> parse(
    const std::string_view contents)
{
    constexpr std::string_view version_prefix{"version="};
    constexpr std::string_view backend_prefix{"backend="};
    constexpr std::string_view phase_prefix{"phase="};
    constexpr std::string_view token_prefix{"token="};
    const std::size_t first_end{contents.find('\n')};
    if (first_end == std::string_view::npos
        || contents.substr(0, first_end) != "version=1") {
        return std::nullopt;
    }
    const std::size_t second_begin{first_end + 1U};
    const std::size_t second_end{contents.find('\n', second_begin)};
    if (second_end == std::string_view::npos
        || !contents.substr(second_begin, second_end - second_begin)
                .starts_with(backend_prefix)) {
        return std::nullopt;
    }
    const auto backend{parse_backend(contents.substr(
        second_begin + backend_prefix.size(),
        second_end - second_begin - backend_prefix.size()))};
    const std::size_t third_begin{second_end + 1U};
    const std::size_t third_end{contents.find('\n', third_begin)};
    if (!backend.has_value() || third_end == std::string_view::npos
        || !contents.substr(third_begin, third_end - third_begin)
                .starts_with(phase_prefix)) {
        return std::nullopt;
    }
    const auto phase{parse_phase(contents.substr(
        third_begin + phase_prefix.size(),
        third_end - third_begin - phase_prefix.size()))};
    const std::size_t fourth_begin{third_end + 1U};
    const std::size_t fourth_end{contents.find('\n', fourth_begin)};
    if (!phase.has_value() || fourth_end == std::string_view::npos
        || fourth_end + 1U != contents.size()
        || !contents.substr(fourth_begin, fourth_end - fourth_begin)
                .starts_with(token_prefix)) {
        return std::nullopt;
    }
    std::string token{contents.substr(
        fourth_begin + token_prefix.size(),
        fourth_end - fourth_begin - token_prefix.size())};
    if (!valid_recovery_token(token)) {
        return std::nullopt;
    }
    return RecoveryRecord{*backend, *phase, std::move(token)};
}

[[nodiscard]] bool write_all(const int descriptor, const std::string_view data)
{
    std::size_t written{};
    while (written < data.size()) {
        const ssize_t count{write(
            descriptor, data.data() + written, data.size() - written)};
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

bool valid_recovery_token(const std::string_view token)
{
    if (token.size() != 32U) {
        return false;
    }
    for (const char value : token) {
        if (!((value >= '0' && value <= '9')
              || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> create_recovery_token()
{
    std::array<unsigned char, 16> random_bytes{};
    std::size_t received{};
    while (received < random_bytes.size()) {
        const ssize_t count{getrandom(
            random_bytes.data() + received,
            random_bytes.size() - received,
            0)};
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return std::nullopt;
        }
    }
    constexpr char digits[]{"0123456789abcdef"};
    std::string token;
    token.reserve(32U);
    for (const unsigned char byte : random_bytes) {
        token.push_back(digits[byte >> 4U]);
        token.push_back(digits[byte & 0x0fU]);
    }
    return token;
}

int open_recovery_directory()
{
    const int var_directory{open(
        "/var/lib", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (var_directory == -1) {
        return -1;
    }
    FileDescriptor parent{var_directory};
    struct stat parent_status {};
    if (fstat(parent.get(), &parent_status) == -1
        || !S_ISDIR(parent_status.st_mode) || parent_status.st_uid != 0
        || (parent_status.st_mode & 0022) != 0) {
        errno = EPERM;
        return -1;
    }
    if (mkdirat(parent.get(), "netlaglab", 0700) == -1 && errno != EEXIST) {
        return -1;
    }
    const int directory{openat(
        parent.get(), "netlaglab", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (directory == -1) {
        return -1;
    }
    struct stat status {};
    if (fstat(directory, &status) == -1 || !S_ISDIR(status.st_mode)
        || status.st_uid != 0 || (status.st_mode & 0777) != 0700) {
        close(directory);
        errno = EPERM;
        return -1;
    }
    return directory;
}

RecoveryReadResult read_recovery_record(
    const int directory_descriptor,
    const uid_t expected_owner)
{
    if (!valid_directory(directory_descriptor, expected_owner)) {
        return {RecoveryReadStatus::failure, std::nullopt};
    }
    const int descriptor{openat(
        directory_descriptor,
        record_name.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor == -1) {
        return errno == ENOENT
            ? RecoveryReadResult{RecoveryReadStatus::empty, std::nullopt}
            : RecoveryReadResult{RecoveryReadStatus::failure, std::nullopt};
    }
    FileDescriptor file{descriptor};
    struct stat status {};
    if (fstat(file.get(), &status) == -1 || !S_ISREG(status.st_mode)
        || status.st_uid != expected_owner || (status.st_mode & 0777) != 0600
        || status.st_size < 0
        || static_cast<std::size_t>(status.st_size) > maximum_record_size) {
        return {RecoveryReadStatus::corrupt, std::nullopt};
    }
    std::array<char, maximum_record_size + 1U> buffer{};
    std::size_t received{};
    while (received < buffer.size()) {
        const ssize_t count{read(
            file.get(), buffer.data() + received, buffer.size() - received)};
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            return {RecoveryReadStatus::failure, std::nullopt};
        }
    }
    if (received > maximum_record_size) {
        return {RecoveryReadStatus::corrupt, std::nullopt};
    }
    const auto record{parse(std::string_view{buffer.data(), received})};
    return record.has_value()
        ? RecoveryReadResult{RecoveryReadStatus::valid, record}
        : RecoveryReadResult{RecoveryReadStatus::corrupt, std::nullopt};
}

bool write_recovery_record(
    const int directory_descriptor,
    const uid_t expected_owner,
    const RecoveryRecord& record)
{
    const auto content{serialize(record)};
    if (!content.has_value() || !valid_directory(directory_descriptor, expected_owner)) {
        return false;
    }
    std::string temporary_name;
    FileDescriptor file{-1};
    for (unsigned int attempt{}; attempt < 16U; ++attempt) {
        temporary_name = ".recovery.state.tmp." + std::to_string(getpid())
                         + "." + std::to_string(attempt);
        const int descriptor{openat(
            directory_descriptor,
            temporary_name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600)};
        if (descriptor != -1) {
            file.reset();
            file = FileDescriptor{descriptor};
            break;
        }
        if (errno != EEXIST) {
            return false;
        }
    }
    if (file.get() == -1) {
        return false;
    }
    if (fchmod(file.get(), 0600) == -1 || !write_all(file.get(), *content)
        || fsync(file.get()) == -1) {
        (void)unlinkat(directory_descriptor, temporary_name.c_str(), 0);
        return false;
    }
    file.reset();
    if (renameat(
            directory_descriptor,
            temporary_name.c_str(),
            directory_descriptor,
            record_name.data())
            == -1
        || fsync(directory_descriptor) == -1) {
        (void)unlinkat(directory_descriptor, temporary_name.c_str(), 0);
        return false;
    }
    return true;
}

bool clear_recovery_record(
    const int directory_descriptor,
    const uid_t expected_owner)
{
    if (!valid_directory(directory_descriptor, expected_owner)) {
        return false;
    }
    if (unlinkat(directory_descriptor, record_name.data(), 0) == -1
        && errno != ENOENT) {
        return false;
    }
    return fsync(directory_descriptor) == 0;
}

} // namespace netlaglab::network_environment::detail
