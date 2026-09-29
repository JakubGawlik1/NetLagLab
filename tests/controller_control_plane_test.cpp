#include "controller_control_plane.hpp"
#include "control_plane_action_executor.hpp"
#include "file_descriptor.hpp"
#include "socket_io.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <variant>
#include <vector>

namespace netlaglab {
namespace {

using namespace std::chrono_literals;

template <typename Command>
bool parsed_command_is(const std::string_view line)
{
    const ControllerParseResult result{parse_controller_command(line)};
    const auto* command{std::get_if<ControllerCommand>(&result)};
    return command != nullptr && std::holds_alternative<Command>(*command);
}

template <typename Change>
const Change* parsed_profile_change(const std::string_view line)
{
    static ControllerParseResult result;
    result = parse_controller_command(line);
    const auto* command{std::get_if<ControllerCommand>(&result)};
    if (command == nullptr) {
        return nullptr;
    }
    const auto* profile_change{std::get_if<ProfileChange>(command)};
    return profile_change == nullptr ? nullptr
                                     : std::get_if<Change>(profile_change);
}

void expect_parse_error(
    const std::string_view line,
    const ControllerParseError expected)
{
    const ControllerParseResult result{parse_controller_command(line)};
    const auto* error{std::get_if<ControllerParseError>(&result)};
    ASSERT_NE(error, nullptr) << line;
    EXPECT_EQ(*error, expected) << line;
}

TEST(ControllerCommandParserTest, ParsesExistingCommandFamilies)
{
    EXPECT_TRUE(parsed_command_is<HelpControllerCommand>("help"));
    EXPECT_TRUE(parsed_command_is<StatusControllerCommand>("status"));
    EXPECT_TRUE(parsed_command_is<StopControllerCommand>("stop"));
    EXPECT_TRUE(parsed_command_is<DetachControllerCommand>("detach"));
}

TEST(ControllerCommandParserTest, IgnoresEmptyAndWhitespaceOnlyLines)
{
    EXPECT_TRUE(std::holds_alternative<IgnoredControllerCommand>(
        parse_controller_command("")));
    EXPECT_TRUE(std::holds_alternative<IgnoredControllerCommand>(
        parse_controller_command(" \t  \t")));
}

TEST(ControllerCommandParserTest, AcceptsFlexibleOuterAndTokenWhitespace)
{
    EXPECT_TRUE(parsed_command_is<HelpControllerCommand>("\t help \t"));

    const auto* change{parsed_profile_change<SetDelay>(
        " \t set  \t outbound   delay \t 15 \t ms \t ")};
    ASSERT_NE(change, nullptr);
    EXPECT_EQ(change->value, std::chrono::milliseconds{15});
}

TEST(ControllerCommandParserTest, ParsesAndNormalizesEverySetCommand)
{
    const auto* delay{
        parsed_profile_change<SetDelay>("set outbound delay 2 s")};
    ASSERT_NE(delay, nullptr);
    EXPECT_EQ(delay->direction, TrafficDirection::outbound);
    EXPECT_EQ(delay->value, std::chrono::milliseconds{2000});

    const auto* jitter{
        parsed_profile_change<SetJitter>("set\tinbound\tjitter\t250ms")};
    ASSERT_NE(jitter, nullptr);
    EXPECT_EQ(jitter->direction, TrafficDirection::inbound);
    EXPECT_EQ(jitter->value, std::chrono::milliseconds{250});

    const auto* loss{
        parsed_profile_change<SetPacketLoss>("set outbound loss 1.5%")};
    ASSERT_NE(loss, nullptr);
    EXPECT_EQ(loss->direction, TrafficDirection::outbound);
    EXPECT_DOUBLE_EQ(loss->percent, 1.5);

    const auto* separated_loss{
        parsed_profile_change<SetPacketLoss>("set inbound loss 1.5 %")};
    ASSERT_NE(separated_loss, nullptr);
    EXPECT_DOUBLE_EQ(separated_loss->percent, 1.5);

    const auto* bandwidth{
        parsed_profile_change<SetBandwidth>("set inbound bandwidth 5 mbps")};
    ASSERT_NE(bandwidth, nullptr);
    EXPECT_EQ(bandwidth->direction, TrafficDirection::inbound);
    EXPECT_EQ(bandwidth->kbps, 5000U);
}

TEST(ControllerCommandParserTest, AppliesDocumentedDefaultUnits)
{
    const auto* delay{parsed_profile_change<SetDelay>("set inbound delay 7")};
    ASSERT_NE(delay, nullptr);
    EXPECT_EQ(delay->value, std::chrono::milliseconds{7});

    const auto* jitter{
        parsed_profile_change<SetJitter>("set outbound jitter 9")};
    ASSERT_NE(jitter, nullptr);
    EXPECT_EQ(jitter->value, std::chrono::milliseconds{9});

    const auto* loss{
        parsed_profile_change<SetPacketLoss>("set inbound loss 2.25")};
    ASSERT_NE(loss, nullptr);
    EXPECT_DOUBLE_EQ(loss->percent, 2.25);

    const auto* bandwidth{
        parsed_profile_change<SetBandwidth>("set outbound bandwidth 750")};
    ASSERT_NE(bandwidth, nullptr);
    EXPECT_EQ(bandwidth->kbps, 750U);
}

TEST(ControllerCommandParserTest, RepresentsResetAsOneDirectionAndSetting)
{
    const auto* delay{
        parsed_profile_change<ResetSetting>("reset outbound delay")};
    ASSERT_NE(delay, nullptr);
    EXPECT_EQ(delay->direction, TrafficDirection::outbound);
    EXPECT_EQ(delay->setting, NetworkSetting::delay);

    const auto* jitter{
        parsed_profile_change<ResetSetting>("reset inbound jitter")};
    ASSERT_NE(jitter, nullptr);
    EXPECT_EQ(jitter->direction, TrafficDirection::inbound);
    EXPECT_EQ(jitter->setting, NetworkSetting::jitter);

    const auto* loss{
        parsed_profile_change<ResetSetting>("reset outbound loss")};
    ASSERT_NE(loss, nullptr);
    EXPECT_EQ(loss->setting, NetworkSetting::packet_loss);

    const auto* bandwidth{
        parsed_profile_change<ResetSetting>("reset inbound bandwidth")};
    ASSERT_NE(bandwidth, nullptr);
    EXPECT_EQ(bandwidth->setting, NetworkSetting::bandwidth);
}

TEST(ControllerCommandParserTest, AcceptsEquivalentAttachedAndSeparatedUnits)
{
    const auto* seconds{parsed_profile_change<SetDelay>(
        "set outbound delay 1s")};
    ASSERT_NE(seconds, nullptr);
    EXPECT_EQ(seconds->value, std::chrono::milliseconds{1000});

    const auto* milliseconds{parsed_profile_change<SetDelay>(
        "set outbound delay 1000 ms")};
    ASSERT_NE(milliseconds, nullptr);
    EXPECT_EQ(milliseconds->value, std::chrono::milliseconds{1000});

    const auto* megabits{parsed_profile_change<SetBandwidth>(
        "set inbound bandwidth 1mbps")};
    ASSERT_NE(megabits, nullptr);
    EXPECT_EQ(megabits->kbps, 1000U);

    const auto* kilobits{parsed_profile_change<SetBandwidth>(
        "set inbound bandwidth 1000 kbps")};
    ASSERT_NE(kilobits, nullptr);
    EXPECT_EQ(kilobits->kbps, 1000U);
}

TEST(ControllerCommandParserTest, ReportsEveryStructuredErrorCategory)
{
    expect_parse_error(
        std::string(maximum_controller_command_size + 1, 'x'),
        ControllerParseError::command_too_long);
    expect_parse_error("help\n", ControllerParseError::invalid_character);
    expect_parse_error("unknown", ControllerParseError::unknown_command);
    expect_parse_error("status now", ControllerParseError::wrong_argument_count);
    expect_parse_error(
        "set sideways delay 1", ControllerParseError::invalid_direction);
    expect_parse_error(
        "reset outbound latency", ControllerParseError::invalid_setting);
    expect_parse_error(
        "set outbound delay 1.5ms", ControllerParseError::invalid_number);
    expect_parse_error(
        "set outbound delay 1mss", ControllerParseError::invalid_unit);
    expect_parse_error(
        "set outbound bandwidth 0", ControllerParseError::value_out_of_range);
    expect_parse_error(
        "set outbound bandwidth 18446744073709551616",
        ControllerParseError::value_overflow);

    const std::string underflow{"set inbound loss 0."
        + std::string(400, '0') + "1"};
    expect_parse_error(underflow, ControllerParseError::value_underflow);
}

TEST(ControllerCommandParserTest, EnforcesRawByteLimitBeforeOtherChecks)
{
    const std::string accepted{"help"
        + std::string(maximum_controller_command_size - 4, ' ')};
    EXPECT_TRUE(parsed_command_is<HelpControllerCommand>(accepted));

    std::string oversized(maximum_controller_command_size + 1, 'x');
    oversized[0] = '\n';
    expect_parse_error(oversized, ControllerParseError::command_too_long);
}

TEST(ControllerCommandParserTest, RejectsControlAndNonAsciiBytes)
{
    expect_parse_error(
        std::string{"help\0", 5}, ControllerParseError::invalid_character);
    expect_parse_error("help\r", ControllerParseError::invalid_character);
    expect_parse_error(
        std::string{"help"} + static_cast<char>(0x80),
        ControllerParseError::invalid_character);
}

TEST(ControllerCommandParserTest, RejectsUnsupportedNumericSyntax)
{
    for (const std::string_view value : {
             "+1", "-1", "1.5", "1,5", "1_000", ".5", "1.", "nan",
             "inf"}) {
        expect_parse_error(
            std::string{"set outbound delay "} + std::string{value},
            ControllerParseError::invalid_number);
    }
    for (const std::string_view value : {
             "+1", "-1", "1,5", "1_0", ".5", "1.", "nan", "inf"}) {
        expect_parse_error(
            std::string{"set outbound loss "} + std::string{value},
            ControllerParseError::invalid_number);
    }
    expect_parse_error(
        "set outbound delay 1e3", ControllerParseError::invalid_unit);
    expect_parse_error(
        "set outbound loss 1e3", ControllerParseError::invalid_unit);
}

TEST(ControllerCommandParserTest, DistinguishesMalformedNumberFromInvalidUnit)
{
    expect_parse_error(
        "set outbound delay abcms", ControllerParseError::invalid_number);
    expect_parse_error(
        "set outbound delay 1mss", ControllerParseError::invalid_unit);
    expect_parse_error(
        "set outbound delay 1ms ms", ControllerParseError::invalid_number);
    expect_parse_error(
        "set outbound loss 1wat", ControllerParseError::invalid_unit);
}

TEST(ControllerCommandParserTest, EnforcesDomainAndTargetTypeBounds)
{
    const auto* zero_loss{
        parsed_profile_change<SetPacketLoss>("set outbound loss 000.000%")};
    ASSERT_NE(zero_loss, nullptr);
    EXPECT_DOUBLE_EQ(zero_loss->percent, 0.0);

    const auto* complete_loss{
        parsed_profile_change<SetPacketLoss>("set inbound loss 00100.000")};
    ASSERT_NE(complete_loss, nullptr);
    EXPECT_DOUBLE_EQ(complete_loss->percent, 100.0);

    expect_parse_error(
        "set outbound loss 100.0001",
        ControllerParseError::value_out_of_range);
    expect_parse_error(
        "set outbound loss 101", ControllerParseError::value_out_of_range);

    const auto maximum_delay{
        std::numeric_limits<std::chrono::milliseconds::rep>::max()};
    const auto* accepted{parsed_profile_change<SetDelay>(
        "set inbound delay " + std::to_string(maximum_delay))};
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->value.count(), maximum_delay);

