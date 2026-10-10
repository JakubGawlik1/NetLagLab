#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace netlaglab::network_environment::detail {

enum class RecoveryBackend {
    nftables,
    ufw,
    firewalld,
};

enum class RecoveryPhase {
    intent,
    applied,
    removing,
};

struct RecoveryRecord {
    RecoveryBackend backend;
    RecoveryPhase phase;
    std::string token;
};

enum class RecoveryReadStatus {
    empty,
    valid,
    corrupt,
    failure,
};

struct RecoveryReadResult {
    RecoveryReadStatus status;
    std::optional<RecoveryRecord> record;
};

[[nodiscard]] RecoveryReadResult read_recovery_record(
    int directory_descriptor,
    uid_t expected_owner);
[[nodiscard]] bool write_recovery_record(
    int directory_descriptor,
    uid_t expected_owner,
    const RecoveryRecord& record);
[[nodiscard]] bool clear_recovery_record(
    int directory_descriptor,
    uid_t expected_owner);
[[nodiscard]] bool valid_recovery_token(std::string_view token);
[[nodiscard]] std::optional<std::string> create_recovery_token();
[[nodiscard]] int open_recovery_directory();

} // namespace netlaglab::network_environment::detail
