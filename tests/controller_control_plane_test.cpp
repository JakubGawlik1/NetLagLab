#include "controller_control_plane.hpp"

#include "file_descriptor.hpp"
#include "socket_io.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>

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

TEST(ControllerControlPlaneTest, EscapesUnknownCommandsWithoutBreakingFraming)
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
    EXPECT_EQ(response, "ERROR Unknown command: bad\\tcommand\n");
}

} // namespace
} // namespace netlaglab