    const std::uint64_t first_overflowing_seconds{
        static_cast<std::uint64_t>(maximum_delay) / 1000U + 1U};
    expect_parse_error(
        "set inbound delay " + std::to_string(first_overflowing_seconds) + "s",
        ControllerParseError::value_overflow);

    const std::uint64_t first_overflowing_megabits{
        std::numeric_limits<std::uint64_t>::max() / 1000U + 1U};
    expect_parse_error(
        "set inbound bandwidth " + std::to_string(first_overflowing_megabits)
            + "mbps",
        ControllerParseError::value_overflow);
}

TEST(ControllerCommandParserTest, AppliesDocumentedErrorPrecedence)
{
    expect_parse_error("wat extra", ControllerParseError::unknown_command);
    expect_parse_error(
        "set sideways nope bad", ControllerParseError::invalid_direction);
    expect_parse_error(
        "set outbound nope bad", ControllerParseError::invalid_setting);
    expect_parse_error(
        "set outbound delay 1.5wat", ControllerParseError::invalid_number);
    expect_parse_error(
        "set outbound bandwidth 0wat", ControllerParseError::invalid_unit);
}

TEST(ControllerCommandParserTest, AllowsJitterWithoutBaseDelayState)
{
    const auto* jitter{
        parsed_profile_change<SetJitter>("set outbound jitter 25ms")};
    ASSERT_NE(jitter, nullptr);
    EXPECT_EQ(jitter->value, std::chrono::milliseconds{25});
}

ControllerControlPlane make_control_plane()
{
    static char program[]{"/bin/true"};
    static char argument[]{"hello"};
    static char* arguments[]{program, argument, nullptr};
    return ControllerControlPlane{1234, arguments};
}

const SendControllerTextAction* sent_text(
    const std::vector<ControlPlaneAction>& actions,
    const std::size_t index)
{
    return index < actions.size()
        ? std::get_if<SendControllerTextAction>(&actions[index]) : nullptr;
}

struct SocketPair {
    FileDescriptor first;
    FileDescriptor second;
};

SocketPair make_socket_pair()
{
    int descriptors[2]{};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors), 0);
    return {FileDescriptor{descriptors[0]}, FileDescriptor{descriptors[1]}};
}

