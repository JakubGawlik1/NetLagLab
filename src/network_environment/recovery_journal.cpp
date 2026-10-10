#include "recovery_journal.hpp"

#include "file_descriptor.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <unistd.h>

namespace netlaglab::network_environment::detail {
namespace {

constexpr std::string_view journal_name{"recovery.state"};
constexpr std::size_t maximum_journal_size{4096};
std::atomic<unsigned long> temporary_sequence{};

[[nodiscard]] std::string serialize(const RecoveryRecord record)
{
    return "NETLAGLAB-RECOVERY 1\nphase="
        + std::string{recovery_phase_name(record.phase)} + "\nid="
        + record.transaction_id + "\n"
          "backend=ufw\n"
          "kind=route-allow\n"
          "in_interface=nll-host\n"
          "source=10.200.0.2/32\n"
          "destination=any\n";
}

[[nodiscard]] bool parse(
    const std::string_view contents,
    RecoveryRecord& record)
{
    if (contents.size() > maximum_journal_size) {
        return false;
    }
    constexpr std::string_view prefix{"NETLAGLAB-RECOVERY 1\nphase="};
    constexpr std::string_view id_prefix{"\nid="};
    constexpr std::string_view suffix{
        "\nbackend=ufw\nkind=route-allow\n"
        "in_interface=nll-host\nsource=10.200.0.2/32\n"
        "destination=any\n"};
    if (!contents.starts_with(prefix) || !contents.ends_with(suffix)) {
        return false;
    }
    const std::size_t phase_end{contents.find('\n', prefix.size())};
    if (phase_end == std::string_view::npos) {
        return false;
    }
    const std::string_view phase{contents.substr(
        prefix.size(), phase_end - prefix.size())};
    if (phase == "intent") {
        record.phase = RecoveryPhase::intent;
    } else if (phase == "applied") {
        record.phase = RecoveryPhase::applied;
    } else if (phase == "removing") {
        record.phase = RecoveryPhase::removing;
    } else if (phase == "removed") {
        record.phase = RecoveryPhase::removed;
    } else {
        return false;
    }
    if (!contents.substr(phase_end).starts_with(id_prefix)) {
        return false;
    }
    const std::size_t id_begin{phase_end + id_prefix.size()};
    const std::size_t suffix_begin{contents.size() - suffix.size()};
    if (suffix_begin < id_begin + 32 || suffix_begin - id_begin != 32
        || !contents.substr(suffix_begin).starts_with(suffix)) {
        return false;
    }
    record.transaction_id = std::string{contents.substr(id_begin, 32)};
    for (const char digit : record.transaction_id) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_all(
    const int descriptor,
    std::string& contents,
    const std::size_t expected_size)
{
    contents.resize(expected_size);
    std::size_t offset{};
    while (offset < expected_size) {
        const ssize_t count{
            read(descriptor, contents.data() + offset, expected_size - offset)};
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    char extra{};
    ssize_t count{};
    do {
        count = read(descriptor, &extra, 1);
    } while (count == -1 && errno == EINTR);
    return count == 0;
}

[[nodiscard]] bool write_all(const int descriptor, const std::string_view contents)
{
    std::size_t offset{};
    while (offset < contents.size()) {
        const ssize_t count{
            write(descriptor, contents.data() + offset, contents.size() - offset)};
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_directory(
    const int descriptor,
    const std::uint32_t expected_owner)
{
    struct stat status {};
    return fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode)
        && status.st_uid == expected_owner && (status.st_mode & 0022) == 0;
}

[[nodiscard]] int open_directory(
    const std::string& path,
    const std::uint32_t expected_owner,
    const bool create)
{
    int descriptor{open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor == -1 && errno == ENOENT && create) {
        const bool created{mkdir(path.c_str(), 0755) == 0};
        if (!created && errno != EEXIST) {
            return -1;
        }
        descriptor = open(
            path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (created && descriptor != -1 && fchmod(descriptor, 0755) == -1) {
            const int saved_errno{errno};
            close(descriptor);
            errno = saved_errno;
            return -1;
        }
    }
    if (descriptor != -1 && !valid_directory(descriptor, expected_owner)) {
        close(descriptor);
        errno = EPERM;
        return -1;
    }
    return descriptor;
}

[[nodiscard]] bool valid_file(
    const struct stat& status,
    const std::uint32_t expected_owner)
{
    return S_ISREG(status.st_mode) && status.st_uid == expected_owner
        && (status.st_mode & 0777) == 0600 && status.st_size >= 0
        && static_cast<std::uintmax_t>(status.st_size) <= maximum_journal_size;
}

[[nodiscard]] bool allowed_transition(
    const JournalReadResult current,
    const RecoveryRecord next)
{
    if (current.kind == JournalReadKind::absent) {
        return next.phase == RecoveryPhase::intent;
    }
    if (current.kind != JournalReadKind::record) {
        return false;
    }
    if (current.record.transaction_id != next.transaction_id) {
        return false;
    }
    switch (current.record.phase) {
    case RecoveryPhase::intent:
        return next.phase == RecoveryPhase::applied
            || next.phase == RecoveryPhase::removing
            || next.phase == RecoveryPhase::removed;
    case RecoveryPhase::applied:
        return next.phase == RecoveryPhase::removing
            || next.phase == RecoveryPhase::removed;
    case RecoveryPhase::removing:
        return next.phase == RecoveryPhase::removed;
    case RecoveryPhase::removed:
        return false;
    }
    return false;
}

} // namespace

RecoveryJournalStore::RecoveryJournalStore(
    std::string directory,
    const std::uint32_t expected_owner)
    : directory_{std::move(directory)}
    , expected_owner_{expected_owner}
{
}

RecoveryJournalStore RecoveryJournalStore::production()
{
    return {"/var/lib/netlaglab", 0};
}

std::string_view recovery_phase_name(const RecoveryPhase phase)
{
    switch (phase) {
    case RecoveryPhase::intent:
        return "intent";
    case RecoveryPhase::applied:
        return "applied";
    case RecoveryPhase::removing:
        return "removing";
    case RecoveryPhase::removed:
        return "removed";
    }
    return {};
}

std::optional<RecoveryRecord> make_recovery_intent()
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
    constexpr std::string_view hex{"0123456789abcdef"};
    std::string transaction_id;
    transaction_id.reserve(32);
    for (const unsigned char byte : random_bytes) {
        transaction_id.push_back(hex[byte >> 4U]);
        transaction_id.push_back(hex[byte & 0x0fU]);
    }
    return RecoveryRecord{RecoveryPhase::intent, std::move(transaction_id)};
}

JournalReadResult RecoveryJournalStore::read() const
{
    netlaglab::FileDescriptor directory{
        open_directory(directory_, expected_owner_, false)};
    if (directory.get() == -1) {
        return errno == ENOENT
            ? JournalReadResult{JournalReadKind::absent, {}}
            : JournalReadResult{JournalReadKind::failure, {}};
    }
    netlaglab::FileDescriptor file{openat(
        directory.get(),
        journal_name.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.get() == -1) {
        return errno == ENOENT
            ? JournalReadResult{JournalReadKind::absent, {}}
            : JournalReadResult{JournalReadKind::failure, {}};
    }
    struct stat status {};
    if (fstat(file.get(), &status) == -1) {
        return {JournalReadKind::failure, {}};
    }
    if (!valid_file(status, expected_owner_)) {
        return {JournalReadKind::invalid, {}};
    }
    std::string contents;
    if (!read_all(file.get(), contents, static_cast<std::size_t>(status.st_size))) {
        return {JournalReadKind::failure, {}};
    }
    RecoveryRecord record{};
    return parse(contents, record)
        ? JournalReadResult{JournalReadKind::record, record}
        : JournalReadResult{JournalReadKind::invalid, {}};
}

bool RecoveryJournalStore::write(const RecoveryRecord record) const
{
    if (record.transaction_id.size() != 32) {
        return false;
    }
    for (const char digit : record.transaction_id) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return false;
        }
    }
    const std::string contents{serialize(record)};
    if (contents.size() > maximum_journal_size
        || !allowed_transition(read(), record)) {
        return false;
    }
    netlaglab::FileDescriptor directory{
        open_directory(directory_, expected_owner_, true)};
    if (directory.get() == -1) {
        return false;
    }
    struct stat existing_status {};
    if (fstatat(
            directory.get(), journal_name.data(), &existing_status, AT_SYMLINK_NOFOLLOW)
        == 0) {
        if (!valid_file(existing_status, expected_owner_)) {
            return false;
        }
    } else if (errno != ENOENT) {
        return false;
    }

    const std::string temporary_name{
        ".recovery.state.tmp." + std::to_string(getpid()) + "."
        + std::to_string(temporary_sequence.fetch_add(1))};
    netlaglab::FileDescriptor temporary{openat(
        directory.get(),
        temporary_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600)};
    if (temporary.get() == -1) {
        return false;
    }
    const bool durable_file{fchmod(temporary.get(), 0600) == 0
        && fchown(temporary.get(), expected_owner_, static_cast<gid_t>(-1)) == 0
        && write_all(temporary.get(), contents) && fsync(temporary.get()) == 0};
    if (!durable_file
        || renameat(
               directory.get(),
               temporary_name.c_str(),
               directory.get(),
               journal_name.data())
            == -1) {
        (void)unlinkat(directory.get(), temporary_name.c_str(), 0);
        return false;
    }
    return fsync(directory.get()) == 0;
}

bool RecoveryJournalStore::clear() const
{
    const JournalReadResult current{read()};
    if (current.kind == JournalReadKind::absent) {
        return true;
    }
    if (current.kind != JournalReadKind::record
        || current.record.phase != RecoveryPhase::removed) {
        return false;
    }
    netlaglab::FileDescriptor directory{
        open_directory(directory_, expected_owner_, false)};
    if (directory.get() == -1
        || unlinkat(directory.get(), journal_name.data(), 0) == -1) {
        return false;
    }
    return fsync(directory.get()) == 0;
}

RecoveryOutcome reconcile_persistent_firewall(
    const RecoveryJournalStore& journal,
    PersistentFirewallBackend& backend)
{
    JournalReadResult current{journal.read()};
    if (current.kind == JournalReadKind::absent) {
        return RecoveryOutcome::complete;
    }
    if (current.kind != JournalReadKind::record) {
        return RecoveryOutcome::refused;
    }
    if (current.record.phase == RecoveryPhase::removed) {
        return journal.clear() ? RecoveryOutcome::complete
                               : RecoveryOutcome::refused;
    }

    LiveRuleState state{backend.inspect(current.record.transaction_id)};
    if (state == LiveRuleState::mismatch || state == LiveRuleState::ambiguous
        || state == LiveRuleState::failure) {
        return RecoveryOutcome::refused;
    }
    if (state == LiveRuleState::exact) {
        if (current.record.phase != RecoveryPhase::removing
            && !journal.write({
                RecoveryPhase::removing, current.record.transaction_id})) {
            return RecoveryOutcome::refused;
        }
        (void)backend.remove_exact(current.record.transaction_id);
        state = backend.inspect(current.record.transaction_id);
        if (state != LiveRuleState::absent) {
            return RecoveryOutcome::refused;
        }
    }
    if (!journal.write({RecoveryPhase::removed, current.record.transaction_id})) {
        return RecoveryOutcome::refused;
    }
    return journal.clear() ? RecoveryOutcome::complete
                           : RecoveryOutcome::refused;
}

} // namespace netlaglab::network_environment::detail
