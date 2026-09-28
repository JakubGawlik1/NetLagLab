#include "process_status.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

namespace netlaglab {
namespace {

TEST(ProcessStatusTest, ParsesParentAndStartTimeAfterComplexProcessName)
{
    const auto status{parse_process_status(
        "42 (name with ) parenthesis) S 17 5 6 7 8 9 10 11 12 13 14 15 16 17 "
        "18 19 20 21 987654")};

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->parent_pid, 17);
    EXPECT_EQ(status->start_time_ticks, 987654U);
}

TEST(ProcessStatusTest, ReadsCurrentProcessIdentity)
{
    const auto status{read_process_status(getpid())};

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->parent_pid, getppid());
    EXPECT_GT(status->start_time_ticks, 0U);
}

TEST(ProcessStatusTest, RejectsTruncatedInput)
{
    EXPECT_FALSE(parse_process_status("42 (name) S 17").has_value());
}

} // namespace
} // namespace netlaglab