TEST(ControllerControlPlaneTest, PreservesAttachmentHelpStatusAndDetachBehavior)
{
    ControllerControlPlane control_plane{make_control_plane()};

    const auto attached{control_plane.handle(ControllerAttachedEvent{})};
    ASSERT_EQ(attached.size(), 1U);
    ASSERT_NE(sent_text(attached, 0), nullptr);
    EXPECT_EQ(sent_text(attached, 0)->text, "ATTACHED\n");

    const auto commands{control_plane.handle(
        ControllerBytesReceivedEvent{"help\nstatus\ndetach\nignored\n"})};
    ASSERT_EQ(commands.size(), 4U);
    EXPECT_NE(sent_text(commands, 0)->text.find("HELP_BEGIN\n"), std::string::npos);
    EXPECT_NE(
        sent_text(commands, 1)->text.find(
            "STATUS_BEGIN\nstate: running\npid: 1234\n"),
        std::string::npos);
    EXPECT_NE(
        sent_text(commands, 1)->text.find(
            "program: /bin/true\nargument[0]: hello\n"),
        std::string::npos);
    EXPECT_EQ(sent_text(commands, 2)->text, "DETACHED\n");
    EXPECT_NE(std::get_if<DisconnectControllerAction>(&commands[3]), nullptr);
}

