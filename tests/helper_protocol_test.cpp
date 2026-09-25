#include "helper_protocol.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace netlaglab {
namespace {

TEST(HelperProtocolTest, FramesPartialAndCoalescedLifecycleEvents)
{
    SupervisorHelperConversation conversation;

    EXPECT_TRUE(conversation.receive_bytes("REA").empty());
    const std::vector<HelperConversationEvent> startup{
        conversation.receive_bytes("DY\nACTIVE 4321\nWORKLOAD_EXITED 9\n")};

    ASSERT_EQ(startup.size(), 3U);
    EXPECT_EQ(startup[0].kind, HelperConversationEventKind::ready);
    EXPECT_EQ(startup[1].kind, HelperConversationEventKind::activated);
    EXPECT_EQ(startup[1].value, 4321);
    EXPECT_EQ(startup[2].kind, HelperConversationEventKind::workload_exited);
    EXPECT_EQ(startup[2].value, 9);

    const std::vector<HelperConversationEvent> cleanup{
        conversation.receive_bytes("CLEANUP_OK\n")};
    ASSERT_EQ(cleanup.size(), 1U);
    EXPECT_EQ(cleanup.front().kind, HelperConversationEventKind::cleanup_succeeded);
    EXPECT_TRUE(conversation.peer_closed().empty());
}

TEST(HelperProtocolTest, RejectsImpossibleOrdering)
{
    SupervisorHelperConversation conversation;

    const std::vector<HelperConversationEvent> events{
        conversation.receive_bytes("READY\nWORKLOAD_EXITED 0\n")};

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.front().kind, HelperConversationEventKind::ready);
    EXPECT_EQ(events.back().kind, HelperConversationEventKind::conversation_lost);
}

TEST(HelperProtocolTest, TreatsEarlyEofAndOversizedFrameAsConversationLoss)
{
    SupervisorHelperConversation early_eof;
    ASSERT_EQ(early_eof.receive_bytes("READY\n").size(), 1U);
    const auto eof_events{early_eof.peer_closed()};
    ASSERT_EQ(eof_events.size(), 1U);
    EXPECT_EQ(eof_events.front().kind, HelperConversationEventKind::conversation_lost);

    SupervisorHelperConversation oversized;
    const auto oversized_events{
        oversized.receive_bytes(std::string(maximum_helper_message_size + 1, 'x'))};
    ASSERT_EQ(oversized_events.size(), 1U);
    EXPECT_EQ(
        oversized_events.front().kind,
        HelperConversationEventKind::conversation_lost);
}

TEST(HelperProtocolTest, RejectsTrailingPartialDataAfterFinalEvent)
{
    SupervisorHelperConversation conversation;
    const auto events{conversation.receive_bytes(
        "READY\nSTART_FAILED 127\nCLEANUP_OK\ntrailing")};
    ASSERT_EQ(events.size(), 3U);

    const auto eof_events{conversation.peer_closed()};
    ASSERT_EQ(eof_events.size(), 1U);
    EXPECT_EQ(eof_events.front().kind, HelperConversationEventKind::conversation_lost);
}

TEST(HelperProtocolTest, ParsesOnlyAllowlistedRuntimeCommands)
{
    EXPECT_EQ(
        parse_helper_runtime_command("STOP TERM"),
        HelperRuntimeCommand::stop_terminate);
    EXPECT_EQ(
        parse_helper_runtime_command("STOP KILL"),
        HelperRuntimeCommand::stop_kill);
    EXPECT_EQ(
        parse_helper_runtime_command("SHUTDOWN"),
        HelperRuntimeCommand::shutdown);
    EXPECT_FALSE(parse_helper_runtime_command("STOP 9").has_value());
}

TEST(HelperProtocolTest, HelperFramesPartialAndCoalescedStartBlock)
{
    const WorkloadContext expected{
        .working_directory = "/tmp",
        .arguments = {"/bin/true", "value"},
        .environment = {"PATH=/bin"},
    };
    const std::string block{*serialize_start_block(expected)};
    HelperStartConversation conversation;

    const auto partial{conversation.receive_bytes(block.substr(0, 7))};
    EXPECT_EQ(partial.state, StartBlockState::incomplete);
    const auto complete{conversation.receive_bytes(block.substr(7))};

    ASSERT_EQ(complete.state, StartBlockState::complete);
    ASSERT_TRUE(complete.context.has_value());
    EXPECT_EQ(*complete.context, expected);
}

TEST(HelperProtocolTest, HelperRejectsMalformedOrTrailingStartData)
{
    HelperStartConversation malformed;
    EXPECT_EQ(
        malformed.receive_bytes("NOT_START\n").state,
        StartBlockState::invalid);

    HelperStartConversation trailing;
    EXPECT_EQ(
        trailing.receive_bytes(
            "START_BEGIN\nCWD 2F\nARG 2F62696E2F74727565\n"
            "START_END\ntrailing").state,
        StartBlockState::invalid);
}

TEST(HelperProtocolTest, HelperFramesRuntimeCommandsAndRejectsUnknownFrames)
{
    HelperRuntimeConversation conversation;
    EXPECT_TRUE(conversation.receive_bytes("STOP ").commands.empty());
    const RuntimeCommandFeedResult commands{
        conversation.receive_bytes("TERM\nSTOP KILL\n")};
    ASSERT_TRUE(commands.valid);
    ASSERT_EQ(commands.commands.size(), 2U);
    EXPECT_EQ(commands.commands[0], HelperRuntimeCommand::stop_terminate);
    EXPECT_EQ(commands.commands[1], HelperRuntimeCommand::stop_kill);

    const RuntimeCommandFeedResult invalid{
        conversation.receive_bytes("UNKNOWN\n")};
    EXPECT_FALSE(invalid.valid);
}

} // namespace
} // namespace netlaglab
