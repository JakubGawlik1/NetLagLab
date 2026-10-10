#include "helper_protocol.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace netlaglab {
namespace {

using namespace std::chrono_literals;

TEST(HelperProtocolTest, FramesPartialAndCoalescedLifecycleEvents)
{
    SupervisorHelperConversation conversation;

    EXPECT_TRUE(conversation.receive_bytes("REA").empty());
    const std::vector<HelperConversationEvent> startup{
        conversation.receive_bytes("DY\nACTIVE 4321\nWORKLOAD_EXITED 9\n")};

    ASSERT_EQ(startup.size(), 3U);
    EXPECT_NE(std::get_if<ReadyEvent>(&startup[0]), nullptr);
    ASSERT_NE(std::get_if<ActivatedEvent>(&startup[1]), nullptr);
    EXPECT_EQ(std::get<ActivatedEvent>(startup[1]).workload_pid, 4321);
    ASSERT_NE(std::get_if<WorkloadExitedEvent>(&startup[2]), nullptr);
    EXPECT_EQ(std::get<WorkloadExitedEvent>(startup[2]).result, 9);

    const std::vector<HelperConversationEvent> cleanup{
        conversation.receive_bytes("CLEANUP_OK\n")};
    ASSERT_EQ(cleanup.size(), 1U);
    EXPECT_NE(std::get_if<CleanupSucceededEvent>(&cleanup.front()), nullptr);
    EXPECT_TRUE(conversation.peer_closed().empty());
}

TEST(HelperProtocolTest, RejectsImpossibleOrdering)
{
    SupervisorHelperConversation conversation;

    const std::vector<HelperConversationEvent> events{
        conversation.receive_bytes("READY\nWORKLOAD_EXITED 0\n")};

    ASSERT_EQ(events.size(), 2U);
    EXPECT_NE(std::get_if<ReadyEvent>(&events.front()), nullptr);
    EXPECT_NE(std::get_if<ConversationLostEvent>(&events.back()), nullptr);
}

TEST(HelperProtocolTest, TreatsEarlyEofAndOversizedFrameAsConversationLoss)
{
    SupervisorHelperConversation early_eof;
    ASSERT_EQ(early_eof.receive_bytes("READY\n").size(), 1U);
    const auto eof_events{early_eof.peer_closed()};
    ASSERT_EQ(eof_events.size(), 1U);
    EXPECT_NE(std::get_if<ConversationLostEvent>(&eof_events.front()), nullptr);

    SupervisorHelperConversation oversized;
    const auto oversized_events{
        oversized.receive_bytes(std::string(maximum_helper_message_size + 1, 'x'))};
    ASSERT_EQ(oversized_events.size(), 1U);
    EXPECT_NE(
        std::get_if<ConversationLostEvent>(&oversized_events.front()), nullptr);
}

TEST(HelperProtocolTest, RejectsTrailingPartialDataAfterFinalEvent)
{
    SupervisorHelperConversation conversation;
    const auto events{conversation.receive_bytes(
        "READY\nSTART_FAILED 127\nCLEANUP_OK\ntrailing")};
    ASSERT_EQ(events.size(), 3U);

    const auto eof_events{conversation.peer_closed()};
    ASSERT_EQ(eof_events.size(), 1U);
    EXPECT_NE(std::get_if<ConversationLostEvent>(&eof_events.front()), nullptr);
}

TEST(HelperProtocolTest, ParsesOnlyAllowlistedRuntimeCommands)
{
    EXPECT_EQ(
        parse_helper_runtime_command("STOP TERM"),
        HelperRuntimeCommand{StopTerminateCommand{}});
    EXPECT_EQ(
        parse_helper_runtime_command("STOP KILL"),
        HelperRuntimeCommand{StopKillCommand{}});
    EXPECT_EQ(
        parse_helper_runtime_command("SHUTDOWN"),
        HelperRuntimeCommand{CompatibilityShutdownCommand{}});
    EXPECT_FALSE(parse_helper_runtime_command("STOP 9").has_value());
    EXPECT_EQ(helper_error_message("Safe failure."), "ERROR Safe failure.\n");
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
    EXPECT_EQ(commands.commands[0], HelperRuntimeCommand{StopTerminateCommand{}});
    EXPECT_EQ(commands.commands[1], HelperRuntimeCommand{StopKillCommand{}});

    const RuntimeCommandFeedResult invalid{
        conversation.receive_bytes("UNKNOWN\n")};
    EXPECT_FALSE(invalid.valid);
}

TEST(HelperProtocolTest, RoundTripsEveryProfileChangeThroughBothConversations)
{
    const std::vector<ProfileChange> changes{
        SetDelay{TrafficDirection::outbound, 0ms},
        SetDelay{TrafficDirection::inbound, 40ms},
        SetJitter{TrafficDirection::outbound, 7ms},
        SetJitter{TrafficDirection::inbound, 0ms},
        SetPacketLoss{TrafficDirection::outbound, 0.0},
        SetPacketLoss{TrafficDirection::inbound, 100.0},
        SetPacketLoss{TrafficDirection::inbound, 1.5},
        SetPacketLoss{TrafficDirection::outbound, 1e-10},
        SetBandwidth{TrafficDirection::outbound, 1},
        SetBandwidth{
            TrafficDirection::inbound,
            std::numeric_limits<std::uint64_t>::max()},
        ResetSetting{TrafficDirection::outbound, NetworkSetting::delay},
        ResetSetting{TrafficDirection::inbound, NetworkSetting::delay},
        ResetSetting{TrafficDirection::outbound, NetworkSetting::jitter},
        ResetSetting{TrafficDirection::inbound, NetworkSetting::jitter},
        ResetSetting{TrafficDirection::outbound, NetworkSetting::packet_loss},
        ResetSetting{TrafficDirection::inbound, NetworkSetting::packet_loss},
        ResetSetting{TrafficDirection::outbound, NetworkSetting::bandwidth},
        ResetSetting{TrafficDirection::inbound, NetworkSetting::bandwidth},
    };

    for (const ProfileChange& change : changes) {
        SupervisorHelperConversation supervisor;
        ASSERT_EQ(supervisor.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
        const auto request{supervisor.begin_profile_change(change)};
        ASSERT_TRUE(request.has_value());

        HelperRuntimeConversation helper;
        RuntimeCommandFeedResult decoded{true, {}, std::nullopt};
        for (const char byte : *request) {
            decoded = helper.receive_bytes(std::string_view{&byte, 1});
            ASSERT_TRUE(decoded.valid);
        }
        ASSERT_EQ(decoded.commands.size(), 1U);
        const auto* decoded_change{
            std::get_if<ProfileChange>(&decoded.commands.front())};
        ASSERT_NE(decoded_change, nullptr);
        EXPECT_EQ(*decoded_change, change);

        const auto response{
            helper.complete_profile_change(ProfileChangeCompletion::applied)};
        ASSERT_TRUE(response.has_value());
        const std::size_t split{response->size() / 2};
        EXPECT_TRUE(supervisor.receive_bytes(response->substr(0, split)).empty());
        const auto events{supervisor.receive_bytes(response->substr(split))};
        ASSERT_EQ(events.size(), 1U);
        const auto* result{std::get_if<ProfileChangeResultEvent>(&events.front())};
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->result, ProfileChangeResult::applied);
        EXPECT_EQ(result->change, change);
    }
}

TEST(HelperProtocolTest, RejectsNonCanonicalOrInvalidProfileCommandTokens)
{
    const std::vector<std::string> invalid_frames{
        "PROFILE_SET_DELAY OUTBOUND +1\n",
        "PROFILE_SET_DELAY OUTBOUND 01\n",
        "PROFILE_SET_DELAY OUTBOUND 9223372036854775808\n",
        "PROFILE_SET_DELAY  OUTBOUND 1\n",
        "PROFILE_SET_DELAY\tOUTBOUND\t1\n",
        "PROFILE_SET_JITTER INBOUND -1\n",
        "PROFILE_SET_BANDWIDTH OUTBOUND 0\n",
        "PROFILE_SET_BANDWIDTH OUTBOUND 01\n",
        "PROFILE_SET_BANDWIDTH INBOUND 18446744073709551616\n",
        "PROFILE_SET_LOSS OUTBOUND +1\n",
        "PROFILE_SET_LOSS OUTBOUND 1.0\n",
        "PROFILE_SET_LOSS OUTBOUND nan\n",
        "PROFILE_SET_LOSS OUTBOUND inf\n",
        "PROFILE_SET_LOSS OUTBOUND -0\n",
        "PROFILE_SET_LOSS INBOUND 101\n",
        "PROFILE_SET_LOSS INBOUND 1x\n",
        "PROFILE_RESET SIDEWAYS DELAY\n",
        "PROFILE_RESET OUTBOUND LATENCY\n",
        "PROFILE_RESET OUTBOUND DELAY extra\n",
        "PROFILE_RESET OUTBOUND\n",
    };

    for (const std::string& frame : invalid_frames) {
        HelperRuntimeConversation helper;
        const RuntimeCommandFeedResult result{helper.receive_bytes(frame)};
        EXPECT_FALSE(result.valid) << frame;
        EXPECT_EQ(result.response, "ERROR INVALID_RUNTIME_COMMAND\n") << frame;
        EXPECT_FALSE(helper.receive_bytes("STOP TERM\n").valid) << frame;
    }

    HelperRuntimeConversation oversized;
    const auto oversized_result{oversized.receive_bytes(
        std::string(maximum_helper_message_size + 1, '1'))};
    EXPECT_FALSE(oversized_result.valid);
    EXPECT_EQ(oversized_result.response, "ERROR INVALID_RUNTIME_COMMAND\n");
}

TEST(HelperProtocolTest, EnforcesPendingProfileOrderingAndCompletionOutcomes)
{
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    SupervisorHelperConversation inactive;
    EXPECT_FALSE(inactive.begin_profile_change(change).has_value());

    SupervisorHelperConversation unsolicited;
    ASSERT_EQ(unsolicited.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    const auto unsolicited_result{unsolicited.receive_bytes("PROFILE_OK\n")};
    ASSERT_EQ(unsolicited_result.size(), 1U);
    EXPECT_NE(
        std::get_if<ConversationLostEvent>(&unsolicited_result.front()), nullptr);
    EXPECT_FALSE(unsolicited.begin_profile_change(change).has_value());

    SupervisorHelperConversation supervisor;
    ASSERT_EQ(supervisor.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    const auto request{supervisor.begin_profile_change(change)};
    ASSERT_TRUE(request.has_value());
    EXPECT_FALSE(supervisor.begin_profile_change(change).has_value());

    HelperRuntimeConversation helper;
    ASSERT_TRUE(helper.receive_bytes(*request).valid);
    const RuntimeCommandFeedResult second{helper.receive_bytes(*request)};
    EXPECT_FALSE(second.valid);
    EXPECT_EQ(second.response, "ERROR INVALID_RUNTIME_COMMAND\n");

    SupervisorHelperConversation recovered;
    ASSERT_EQ(recovered.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    ASSERT_TRUE(recovered.begin_profile_change(change).has_value());
    const auto failed{recovered.receive_bytes("PROFILE_FAILED APPLY_FAILED\n")};
    ASSERT_EQ(failed.size(), 1U);
    const auto* result{std::get_if<ProfileChangeResultEvent>(&failed.front())};
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->result, ProfileChangeResult::restored_after_failure);
    EXPECT_EQ(result->change, change);
    EXPECT_TRUE(recovered.begin_profile_change(change).has_value());

    SupervisorHelperConversation unknown;
    ASSERT_EQ(unknown.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    ASSERT_TRUE(unknown.begin_profile_change(change).has_value());
    const auto terminal{unknown.receive_bytes("ERROR PROFILE_STATE_UNKNOWN\n")};
    ASSERT_EQ(terminal.size(), 1U);
    EXPECT_NE(std::get_if<ProfileStateUnknownEvent>(&terminal.front()), nullptr);
    EXPECT_FALSE(unknown.begin_profile_change(change).has_value());
}

TEST(HelperProtocolTest, KeepsStopAndStreamFramingAvailableWhileProfileIsPending)
{
    HelperRuntimeConversation helper;
    EXPECT_TRUE(
        helper.receive_bytes("PROFILE_SET_DELAY OUTBOUND ").commands.empty());
    const auto commands{helper.receive_bytes("10\nSTOP TERM\nSHUT")};
    ASSERT_TRUE(commands.valid);
    ASSERT_EQ(commands.commands.size(), 2U);
    EXPECT_NE(std::get_if<ProfileChange>(&commands.commands[0]), nullptr);
    EXPECT_NE(std::get_if<StopTerminateCommand>(&commands.commands[1]), nullptr);

    const auto suffix{helper.receive_bytes("DOWN\n")};
    ASSERT_TRUE(suffix.valid);
    ASSERT_EQ(suffix.commands.size(), 1U);
    EXPECT_NE(std::get_if<CompatibilityShutdownCommand>(&suffix.commands[0]), nullptr);

    const auto response{helper.complete_profile_change(
        ProfileChangeCompletion::restored_after_failure)};
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, "PROFILE_FAILED APPLY_FAILED\n");
}

TEST(HelperProtocolTest, MapsEveryHelperCompletionOutcome)
{
    const std::string request{"PROFILE_SET_BANDWIDTH OUTBOUND 1000\n"};

    HelperRuntimeConversation applied;
    ASSERT_TRUE(applied.receive_bytes(request).valid);
    EXPECT_EQ(
        applied.complete_profile_change(ProfileChangeCompletion::applied),
        "PROFILE_OK\n");

    HelperRuntimeConversation restored;
    ASSERT_TRUE(restored.receive_bytes(request).valid);
    EXPECT_EQ(
        restored.complete_profile_change(
            ProfileChangeCompletion::restored_after_failure),
        "PROFILE_FAILED APPLY_FAILED\n");

    HelperRuntimeConversation unknown;
    ASSERT_TRUE(unknown.receive_bytes(request).valid);
    EXPECT_EQ(
        unknown.complete_profile_change(ProfileChangeCompletion::state_unknown),
        "ERROR PROFILE_STATE_UNKNOWN\n");
    const auto stops{unknown.receive_bytes("STOP TERM\nSTOP KILL\n")};
    ASSERT_TRUE(stops.valid);
    ASSERT_EQ(stops.commands.size(), 2U);
    EXPECT_NE(std::get_if<StopTerminateCommand>(&stops.commands[0]), nullptr);
    EXPECT_NE(std::get_if<StopKillCommand>(&stops.commands[1]), nullptr);
    const auto refused_change{
        unknown.receive_bytes("PROFILE_SET_DELAY OUTBOUND 20\n")};
    EXPECT_TRUE(refused_change.valid);
    EXPECT_TRUE(refused_change.commands.empty());
    const auto later_stop{unknown.receive_bytes("STOP KILL\n")};
    EXPECT_TRUE(later_stop.valid);
    ASSERT_EQ(later_stop.commands.size(), 1U);
    EXPECT_NE(std::get_if<StopKillCommand>(&later_stop.commands[0]), nullptr);
    EXPECT_FALSE(
        unknown.complete_profile_change(ProfileChangeCompletion::applied).has_value());
}

class InMemoryProfileChangeAdapter final : public ProfileChangeAdapter {
public:
    ProfileChangeCompletion apply(const ProfileChange& change) override
    {
        const ProfileChangeApplication application{
            apply_profile_change(profile, change)};
        const auto* applied{std::get_if<NetworkProfile>(&application)};
        if (applied == nullptr) {
            return ProfileChangeCompletion::state_unknown;
        }
        profile = *applied;
        return ProfileChangeCompletion::applied;
    }

    NetworkProfile profile;
};

TEST(HelperProtocolTest, ExercisesSuccessfulPathThroughInjectedAdapter)
{
    HelperRuntimeConversation helper;
    const auto request{
        helper.receive_bytes("PROFILE_SET_DELAY OUTBOUND 10\n")};
    ASSERT_TRUE(request.valid);
    ASSERT_EQ(request.commands.size(), 1U);
    const auto* change{std::get_if<ProfileChange>(&request.commands[0])};
    ASSERT_NE(change, nullptr);
    InMemoryProfileChangeAdapter adapter;

    EXPECT_EQ(
        complete_profile_change(helper, *change, adapter),
        "PROFILE_OK\n");
    EXPECT_EQ(adapter.profile.outbound.delay, 10ms);
}

TEST(HelperProtocolTest, StopRejectsLaterProfileChangesButAllowsEscalation)
{
    HelperRuntimeConversation helper;
    const auto stop{helper.receive_bytes("STOP TERM\n")};
    ASSERT_TRUE(stop.valid);
    ASSERT_EQ(stop.commands.size(), 1U);

    const auto repeated{helper.receive_bytes("STOP KILL\n")};
    ASSERT_TRUE(repeated.valid);
    ASSERT_EQ(repeated.commands.size(), 1U);

    const auto change{
        helper.receive_bytes("PROFILE_SET_DELAY OUTBOUND 10\n")};
    EXPECT_FALSE(change.valid);
    EXPECT_EQ(change.response, "ERROR INVALID_RUNTIME_COMMAND\n");
}

TEST(HelperProtocolTest, UnknownProfileStatePreservesLifecycleEventFraming)
{
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    SupervisorHelperConversation supervisor;
    ASSERT_EQ(supervisor.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    ASSERT_TRUE(supervisor.begin_profile_change(change).has_value());

    const auto events{supervisor.receive_bytes(
        "ERROR PROFILE_STATE_UNKNOWN\n"
        "WORKLOAD_EXITED 7\n"
        "CLEANUP_FAILED\n")};

    ASSERT_EQ(events.size(), 3U);
    EXPECT_NE(std::get_if<ProfileStateUnknownEvent>(&events[0]), nullptr);
    EXPECT_NE(std::get_if<WorkloadExitedEvent>(&events[1]), nullptr);
    EXPECT_NE(std::get_if<CleanupFailedEvent>(&events[2]), nullptr);
}

TEST(HelperProtocolTest, ConnectivityFailurePreservesWorkloadAndCleanupEvents)
{
    EXPECT_EQ(
        helper_conversation_event_message(ConnectivityFailedEvent{}),
        "CONNECTIVITY_FAILED\n");
    SupervisorHelperConversation supervisor;
    ASSERT_EQ(supervisor.receive_bytes("READY\nACTIVE 123\n").size(), 2U);

    const auto failure{supervisor.receive_bytes("CONNECTIVITY_FAILED\n")};

    ASSERT_EQ(failure.size(), 1U);
    EXPECT_NE(std::get_if<ConnectivityFailedEvent>(&failure.front()), nullptr);
    EXPECT_FALSE(supervisor.begin_profile_change(
        ProfileChange{SetDelay{TrafficDirection::outbound, 10ms}}).has_value());
    const auto completed{supervisor.receive_bytes(
        "WORKLOAD_SIGNALED 15\nCLEANUP_OK\n")};
    ASSERT_EQ(completed.size(), 2U);
    EXPECT_NE(std::get_if<WorkloadSignaledEvent>(&completed[0]), nullptr);
    EXPECT_NE(std::get_if<CleanupSucceededEvent>(&completed[1]), nullptr);

    HelperRuntimeConversation helper;
    helper.connectivity_failed();
    const RuntimeCommandFeedResult commands{helper.receive_bytes(
        "PROFILE_SET_DELAY OUTBOUND 10\nSTOP TERM\n")};
    ASSERT_TRUE(commands.valid);
    ASSERT_EQ(commands.commands.size(), 1U);
    EXPECT_NE(std::get_if<StopTerminateCommand>(&commands.commands.front()), nullptr);
}

TEST(HelperProtocolTest, TerminalWorkloadEventCancelsPendingProfileState)
{
    const ProfileChange change{SetPacketLoss{TrafficDirection::inbound, 2.5}};

    SupervisorHelperConversation supervisor;
    ASSERT_EQ(supervisor.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    ASSERT_TRUE(supervisor.begin_profile_change(change).has_value());
    const auto terminal{supervisor.receive_bytes("WORKLOAD_EXITED 0\n")};
    ASSERT_EQ(terminal.size(), 1U);
    EXPECT_NE(std::get_if<WorkloadExitedEvent>(&terminal.front()), nullptr);
    const auto late_result{supervisor.receive_bytes("PROFILE_OK\n")};
    ASSERT_EQ(late_result.size(), 1U);
    EXPECT_NE(std::get_if<ConversationLostEvent>(&late_result.front()), nullptr);

    HelperRuntimeConversation helper;
    ASSERT_TRUE(helper.receive_bytes(
        "PROFILE_SET_LOSS INBOUND 2.5\n").valid);
    helper.workload_finished();
    EXPECT_FALSE(
        helper.complete_profile_change(ProfileChangeCompletion::applied).has_value());
}

} // namespace
} // namespace netlaglab
