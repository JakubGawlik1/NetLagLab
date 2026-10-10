#include "workload_process.hpp"
#include "workload_resolver_view.hpp"
#include "file_descriptor.hpp"
#include "socket_io.hpp"

#include <gtest/gtest.h>

#include <array>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace netlaglab {
namespace {

WorkloadIdentity current_identity()
{
    const uid_t uid{geteuid()};
    const int group_count{getgroups(0, nullptr)};
    std::vector<gid_t> groups(static_cast<std::size_t>(group_count));
    if (group_count > 0) {
        (void)getgroups(group_count, groups.data());
    }
    return {uid, getegid(), std::move(groups)};
}

TEST(WorkloadProcessTest, ExecSuccessPrecedesImmediateWorkloadExit)
{
    const WorkloadContext context{
        .working_directory = "/",
        .arguments = {"/bin/true"},
        .environment = {"PATH=/bin"},
    };

    WorkloadLaunchResult launch{launch_workload(context, current_identity())};

    ASSERT_TRUE(launch.process.has_value());
    const std::optional<WorkloadStatus> status{launch.process->wait()};
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(status->exited);
    EXPECT_EQ(status->value, 0);
}

TEST(WorkloadProcessTest, UsesOnlyFirstTransmittedPathEntry)
{
    const WorkloadContext context{
        .working_directory = "/",
        .arguments = {"true"},
        .environment = {"PATH=/definitely-missing", "PATH=/bin"},
    };

    WorkloadLaunchResult launch{launch_workload(context, current_identity())};

    EXPECT_EQ(launch.failure_exit_code, 127);
    EXPECT_FALSE(launch.process.has_value());
}

TEST(WorkloadProcessTest, PreservesArgumentsEnvironmentAndWorkingDirectory)
{
    const WorkloadContext context{
        .working_directory = "/tmp",
        .arguments = {WORKLOAD_PROBE_PATH, "argument\nwith-tab\t"},
        .environment = {"PATH=/bin", "DUP=first", "DUP=second", "EMPTY="},
    };

    WorkloadLaunchResult launch{launch_workload(context, current_identity())};

    ASSERT_TRUE(launch.process.has_value());
    const auto status{launch.process->wait()};
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(status->exited);
    EXPECT_EQ(status->value, 0);
}

TEST(WorkloadProcessTest, ResolvesBareProgramUsingTransmittedPath)
{
    const WorkloadContext context{
        .working_directory = "/",
        .arguments = {"true"},
        .environment = {"PATH=/bin"},
    };

    WorkloadLaunchResult launch{launch_workload(context, current_identity())};

    ASSERT_TRUE(launch.process.has_value());
    const auto status{launch.process->wait()};
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(status->exited);
    EXPECT_EQ(status->value, 0);
}

TEST(WorkloadProcessTest, MissingProgramAndNonExecutableMapToConventionalResults)
{
    const WorkloadContext missing{
        .working_directory = "/",
        .arguments = {"netlaglab-program-that-does-not-exist"},
        .environment = {"PATH=/bin"},
    };
    const WorkloadContext non_executable{
        .working_directory = "/",
        .arguments = {"/"},
        .environment = {"PATH=/bin"},
    };

    EXPECT_EQ(launch_workload(missing, current_identity()).failure_exit_code, 127);
    EXPECT_EQ(
        launch_workload(non_executable, current_identity()).failure_exit_code,
        126);
}

TEST(WorkloadProcessTest, NamespaceEntryFailurePreventsExec)
{
    const WorkloadContext context{
        .working_directory = "/",
        .arguments = {"true"},
        .environment = {"PATH=/bin"},
    };
    const network_environment::WorkloadNamespaceEntry unavailable_namespace;

    WorkloadLaunchResult launch{
        launch_workload(context, current_identity(), {}, &unavailable_namespace)};

    EXPECT_FALSE(launch.process.has_value());
    EXPECT_EQ(launch.failure_exit_code, 125);
}

TEST(WorkloadProcessTest, ResolverViewSetupFailurePreventsExec)
{
    const WorkloadContext context{
        .working_directory = "/",
        .arguments = {"/bin/true"},
        .environment = {"PATH=/bin"},
    };
    const WorkloadResolverView invalid_view{FileDescriptor{-1}, FileDescriptor{-1}};

    WorkloadLaunchResult launch{launch_workload(
        context, current_identity(), {}, nullptr, &invalid_view)};

    EXPECT_FALSE(launch.process.has_value());
    EXPECT_EQ(launch.failure_exit_code, 125);
}

TEST(WorkloadProcessTest, ExplicitlyRedirectsWorkloadStandardOutput)
{
    int pipe_descriptors[2]{};
    ASSERT_EQ(pipe2(pipe_descriptors, O_CLOEXEC), 0);
    FileDescriptor reader{pipe_descriptors[0]};
    FileDescriptor writer{pipe_descriptors[1]};
    const WorkloadContext context{
        .working_directory = "/",
        .arguments = {"/bin/echo", "redirected"},
        .environment = {"PATH=/bin"},
    };
    WorkloadStandardDescriptors descriptors;
    descriptors.sources[1] = writer.get();

    WorkloadLaunchResult launch{
        launch_workload(context, current_identity(), descriptors)};
    writer.reset();
    ASSERT_TRUE(launch.process.has_value());
    ASSERT_TRUE(launch.process->wait().has_value());

    std::array<char, 32> output{};
    const ssize_t size{read(reader.get(), output.data(), output.size())};
    ASSERT_GT(size, 0);
    EXPECT_EQ(std::string_view(output.data(), static_cast<std::size_t>(size)),
              "redirected\n");
}

TEST(WorkloadProcessTest, TransfersOpenDescriptionsAndClosedMarkersOverUnixSocket)
{
    int sockets[2]{};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    FileDescriptor sender{sockets[0]};
    FileDescriptor receiver{sockets[1]};
    int pipe_descriptors[2]{};
    ASSERT_EQ(pipe2(pipe_descriptors, O_CLOEXEC), 0);
    FileDescriptor reader{pipe_descriptors[0]};
    FileDescriptor writer{pipe_descriptors[1]};
    const int closed_descriptor{dup(STDERR_FILENO)};
    ASSERT_NE(closed_descriptor, -1);
    ASSERT_EQ(close(closed_descriptor), 0);

    ASSERT_TRUE(send_standard_descriptors(
        sender.get(), {STDIN_FILENO, writer.get(), closed_descriptor}));
    auto received{receive_standard_descriptors(receiver.get())};

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(
        received->dispositions[1], StandardDescriptorDisposition::transferred);
    EXPECT_EQ(
        received->dispositions[2],
        StandardDescriptorDisposition::explicitly_closed);
    ASSERT_TRUE(received->descriptors[1].has_value());
    constexpr std::string_view payload{"same-open-description"};
    ASSERT_EQ(
        write(received->descriptors[1]->get(), payload.data(), payload.size()),
        static_cast<ssize_t>(payload.size()));
    std::array<char, 32> data{};
    ASSERT_EQ(
        read(reader.get(), data.data(), data.size()),
        static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(std::string_view(data.data(), payload.size()), payload);
}

} // namespace
} // namespace netlaglab
