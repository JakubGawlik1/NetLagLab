#include "network_environment/recovery_journal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace netlaglab::network_environment::detail {
namespace {

constexpr std::string_view test_transaction_id{"00112233445566778899aabbccddeeff"};

[[nodiscard]] RecoveryRecord record(const RecoveryPhase phase)
{
    return {phase, std::string{test_transaction_id}};
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        std::array<char, 32> path{};
        constexpr std::string_view pattern{"/tmp/nll-recovery-XXXXXX"};
        std::copy(pattern.begin(), pattern.end(), path.begin());
        char* created{mkdtemp(path.data())};
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

class ScriptedFirewall final : public PersistentFirewallBackend {
public:
    [[nodiscard]] LiveRuleState inspect(const std::string_view transaction_id) override
    {
        ++inspect_count;
        observed_ids.emplace_back(transaction_id);
        return states.empty() ? state : take_state();
    }

    [[nodiscard]] bool remove_exact(const std::string_view transaction_id) override
    {
        ++remove_count;
        removed_id = transaction_id;
        if (remove_changes_state) {
            state = LiveRuleState::absent;
        }
        return remove_succeeds;
    }

    LiveRuleState state{LiveRuleState::absent};
    std::vector<LiveRuleState> states;
    std::size_t inspect_count{};
    std::size_t remove_count{};
    bool remove_succeeds{true};
    bool remove_changes_state{true};
    std::vector<std::string> observed_ids;
    std::string removed_id;

private:
    [[nodiscard]] LiveRuleState take_state()
    {
        const LiveRuleState result{states.front()};
        states.erase(states.begin());
        return result;
    }
};

[[nodiscard]] RecoveryJournalStore make_store(const TemporaryDirectory& directory)
{
    return {directory.path(), static_cast<std::uint32_t>(getuid())};
}

[[nodiscard]] std::string journal_path(const TemporaryDirectory& directory)
{
    return directory.path() + "/recovery.state";
}

TEST(NetworkRecoveryJournalTest, WritesEachDurableTransitionAndClearsRemovedRecord)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};

    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    EXPECT_EQ(journal.read().record.phase, RecoveryPhase::intent);
    ASSERT_TRUE(journal.write(record(RecoveryPhase::applied)));
    EXPECT_EQ(journal.read().record.phase, RecoveryPhase::applied);
    ASSERT_TRUE(journal.write(record(RecoveryPhase::removing)));
    EXPECT_EQ(journal.read().record.phase, RecoveryPhase::removing);
    ASSERT_TRUE(journal.write(record(RecoveryPhase::removed)));
    EXPECT_EQ(journal.read().record.phase, RecoveryPhase::removed);
    EXPECT_TRUE(journal.clear());
    EXPECT_EQ(journal.read().kind, JournalReadKind::absent);
}

TEST(NetworkRecoveryJournalTest, RejectsIllegalPhaseTransitions)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};

    EXPECT_FALSE(journal.write(record(RecoveryPhase::applied)));
    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    EXPECT_FALSE(journal.write(record(RecoveryPhase::intent)));
    ASSERT_TRUE(journal.write(record(RecoveryPhase::applied)));
    EXPECT_FALSE(journal.write(record(RecoveryPhase::applied)));
    EXPECT_FALSE(journal.write(
        {RecoveryPhase::removing, "ffeeddccbbaa99887766554433221100"}));
}

