#include "network_environment/recovery_journal.hpp"

#include "file_descriptor.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace netlaglab::network_environment::detail {
namespace {

class RecoveryJournalTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::array<char, 64> template_path{};
        constexpr std::string_view prefix{"/tmp/netlaglab-journal-XXXXXX"};
        std::copy(prefix.begin(), prefix.end(), template_path.begin());
        char* const path{mkdtemp(template_path.data())};
        ASSERT_NE(path, nullptr);
        directory_path_ = path;
        const int descriptor{open(
            directory_path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        ASSERT_NE(descriptor, -1);
        directory_ = FileDescriptor{descriptor};
    }

    void TearDown() override
    {
        directory_.reset();
        std::filesystem::remove_all(directory_path_);
    }

    std::string directory_path_;
    FileDescriptor directory_{-1};
};

TEST_F(RecoveryJournalTest, AtomicallyRoundTripsAndClearsTypedRecord)
{
    const RecoveryRecord record{
        RecoveryBackend::ufw,
        RecoveryPhase::applied,
        "0123456789abcdef0123456789abcdef",
    };
    ASSERT_TRUE(write_recovery_record(directory_.get(), geteuid(), record));

    const RecoveryReadResult read{read_recovery_record(directory_.get(), geteuid())};
    ASSERT_EQ(read.status, RecoveryReadStatus::valid);
    ASSERT_TRUE(read.record.has_value());
    EXPECT_EQ(read.record->backend, RecoveryBackend::ufw);
    EXPECT_EQ(read.record->phase, RecoveryPhase::applied);
    EXPECT_EQ(read.record->token, record.token);

    ASSERT_TRUE(clear_recovery_record(directory_.get(), geteuid()));
    EXPECT_EQ(
        read_recovery_record(directory_.get(), geteuid()).status,
        RecoveryReadStatus::empty);
}

TEST_F(RecoveryJournalTest, RefusesCorruptRecordWithoutDeletingIt)
{
    const int descriptor{openat(
        directory_.get(), "recovery.state", O_WRONLY | O_CREAT | O_EXCL, 0600)};
    ASSERT_NE(descriptor, -1);
    constexpr std::string_view corrupt{"version=2\n"};
    ASSERT_EQ(write(descriptor, corrupt.data(), corrupt.size()),
              static_cast<ssize_t>(corrupt.size()));
    ASSERT_EQ(close(descriptor), 0);

    EXPECT_EQ(
        read_recovery_record(directory_.get(), geteuid()).status,
        RecoveryReadStatus::corrupt);
    EXPECT_EQ(access((directory_path_ + "/recovery.state").c_str(), F_OK), 0);
}

TEST_F(RecoveryJournalTest, RefusesSymlinkAndInvalidOwnershipToken)
{
    ASSERT_EQ(
        symlinkat("missing", directory_.get(), "recovery.state"), 0);
    EXPECT_EQ(
        read_recovery_record(directory_.get(), geteuid()).status,
        RecoveryReadStatus::failure);
    EXPECT_FALSE(write_recovery_record(
        directory_.get(),
        geteuid(),
        {RecoveryBackend::ufw, RecoveryPhase::intent, "../../etc/passwd"}));
    EXPECT_FALSE(valid_recovery_token("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaz"));
}

} // namespace
} // namespace netlaglab::network_environment::detail
