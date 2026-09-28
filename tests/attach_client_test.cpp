#include "attach_client.hpp"

#include "file_descriptor.hpp"
#include "socket_io.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace netlaglab {
namespace {

struct AttachRunResult {
    int status;
    std::string output;
    std::string error;
};

AttachRunResult run_controller_with_response(
    const std::string_view response,
    const bool close_peer = false)
{
    int descriptors[2]{};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) == -1) {
        ADD_FAILURE() << "socketpair failed";
        return {1, {}, {}};
    }
    FileDescriptor controller{descriptors[0]};
    FileDescriptor supervisor{descriptors[1]};
    if (!response.empty() && !send_socket_text(supervisor.get(), response)) {
        ADD_FAILURE() << "failed to send scripted Supervisor response";
        return {1, {}, {}};
    }
    if (close_peer) {
        supervisor.reset();
    }

    std::ostringstream output;
    std::ostringstream error;
    const int status{
        run_attached_controller(controller.get(), output, error)};
    return {status, output.str(), error.str()};
}

TEST(AttachClientTest, ReportsCleanExitAndSignalWithSuccessfulObservationStatus)
{
    const AttachRunResult exited{run_controller_with_response(
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD EXIT 23\n"
        "INFRASTRUCTURE OK\n"
        "SESSION_OUTCOME_END\n")};
    EXPECT_EQ(exited.status, 0);
    EXPECT_EQ(exited.output, "Session ended; Workload exit code: 23.\n");
    EXPECT_TRUE(exited.error.empty());

    const AttachRunResult signaled{run_controller_with_response(
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD SIGNAL 9\n"
        "INFRASTRUCTURE OK\n"
        "SESSION_OUTCOME_END\n")};
    EXPECT_EQ(signaled.status, 0);
    EXPECT_EQ(
        signaled.output,
        "Session ended; Workload terminated by signal 9.\n");
    EXPECT_TRUE(signaled.error.empty());
}

TEST(AttachClientTest, ReportsInfrastructureFailureWithKnownOrUnknownWorkload)
{
    const AttachRunResult known{run_controller_with_response(
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD EXIT 7\n"
        "INFRASTRUCTURE FAILED\n"
        "FAILURE HELPER_CONVERSATION\n"
        "SESSION_OUTCOME_END\n")};
    EXPECT_EQ(known.status, 1);
    EXPECT_TRUE(known.output.empty());
    EXPECT_EQ(
        known.error,
        "NetLagLab: Session infrastructure failed: helper conversation; "
        "Workload exit code: 7.\n");

    const AttachRunResult unknown{run_controller_with_response(
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD UNKNOWN\n"
        "INFRASTRUCTURE FAILED\n"
        "FAILURE STARTUP\n"
        "SESSION_OUTCOME_END\n")};
    EXPECT_EQ(unknown.status, 1);
    EXPECT_EQ(
        unknown.error,
        "NetLagLab: Session infrastructure failed: startup; Workload result "
        "is unknown.\n");
}

TEST(AttachClientTest, RejectsMalformedOrIncompleteTerminalBlocks)
{
    const AttachRunResult malformed{run_controller_with_response(
        "SESSION_OUTCOME_BEGIN\n"
        "WORKLOAD EXIT 999\n"
        "INFRASTRUCTURE OK\n"
        "SESSION_OUTCOME_END\n")};
    EXPECT_EQ(malformed.status, 1);
    EXPECT_EQ(malformed.error, "NetLagLab: invalid response from session\n");

    const AttachRunResult incomplete{run_controller_with_response(
        "SESSION_OUTCOME_BEGIN\nWORKLOAD EXIT 0\n", true)};
    EXPECT_EQ(incomplete.status, 1);
    EXPECT_EQ(
        incomplete.error,
        "NetLagLab: connection to session lost; session result is unknown.\n");

    const AttachRunResult disconnected{run_controller_with_response({}, true)};
    EXPECT_EQ(disconnected.status, 1);
    EXPECT_EQ(
        disconnected.error,
        "NetLagLab: connection to session lost; session result is unknown.\n");
}

TEST(AttachClientTest, RetainsLegacyTerminalCompatibility)
{
    const AttachRunResult ended{
        run_controller_with_response("SESSION_ENDED\n")};
    EXPECT_EQ(ended.status, 0);
    EXPECT_EQ(ended.output, "Session ended.\n");

    const AttachRunResult failed{
        run_controller_with_response("SESSION_FAILED\n")};
    EXPECT_EQ(failed.status, 1);
    EXPECT_EQ(failed.error, "NetLagLab: session failed\n");
}

} // namespace
} // namespace netlaglab
