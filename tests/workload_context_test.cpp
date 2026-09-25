#include "workload_context.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace netlaglab {
namespace {

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t beginning{};
    while (beginning < text.size()) {
        const std::size_t ending{text.find('\n', beginning)};
        if (ending == std::string::npos) {
            break;
        }
        lines.push_back(text.substr(beginning, ending - beginning));
        beginning = ending + 1;
    }
    return lines;
}

TEST(WorkloadContextTest, StartBlockRoundTripPreservesBytesOrderAndDuplicates)
{
    const WorkloadContext original{
        .working_directory = "/tmp/a directory",
        .arguments = {"program", "line\nbreak", "tab\tvalue"},
        .environment = {"PATH=/first", "DUP=one", "DUP=two", "EMPTY="},
    };

    const std::optional<std::string> encoded{serialize_start_block(original)};

    ASSERT_TRUE(encoded.has_value());
    const std::optional<WorkloadContext> decoded{parse_start_block(split_lines(*encoded))};
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, original);
}

TEST(WorkloadContextTest, StartBlockRejectsMalformedHexAndNul)
{
    EXPECT_FALSE(parse_start_block({"START_BEGIN", "CWD 0", "ARG 70", "START_END"}));
    EXPECT_FALSE(parse_start_block({"START_BEGIN", "CWD 00", "ARG 70", "START_END"}));
}

TEST(WorkloadContextTest, StartBlockRejectsInvalidOrdering)
{
    EXPECT_FALSE(parse_start_block(
        {"START_BEGIN", "ARG 70", "CWD 2F", "START_END"}));
    EXPECT_FALSE(parse_start_block(
        {"START_BEGIN", "CWD 2F", "ENV 413D31", "START_END"}));
    EXPECT_FALSE(parse_start_block(
        {"START_BEGIN", "CWD 2F", "ARG 70", "CWD 2F", "START_END"}));
}

TEST(WorkloadContextTest, StartBlockRejectsOversizedValue)
{
    WorkloadContext context{
        .working_directory = "/tmp",
        .arguments = {"program", std::string(maximum_context_value_size + 1, 'x')},
        .environment = {},
    };

    EXPECT_FALSE(serialize_start_block(context).has_value());
}

} // namespace
} // namespace netlaglab
