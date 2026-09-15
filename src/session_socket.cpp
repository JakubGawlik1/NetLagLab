#include "session_socket.hpp"

#include "file_descriptor.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <ostream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace netlaglab {
namespace {

constexpr int listen_backlog{1};

} // namespace

SocketPathOwner::SocketPathOwner(
    const int session_directory_descriptor,
    const std::string_view socket_name,
    const std::string& socket_path)
    : session_directory_descriptor_{session_directory_descriptor},
      socket_name_{socket_name},
      socket_path_{socket_path}
{
}

SocketPathOwner::~SocketPathOwner()
{
    (void)remove_owned();
}

const std::string& SocketPathOwner::name() const noexcept
{
    return socket_name_;
}

const std::string& SocketPathOwner::path() const noexcept
{
    return socket_path_;
}

bool SocketPathOwner::remove_stale(std::ostream& error) const
{
    struct stat socket_status {};
    if (fstatat(
            session_directory_descriptor_,
            socket_name_.data(),
            &socket_status,
            AT_SYMLINK_NOFOLLOW)
        == -1) {
        if (errno == ENOENT) {
            return true;
        }

        const int status_error{errno};
        error << "NetLagLab: failed to inspect " << socket_name_ << ": "
              << std::strerror(status_error) << '\n';
        return false;
    }

    if (!S_ISSOCK(socket_status.st_mode)) {
        error << "NetLagLab: refusing to remove " << socket_name_
              << " because it is not a socket\n";
        return false;
    }

    if (socket_status.st_uid != geteuid()) {
        error << "NetLagLab: refusing to remove " << socket_name_
              << " owned by another user\n";
        return false;
    }

    if (unlinkat(session_directory_descriptor_, socket_name_.c_str(), 0) == -1) {
        const int unlink_error{errno};
        error << "NetLagLab: failed to remove stale " << socket_name_ << ": "
              << std::strerror(unlink_error) << '\n';
        return false;
    }

    return true;
}

void SocketPathOwner::mark_owned()
{
    owned_ = true;
}

int SocketPathOwner::remove_owned()
{
    if (!owned_) {
        return 0;
    }

    if (unlinkat(session_directory_descriptor_, socket_name_.c_str(), 0) == 0) {
        owned_ = false;
        return 0;
    }

    const int unlink_error{errno};
    if (unlink_error == ENOENT) {
        owned_ = false;
        return 0;
    }

    return unlink_error;
}

int create_listening_socket(
    const int session_directory_descriptor,
    SocketPathOwner& socket_path_owner,
    std::ostream& error)
{
    const std::string& socket_path{socket_path_owner.path()};
    const std::string& socket_name{socket_path_owner.name()};

    struct sockaddr_un address {};
    if (socket_path.size() >= sizeof(address.sun_path)) {
        error << "NetLagLab: " << socket_name << " path is too long\n";
        return -1;
    }

    const int raw_socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (raw_socket == -1) {
        const int socket_error{errno};
        error << "NetLagLab: failed to create " << socket_name << ": "
              << std::strerror(socket_error) << '\n';
        return -1;
    }
    FileDescriptor listening_socket{raw_socket};

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const auto address_size{static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1)};

    if (bind(
            listening_socket.get(),
            reinterpret_cast<const struct sockaddr*>(&address),
            address_size)
        == -1) {
        const int bind_error{errno};
        error << "NetLagLab: failed to bind " << socket_name << ": "
              << std::strerror(bind_error) << '\n';
        return -1;
    }
    socket_path_owner.mark_owned();

    if (fchmodat(session_directory_descriptor, socket_name.c_str(), 0600, 0) == -1) {
        const int chmod_error{errno};
        error << "NetLagLab: failed to set permissions on " << socket_name << ": "
              << std::strerror(chmod_error) << '\n';
        return -1;
    }

    if (listen(listening_socket.get(), listen_backlog) == -1) {
        const int listen_error{errno};
        error << "NetLagLab: failed to listen on " << socket_name << ": "
              << std::strerror(listen_error) << '\n';
        return -1;
    }

    return listening_socket.release();
}

} // namespace netlaglab