TEST(NetworkRecoveryJournalTest, ReconcilesEveryInterruptedRemovalPhase)
{
    constexpr std::array phases{
        RecoveryPhase::intent,
        RecoveryPhase::applied,
        RecoveryPhase::removing};
    for (const RecoveryPhase phase : phases) {
        TemporaryDirectory directory;
        const RecoveryJournalStore journal{make_store(directory)};
        ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
        if (phase == RecoveryPhase::applied
            || phase == RecoveryPhase::removing) {
            ASSERT_TRUE(journal.write(record(RecoveryPhase::applied)));
        }
        if (phase == RecoveryPhase::removing) {
            ASSERT_TRUE(journal.write(record(RecoveryPhase::removing)));
        }
        ScriptedFirewall backend;
        backend.state = LiveRuleState::exact;

        EXPECT_EQ(
            reconcile_persistent_firewall(journal, backend),
            RecoveryOutcome::complete);
        EXPECT_EQ(backend.remove_count, 1U);
        EXPECT_EQ(backend.removed_id, test_transaction_id);
        EXPECT_EQ(journal.read().kind, JournalReadKind::absent);
    }
}

TEST(NetworkRecoveryJournalTest, ClearsIntentWhenExactRuleWasNeverApplied)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};
    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    ScriptedFirewall backend;
    backend.state = LiveRuleState::absent;

    EXPECT_EQ(
        reconcile_persistent_firewall(journal, backend),
        RecoveryOutcome::complete);
    EXPECT_EQ(backend.remove_count, 0U);
    EXPECT_EQ(journal.read().kind, JournalReadKind::absent);
}

TEST(NetworkRecoveryJournalTest, RetryCompletesWhenRemoveReportedFailureAfterEffect)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};
    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    ScriptedFirewall backend;
    backend.state = LiveRuleState::exact;
    backend.remove_succeeds = false;

    EXPECT_EQ(
        reconcile_persistent_firewall(journal, backend),
        RecoveryOutcome::complete);
    EXPECT_EQ(backend.remove_count, 1U);
    EXPECT_EQ(journal.read().kind, JournalReadKind::absent);
}

TEST(NetworkRecoveryJournalTest, PreservesEvidenceForMismatchAmbiguityOrInspectionFailure)
{
    constexpr std::array unsafe_states{
        LiveRuleState::mismatch,
        LiveRuleState::ambiguous,
        LiveRuleState::failure};
    for (const LiveRuleState state : unsafe_states) {
        TemporaryDirectory directory;
        const RecoveryJournalStore journal{make_store(directory)};
        ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
        ScriptedFirewall backend;
        backend.state = state;

        EXPECT_EQ(
            reconcile_persistent_firewall(journal, backend),
            RecoveryOutcome::refused);
        EXPECT_EQ(backend.remove_count, 0U);
        EXPECT_EQ(journal.read().kind, JournalReadKind::record);
    }
}

TEST(NetworkRecoveryJournalTest, PreservesRemovingRecordWhenRuleStillExistsAfterDelete)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};
    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    ScriptedFirewall backend;
    backend.state = LiveRuleState::exact;
    backend.remove_changes_state = false;

    EXPECT_EQ(
        reconcile_persistent_firewall(journal, backend),
        RecoveryOutcome::refused);
    ASSERT_EQ(journal.read().kind, JournalReadKind::record);
    EXPECT_EQ(journal.read().record.phase, RecoveryPhase::removing);
}

TEST(NetworkRecoveryJournalTest, ClearsRemovingRecordWhenRuleIsAlreadyAbsent)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};
    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    ASSERT_TRUE(journal.write(record(RecoveryPhase::applied)));
    ASSERT_TRUE(journal.write(record(RecoveryPhase::removing)));
    ScriptedFirewall backend;
    backend.state = LiveRuleState::absent;

    EXPECT_EQ(
        reconcile_persistent_firewall(journal, backend),
        RecoveryOutcome::complete);
    EXPECT_EQ(backend.remove_count, 0U);
    EXPECT_EQ(journal.read().kind, JournalReadKind::absent);
}