TEST(ControllerControlPlaneTest, FramesBytesAndHandlesParserErrorsImmediately)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});

    EXPECT_TRUE(control_plane.handle(ControllerBytesReceivedEvent{"he"}).empty());
    const auto actions{control_plane.handle(
        ControllerBytesReceivedEvent{"lp\n\nwat\n"})};

    ASSERT_EQ(actions.size(), 2U);
    EXPECT_NE(sent_text(actions, 0)->text.find("HELP_BEGIN\n"), std::string::npos);
    EXPECT_EQ(sent_text(actions, 1)->text, "ERROR Unknown command.\n");
}

TEST(ControllerControlPlaneTest, AppliesConfirmedChangeBeforeQueuedStatus)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};

    const auto request{control_plane.handle(ControllerBytesReceivedEvent{
        "set outbound delay 10ms\nstatus\n"})};
    ASSERT_EQ(request.size(), 1U);
    const auto* dispatch{std::get_if<DispatchProfileChangeAction>(&request[0])};
    ASSERT_NE(dispatch, nullptr);
    EXPECT_EQ(dispatch->change, change);

    const auto completed{control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied, change})};
    ASSERT_EQ(completed.size(), 2U);
    EXPECT_EQ(sent_text(completed, 0)->text, "PROFILE_CHANGED\n");
    EXPECT_NE(sent_text(completed, 1)->text.find("delay: 10 ms\n"), std::string::npos);
    EXPECT_NE(sent_text(completed, 1)->text.find("shaping: applied\n"), std::string::npos);
}

