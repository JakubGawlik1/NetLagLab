#include "network_environment/command_runner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace netlaglab::network_environment {
namespace {

TEST(NetworkCommandRunnerTest, PreservesArgumentsAndUsesAnEmptyEnvironment)
{
    const std::vector<std::string> arguments{
        NETWORK_COMMAND_PROBE_PATH,
        "first argument",
        "second\nargument",
    };
    const auto started{std::chrono::steady_clock::now()};

    const CommandResult result{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        arguments,
        started + std::chrono::seconds{2})};

    EXPECT_EQ(result.kind, CommandResultKind::success);
    EXPECT_EQ(result.code, 0);
    EXPECT_TRUE(result.standard_error.empty());
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds{1});
}

TEST(NetworkCommandRunnerTest, ReportsExecFailureSeparatelyFromProgramExit)
{
    const std::vector<std::string> arguments;

    const CommandResult result{run_command(
        "/definitely/missing/netlaglab-command",
        arguments,
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};

    EXPECT_EQ(result.kind, CommandResultKind::exec_failure);
    EXPECT_EQ(result.code, ENOENT);
}

TEST(NetworkCommandRunnerTest, ExpiredDeadlineStartsNoChild)
{
    const std::string marker{
        "/tmp/netlaglab-expired-command-" + std::to_string(getpid())};
    (void)unlink(marker.c_str());
    const std::vector<std::string> arguments{"create-marker", marker};

    const CommandResult result{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        arguments,
        std::chrono::steady_clock::now() - std::chrono::seconds{1})};

    EXPECT_EQ(result.kind, CommandResultKind::timeout);
    EXPECT_EQ(result.code, 0);
    errno = 0;
    EXPECT_EQ(access(marker.c_str(), F_OK), -1);
    EXPECT_EQ(errno, ENOENT);
    (void)unlink(marker.c_str());
}

TEST(NetworkCommandRunnerTest, CapturesStandardError)
{
    const std::vector<std::string> arguments{"stderr", "10"};

    const CommandResult result{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        arguments,
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};

    EXPECT_EQ(result.kind, CommandResultKind::success);
    EXPECT_EQ(result.standard_error, std::string(10, 'x'));
}

TEST(NetworkCommandRunnerTest, DistinguishesNonzeroExitFromSignalTermination)
{
    const CommandResult exited{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        std::vector<std::string>{"exit", "23"},
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};
    const CommandResult signaled{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        std::vector<std::string>{"signal"},
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};

    EXPECT_EQ(exited.kind, CommandResultKind::nonzero_exit);
    EXPECT_EQ(exited.code, 23);
    EXPECT_EQ(signaled.kind, CommandResultKind::signal);
    EXPECT_EQ(signaled.code, SIGTERM);
}

TEST(NetworkCommandRunnerTest, BoundsAndDrainsLargeStandardError)
{
    const std::vector<std::string> arguments{"stderr", "131072"};

    const CommandResult result{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        arguments,
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};

    EXPECT_EQ(result.kind, CommandResultKind::success);
    EXPECT_EQ(result.standard_error, std::string(4096, 'x'));
}

TEST(NetworkCommandRunnerTest, TimeoutTerminatesAndReapsTheChild)
{
    const std::vector<std::string> arguments{"sleep", "2000"};
    const auto started{std::chrono::steady_clock::now()};

    const CommandResult result{run_command(
        NETWORK_COMMAND_PROBE_PATH,
        arguments,
        started + std::chrono::milliseconds{50})};

    EXPECT_EQ(result.kind, CommandResultKind::timeout);
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds{1});
    ASSERT_FALSE(result.standard_error.empty());
    const pid_t child_pid{static_cast<pid_t>(std::stoi(result.standard_error))};
    int status{};
    errno = 0;
    EXPECT_EQ(waitpid(child_pid, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(NetworkCommandRunnerTest, InheritsTheExplicitNamespaceDescriptor)
{
    const int descriptor{open("/dev/null", O_RDONLY | O_CLOEXEC)};
    ASSERT_NE(descriptor, -1);
    const std::vector<std::string> arguments{
        "fd-open", std::to_string(descriptor)};

    const CommandResult result{run_command_with_inherited_descriptor(
        NETWORK_COMMAND_PROBE_PATH,
        arguments,
        descriptor,
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};

    EXPECT_EQ(result.kind, CommandResultKind::success);
    EXPECT_EQ(close(descriptor), 0);
}

TEST(NetworkCommandRunnerTest, ReportsNamespaceEntryFailureBeforeExec)
{
    const int descriptor{open("/dev/null", O_RDONLY | O_CLOEXEC)};
    ASSERT_NE(descriptor, -1);
    const std::string marker{
        "/tmp/netlaglab-setns-command-" + std::to_string(getpid())};
    (void)unlink(marker.c_str());

    const CommandResult result{run_command_in_network_namespace(
        NETWORK_COMMAND_PROBE_PATH,
        std::vector<std::string>{"create-marker", marker},
        descriptor,
        std::chrono::steady_clock::now() + std::chrono::seconds{2})};

    EXPECT_EQ(result.kind, CommandResultKind::system_failure);
    EXPECT_NE(result.code, 0);
    errno = 0;
    EXPECT_EQ(access(marker.c_str(), F_OK), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(close(descriptor), 0);
    (void)unlink(marker.c_str());
}

} // namespace
} // namespace netlaglab::network_environment
