#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace netlaglab::network_environment::detail {

enum class RecoveryPhase : std::uint8_t {
    intent,
    applied,
    removing,
    removed,
};

struct RecoveryRecord {
    RecoveryPhase phase;
    std::string transaction_id;

    friend bool operator==(const RecoveryRecord&, const RecoveryRecord&) =
        default;
};

enum class JournalReadKind {
    absent,
    record,
    invalid,
    failure,
};

struct JournalReadResult {
    JournalReadKind kind;
    RecoveryRecord record{};
};

// The directory and expected owner are injectable so tests can use a private
// temporary directory. Production always uses /var/lib/netlaglab and uid 0.
class RecoveryJournalStore {
public:
    RecoveryJournalStore(std::string directory, std::uint32_t expected_owner);

    [[nodiscard]] JournalReadResult read() const;
    [[nodiscard]] bool write(RecoveryRecord record) const;
    [[nodiscard]] bool clear() const;

    [[nodiscard]] static RecoveryJournalStore production();

private:
    std::string directory_;
    std::uint32_t expected_owner_;
};

enum class LiveRuleState {
    absent,
    exact,
    mismatch,
    ambiguous,
    failure,
};

class PersistentFirewallBackend {
public:
    virtual ~PersistentFirewallBackend() = default;
    [[nodiscard]] virtual LiveRuleState inspect(std::string_view transaction_id) = 0;
    [[nodiscard]] virtual bool remove_exact(std::string_view transaction_id) = 0;
};

enum class RecoveryOutcome {
    complete,
    refused,
};

[[nodiscard]] RecoveryOutcome reconcile_persistent_firewall(
    const RecoveryJournalStore& journal,
    PersistentFirewallBackend& backend);

[[nodiscard]] LiveRuleState decode_ufw_route_status(
    std::string_view output,
    std::string_view transaction_id);
[[nodiscard]] std::optional<RecoveryRecord> make_recovery_intent();
[[nodiscard]] std::unique_ptr<PersistentFirewallBackend>
make_linux_persistent_firewall_backend();

[[nodiscard]] std::string_view recovery_phase_name(RecoveryPhase phase);

} // namespace netlaglab::network_environment::detail