TEST(ControllerControlPlaneTest, ReversibleFailurePreservesConfirmedProfile)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetPacketLoss{TrafficDirection::inbound, 2.5}};
    (void)control_plane.handle(
        ControllerBytesReceivedEvent{"set inbound loss 2.5%\nstatus\n"});

    const auto completed{control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::restored_after_failure, change})};

    ASSERT_EQ(completed.size(), 2U);
    EXPECT_EQ(
        sent_text(completed, 0)->text,
        "ERROR Profile change could not be applied; previous profile remains active.\n");
    EXPECT_NE(sent_text(completed, 1)->text.find("packet loss: 0%\n"), std::string::npos);
    EXPECT_NE(
        sent_text(completed, 1)->text.find("shaping: not applied\n"),
        std::string::npos);
}

TEST(ControllerControlPlaneTest, BoundsQueuedCommandsExcludingInflightOperation)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    std::string bytes{"set outbound delay 10ms\n"};
    for (int index{}; index < 32; ++index) {
        bytes += "status\n";
    }
    bytes += "bad\n\nstatus\n";

    const auto actions{control_plane.handle(ControllerBytesReceivedEvent{bytes})};

    ASSERT_EQ(actions.size(), 4U);
    EXPECT_EQ(sent_text(actions, 0)->text, "ERROR Unknown command.\n");
    EXPECT_EQ(sent_text(actions, 1)->text, "ERROR Too many queued commands.\n");
    EXPECT_NE(std::get_if<DisconnectControllerAction>(&actions[2]), nullptr);
    EXPECT_NE(std::get_if<DispatchProfileChangeAction>(&actions[3]), nullptr);
}

TEST(ControllerControlPlaneTest, StopPreemptsQueueAndRejectsLaterProfileChanges)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    (void)control_plane.handle(ControllerBytesReceivedEvent{
        "set outbound delay 10ms\nstatus\n"});

    const auto stopped{control_plane.handle(
        ControllerBytesReceivedEvent{"stop\nset inbound loss 1%\nhelp\nstop\n"})};

    ASSERT_EQ(stopped.size(), 5U);
    EXPECT_NE(std::get_if<RequestLifecycleStopAction>(&stopped[0]), nullptr);
    EXPECT_EQ(sent_text(stopped, 1)->text, "STOPPING\n");
    EXPECT_EQ(sent_text(stopped, 2)->text, "ERROR Session is stopping.\n");
    EXPECT_NE(sent_text(stopped, 3)->text.find("HELP_BEGIN\n"), std::string::npos);
    EXPECT_EQ(sent_text(stopped, 4)->text, "STOPPING\n");
}

TEST(ControllerControlPlaneTest, DisconnectKeepsInflightChangeWithoutStaleReply)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetBandwidth{TrafficDirection::inbound, 5000}};
    (void)control_plane.handle(
        ControllerBytesReceivedEvent{"set inbound bandwidth 5mbps\n"});
    (void)control_plane.handle(ControllerDisconnectedEvent{});

    const auto replacement{control_plane.handle(ControllerAttachedEvent{})};
    ASSERT_EQ(replacement.size(), 1U);
    (void)control_plane.handle(ControllerBytesReceivedEvent{"status\n"});
    const auto completed{control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied, change})};

    ASSERT_EQ(completed.size(), 1U);
    EXPECT_NE(sent_text(completed, 0)->text.find("5000 kbps\n"), std::string::npos);
}

TEST(ControllerControlPlaneTest, WriteFailureCancelsOnlyUndispatchedChange)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    const auto batch{control_plane.handle(ControllerBytesReceivedEvent{
        "bad\nset outbound delay 10ms\n"})};
    ASSERT_EQ(batch.size(), 2U);
    EXPECT_NE(sent_text(batch, 0), nullptr);
    EXPECT_NE(std::get_if<DispatchProfileChangeAction>(&batch[1]), nullptr);

    EXPECT_TRUE(control_plane.handle(ControllerWriteFailedEvent{}).empty());
    (void)control_plane.handle(ControllerAttachedEvent{});
    const auto status{control_plane.handle(ControllerBytesReceivedEvent{"status\n"})};
    ASSERT_EQ(status.size(), 1U);
    EXPECT_NE(sent_text(status, 0)->text.find("delay: 0 ms\n"), std::string::npos);

    const auto impossible{control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied, change})};
    ASSERT_EQ(impossible.size(), 1U);
    EXPECT_NE(
        std::get_if<FailSessionForUnknownProfileStateAction>(&impossible[0]),
        nullptr);
}

