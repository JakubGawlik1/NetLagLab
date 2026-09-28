#include "controller_control_plane.hpp"

#include "file_descriptor.hpp"
#include "socket_io.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <variant>

namespace netlaglab {
namespace {

struct SocketPair {
    FileDescriptor supervisor;
    FileDescriptor controller;
};

SocketPair make_socket_pair()
{
    int descriptors[2]{};
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors), 0);
    return {FileDescriptor{descriptors[0]}, FileDescriptor{descriptors[1]}};
}

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

TEST(ControllerControlPlaneTest, RetainsPartialStopCommandAndReturnsTypedRequest)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(sockets.controller.get(), "st"))
        << std::strerror(errno) << " fd=" << sockets.controller.get();
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);
    ASSERT_TRUE(send_socket_text(sockets.controller.get(), "op\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::stop_requested);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(response, "STOPPING\n");
}

TEST(ControllerControlPlaneTest, SendsFixedUnknownCommandErrorAndStaysConnected)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(sockets.controller.get(), "bad\tcommand\n"))
        << std::strerror(errno) << " fd=" << sockets.controller.get();
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(response, "ERROR Unknown command.\n");

    ASSERT_TRUE(send_socket_text(sockets.controller.get(), "help\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);
}

TEST(ControllerControlPlaneTest, RecognizesProfileChangesAsTemporarilyUnavailable)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(
        sockets.controller.get(),
        "set outbound delay 10ms\nreset inbound loss\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(
        response,
        "ERROR Profile changes are not available yet.\n"
        "ERROR Profile changes are not available yet.\n");
}

TEST(ControllerControlPlaneTest, IgnoresBlankLineAndProcessesFollowingCommand)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(sockets.controller.get(), " \t \nhelp\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(
        response,
        "HELP_BEGIN\n"
        "help - Show controller commands\n"
        "status - Show the active session\n"
        "stop - Stop the active Workload\n"
        "detach - Disconnect this controller\n"
        "HELP_END\n");
}

TEST(ControllerControlPlaneTest, OversizedCompleteCommandReportsAndDisconnects)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(
        sockets.controller.get(),
        std::string(maximum_controller_command_size + 1, 'x') + '\n'));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::disconnected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(response, "ERROR Command exceeds 1024 bytes.\n");
}

TEST(ControllerControlPlaneTest, OversizedPartialCommandReportsAndDisconnects)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(
        sockets.controller.get(),
        std::string(maximum_controller_command_size + 1, 'x')));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::disconnected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(response, "ERROR Command exceeds 1024 bytes.\n");
}

TEST(ControllerControlPlaneTest, MapsOrdinaryParserErrorsAndKeepsConnection)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};
    const std::string underflow{"set inbound loss 0."
        + std::string(400, '0') + "1\n"};

    ASSERT_TRUE(send_socket_text(
        sockets.controller.get(),
        std::string{"help\r\n"
                    "wat\n"
                    "status now\n"
                    "set sideways delay 1\n"
                    "reset outbound latency\n"
                    "set outbound delay 1.5ms\n"
                    "set outbound delay 1mss\n"
                    "set outbound bandwidth 0\n"
                    "set outbound bandwidth 18446744073709551616\n"}
            + underflow + "help\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(
        response,
        "ERROR Command contains an invalid character.\n"
        "ERROR Unknown command.\n"
        "ERROR Wrong number of arguments.\n"
        "ERROR Direction must be outbound or inbound.\n"
        "ERROR Setting must be delay, jitter, loss, or bandwidth.\n"
        "ERROR Invalid numeric value.\n"
        "ERROR Invalid unit for setting.\n"
        "ERROR Value is outside the allowed range.\n"
        "ERROR Value is too large.\n"
        "ERROR Value is too small to represent.\n"
        "HELP_BEGIN\n"
        "help - Show controller commands\n"
        "status - Show the active session\n"
        "stop - Stop the active Workload\n"
        "detach - Disconnect this controller\n"
        "HELP_END\n");
}

TEST(ControllerControlPlaneTest, PreservesStatusPresentation)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char argument[]{"hello"};
    char* arguments[]{program, argument, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(sockets.controller.get(), "status\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::connected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_NE(response.find("STATUS_BEGIN\nstate: running\npid: 1234\n"),
        std::string::npos);
    EXPECT_NE(response.find("program: /bin/true\nargument[0]: hello\n"),
        std::string::npos);
    EXPECT_NE(response.find("shaping: not applied\n"), std::string::npos);
    EXPECT_NE(response.find("STATUS_END\n"), std::string::npos);
}

TEST(ControllerControlPlaneTest, PreservesDetachResponseAndDisconnectsController)
{
    SocketPair sockets{make_socket_pair()};
    char program[]{"/bin/true"};
    char* arguments[]{program, nullptr};
    ControllerConversation conversation{1234, arguments};

    ASSERT_TRUE(send_socket_text(sockets.controller.get(), "detach\n"));
    EXPECT_EQ(
        conversation.receive(sockets.supervisor.get()),
        ControllerReadResult::disconnected);

    std::string response;
    ASSERT_EQ(
        read_socket_data(sockets.controller.get(), response).status,
        SocketReadStatus::data_received);
    EXPECT_EQ(response, "DETACHED\n");
}

} // namespace
} // namespace netlaglab