TEST(NetworkRecoveryJournalTest, CorruptOrOversizedJournalIsRejectedWithoutDeletion)
{
    constexpr std::array<std::string_view, 2> corrupt_contents{
        "NETLAGLAB-RECOVERY 9\nphase=intent\n",
        std::string_view{"", 0},
    };
    for (const std::string_view contents : corrupt_contents) {
        TemporaryDirectory directory;
        {
            std::ofstream file{journal_path(directory), std::ios::binary};
            file << contents;
        }
        const RecoveryJournalStore journal{make_store(directory)};
        EXPECT_EQ(journal.read().kind, JournalReadKind::invalid);
        ScriptedFirewall backend;
        backend.state = LiveRuleState::exact;
        EXPECT_EQ(
            reconcile_persistent_firewall(journal, backend),
            RecoveryOutcome::refused);
        EXPECT_EQ(backend.remove_count, 0U);
    }

    TemporaryDirectory directory;
    {
        std::ofstream file{journal_path(directory), std::ios::binary};
        file << std::string(4097, 'x');
    }
    EXPECT_EQ(make_store(directory).read().kind, JournalReadKind::invalid);
}

TEST(NetworkRecoveryJournalTest, RejectsSymlinkJournalInsteadOfFollowingIt)
{
    TemporaryDirectory directory;
    const std::string target{directory.path() + "/target"};
    {
        std::ofstream file{target};
        file << "NETLAGLAB-RECOVERY 1\n";
    }
    ASSERT_EQ(symlink(target.c_str(), journal_path(directory).c_str()), 0);
    EXPECT_EQ(make_store(directory).read().kind, JournalReadKind::failure);
}

TEST(NetworkRecoveryJournalTest, RejectsUnexpectedJournalPermissions)
{
    TemporaryDirectory directory;
    const RecoveryJournalStore journal{make_store(directory)};
    ASSERT_TRUE(journal.write(record(RecoveryPhase::intent)));
    ASSERT_EQ(chmod(journal_path(directory).c_str(), 0644), 0);
    EXPECT_EQ(journal.read().kind, JournalReadKind::invalid);
}

TEST(NetworkRecoveryJournalTest, UfwStatusRequiresOneExactForwardRule)
{
    constexpr std::string_view one_exact_rule{
        "Status: active\n"
        "To                         Action      From\n"
        "--                         ------      ----\n"
        "[ 1] Anywhere               ALLOW FWD   10.200.0.2 on nll-host "
        "# netlaglab-00112233445566778899aabbccddeeff\n"};
    EXPECT_EQ(
        decode_ufw_route_status(one_exact_rule, test_transaction_id),
        LiveRuleState::exact);

    const std::string duplicate{std::string{one_exact_rule}
        + "[ 2] Anywhere               ALLOW FWD   10.200.0.2 on nll-host "
          "# netlaglab-00112233445566778899aabbccddeeff\n"};
    EXPECT_EQ(
        decode_ufw_route_status(duplicate, test_transaction_id),
        LiveRuleState::ambiguous);

    const std::string duplicate_marker{std::string{one_exact_rule}
        + "[ 2] 192.0.2.1               ALLOW FWD   198.51.100.2 on eth0 "
          "# netlaglab-00112233445566778899aabbccddeeff\n"};
    EXPECT_EQ(
        decode_ufw_route_status(duplicate_marker, test_transaction_id),
        LiveRuleState::ambiguous);

    constexpr std::string_view mismatched_rule{
        "Status: active\n"
        "[ 1] 192.0.2.1               ALLOW FWD   10.200.0.2 on nll-host "
        "# netlaglab-ffeeddccbbaa99887766554433221100\n"};
    EXPECT_EQ(
        decode_ufw_route_status(mismatched_rule, test_transaction_id),
        LiveRuleState::mismatch);

    EXPECT_EQ(
        decode_ufw_route_status("Status: inactive\n", test_transaction_id),
        LiveRuleState::failure);
}

TEST(NetworkRecoveryJournalTest, CreatesAUniqueTransactionIdentity)
{
    const auto first{make_recovery_intent()};
    const auto second{make_recovery_intent()};

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->phase, RecoveryPhase::intent);
    EXPECT_EQ(first->transaction_id.size(), 32U);
    EXPECT_NE(first->transaction_id, second->transaction_id);
}

} // namespace
} // namespace netlaglab::network_environment::detail