TEST(ControllerControlPlaneTest, TerminalEventSuppressesOperationReplies)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    (void)control_plane.handle(
        ControllerBytesReceivedEvent{"set outbound delay 10ms\nhelp\n"});
    EXPECT_TRUE(control_plane.handle(WorkloadTerminalEvent{}).empty());
    EXPECT_TRUE(control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied, change}).empty());
}

TEST(ControllerControlPlaneTest, HelperResultCanPrecedeTerminalEvent)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    (void)control_plane.handle(
        ControllerBytesReceivedEvent{"set outbound delay 10ms\n"});

    const auto result{control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied, change})};
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(sent_text(result, 0)->text, "PROFILE_CHANGED\n");
    EXPECT_TRUE(control_plane.handle(WorkloadTerminalEvent{}).empty());
}

TEST(ControllerControlPlaneTest, CoalescedHelperFramesRetainProtocolOrder)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    (void)control_plane.handle(
        ControllerBytesReceivedEvent{"set outbound delay 10ms\n"});
    SupervisorHelperConversation conversation;
    ASSERT_EQ(conversation.receive_bytes("READY\nACTIVE 123\n").size(), 2U);
    ASSERT_TRUE(conversation.begin_profile_change(change).has_value());

    const auto events{conversation.receive_bytes(
        "PROFILE_FAILED APPLY_FAILED\nWORKLOAD_EXITED 0\n")};

    ASSERT_EQ(events.size(), 2U);
    const auto* profile_result{
        std::get_if<ProfileChangeResultEvent>(&events[0])};
    ASSERT_NE(profile_result, nullptr);
    const auto actions{control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::restored_after_failure,
        profile_result->change})};
    ASSERT_EQ(actions.size(), 1U);
    EXPECT_NE(sent_text(actions, 0), nullptr);
    EXPECT_NE(std::get_if<WorkloadExitedEvent>(&events[1]), nullptr);
    EXPECT_TRUE(control_plane.handle(WorkloadTerminalEvent{}).empty());
}

TEST(ControllerControlPlaneTest, WriteFailureAfterDispatchPreservesSessionOperation)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};
    (void)control_plane.handle(
        ControllerBytesReceivedEvent{"set outbound delay 10ms\n"});
    const auto parser_error{
        control_plane.handle(ControllerBytesReceivedEvent{"bad\n"})};
    ASSERT_EQ(parser_error.size(), 1U);
    EXPECT_NE(sent_text(parser_error, 0), nullptr);

    EXPECT_TRUE(control_plane.handle(ControllerWriteFailedEvent{}).empty());
    (void)control_plane.handle(ControllerAttachedEvent{});
    EXPECT_TRUE(control_plane.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied, change}).empty());
    const auto status{control_plane.handle(ControllerBytesReceivedEvent{"status\n"})};
    ASSERT_EQ(status.size(), 1U);
    EXPECT_NE(sent_text(status, 0)->text.find("delay: 10 ms\n"), std::string::npos);
}

TEST(ControllerControlPlaneTest, WriteFailureBeforeDisconnectAffectsOnlyController)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    const auto detach{
        control_plane.handle(ControllerBytesReceivedEvent{"detach\n"})};
    ASSERT_EQ(detach.size(), 2U);
    EXPECT_EQ(sent_text(detach, 0)->text, "DETACHED\n");
    EXPECT_NE(std::get_if<DisconnectControllerAction>(&detach[1]), nullptr);

    EXPECT_TRUE(control_plane.handle(ControllerWriteFailedEvent{}).empty());
    const auto replacement{control_plane.handle(ControllerAttachedEvent{})};
    ASSERT_EQ(replacement.size(), 1U);
    EXPECT_EQ(sent_text(replacement, 0)->text, "ATTACHED\n");
}

TEST(ControllerControlPlaneTest, TerminalInterruptMakesPublicStateStopping)
{
    ControllerControlPlane control_plane{make_control_plane()};
    (void)control_plane.handle(ControllerAttachedEvent{});
    EXPECT_TRUE(control_plane.handle(SessionStoppingEvent{}).empty());
    EXPECT_TRUE(control_plane.handle(SessionStoppingEvent{}).empty());

    const auto actions{control_plane.handle(ControllerBytesReceivedEvent{
        "status\nset outbound delay 10ms\n"})};
    ASSERT_EQ(actions.size(), 2U);
    EXPECT_NE(
        sent_text(actions, 0)->text.find("STATUS_BEGIN\nstate: stopping\n"),
        std::string::npos);
    EXPECT_EQ(sent_text(actions, 1)->text, "ERROR Session is stopping.\n");
}

TEST(ControllerControlPlaneTest, UnknownOrMismatchedProfileStateTerminatesOperations)
{
    ControllerControlPlane unknown{make_control_plane()};
    (void)unknown.handle(ControllerAttachedEvent{});
    const auto unknown_actions{unknown.handle(HelperProfileStateUnknownEvent{})};
    ASSERT_EQ(unknown_actions.size(), 1U);
    EXPECT_NE(
        std::get_if<FailSessionForUnknownProfileStateAction>(&unknown_actions[0]),
        nullptr);

    ControllerControlPlane mismatch{make_control_plane()};
    (void)mismatch.handle(ControllerAttachedEvent{});
    (void)mismatch.handle(
        ControllerBytesReceivedEvent{"set outbound delay 10ms\n"});
    const auto mismatch_actions{mismatch.handle(HelperProfileChangeResultEvent{
        ControlPlaneProfileChangeResult::applied,
        ProfileChange{SetDelay{TrafficDirection::outbound, 11ms}}})};
    ASSERT_EQ(mismatch_actions.size(), 1U);
    EXPECT_NE(
        std::get_if<FailSessionForUnknownProfileStateAction>(&mismatch_actions[0]),
        nullptr);
}

TEST(ControlPlaneActionExecutorTest, ExecutesOrderedIoAndFeedsWriteFailureBack)
{
    SocketPair controller_sockets{make_socket_pair()};
    SocketPair helper_sockets{make_socket_pair()};
    std::optional<FileDescriptor> controller{
        std::move(controller_sockets.first)};
    ControllerControlPlane control_plane{make_control_plane()};
    SupervisorHelperConversation helper_conversation;
    ASSERT_EQ(
        helper_conversation.receive_bytes("READY\nACTIVE 123\n").size(),
        2U);
    const ProfileChange change{SetDelay{TrafficDirection::outbound, 10ms}};

    const auto events{execute_control_plane_actions(
        {RequestLifecycleStopAction{},
         SendControllerTextAction{"STOPPING\n"},
         DispatchProfileChangeAction{change}},
        control_plane,
        controller,
        helper_sockets.first.get(),
        helper_conversation)};

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, LifecycleEventKind::stop_requested);
    std::string controller_text;
    ASSERT_EQ(
        read_socket_data(controller_sockets.second.get(), controller_text).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(controller_text, "STOPPING\n");
    std::string helper_text;
    ASSERT_EQ(
        read_socket_data(helper_sockets.second.get(), helper_text).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(helper_text, "PROFILE_SET_DELAY OUTBOUND 10\n");

    ControllerControlPlane failed_control_plane{make_control_plane()};
    SupervisorHelperConversation fresh_helper_conversation;
    ASSERT_EQ(
        fresh_helper_conversation.receive_bytes("READY\nACTIVE 456\n").size(),
        2U);
    SocketPair failed_controller_sockets{make_socket_pair()};
    SocketPair fresh_helper_sockets{make_socket_pair()};
    std::optional<FileDescriptor> failed_controller{
        std::move(failed_controller_sockets.first)};
    failed_controller_sockets.second.reset();

    EXPECT_TRUE(execute_control_plane_actions(
        {SendControllerTextAction{"unavailable\n"},
         DispatchProfileChangeAction{change}},
        failed_control_plane,
        failed_controller,
        fresh_helper_sockets.first.get(),
        fresh_helper_conversation).empty());
    EXPECT_FALSE(failed_controller.has_value());
    EXPECT_TRUE(fresh_helper_conversation.begin_profile_change(change).has_value());
}

} // namespace
} // namespace netlaglab
