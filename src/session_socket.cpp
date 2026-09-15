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
constexpr mode_t socket_creation_mask{0177};

} // namespace

SocketPathOwner::SocketPathOwner(
    const int session_directory_descriptor,
    const std::string_view socket_name,
    const std::string& socket_path,
    const uid_t owner_uid)
    : session_directory_descriptor_{session_directory_descriptor},
      socket_name_{socket_name},
      socket_path_{socket_path},
      owner_uid_{owner_uid}
{
}

SocketPathOwner::~SocketPathOwner()
{
    (void)remove_owned();
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

    if (socket_status.st_uid != owner_uid_ && socket_status.st_uid != geteuid()) {
        error << "NetLagLab: refusing to remove " << socket_name_
              << " owned by an unexpected user\n";
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

int SocketPathOwner::create_listening_socket(std::ostream& error)
{
    struct sockaddr_un address {};
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        error << "NetLagLab: " << socket_name_ << " path is too long\n";
        return -1;
    }

    const int raw_socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (raw_socket == -1) {
        const int socket_error{errno};
        error << "NetLagLab: failed to create " << socket_name_ << ": "
              << std::strerror(socket_error) << '\n';
        return -1;
    }
    FileDescriptor listening_socket{raw_socket};

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    const auto address_size{static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path_.size() + 1)};

    // The helper can bind inside a user-owned directory while running as root.
    // Apply 0600 at creation time to avoid a wider-permission window and a
    // path-based chmod after bind.
    const mode_t previous_mask{umask(socket_creation_mask)};
    const int bind_result{bind(
        listening_socket.get(),
        reinterpret_cast<const struct sockaddr*>(&address),
        address_size)};
    const int bind_error{errno};
    (void)umask(previous_mask);

    if (bind_result == -1) {
        error << "NetLagLab: failed to bind " << socket_name_ << ": "
              << std::strerror(bind_error) << '\n';
        return -1;
    }
    owned_ = true;

    if (owner_uid_ != geteuid()
        && fchownat(
               session_directory_descriptor_,
               socket_name_.c_str(),
               owner_uid_,
               static_cast<gid_t>(-1),
               AT_SYMLINK_NOFOLLOW)
            == -1) {
        const int owner_error{errno};
        error << "NetLagLab: failed to set owner of " << socket_name_ << ": "
              << std::strerror(owner_error) << '\n';
        return -1;
    }

    if (listen(listening_socket.get(), listen_backlog) == -1) {
        const int listen_error{errno};
        error << "NetLagLab: failed to listen on " << socket_name_ << ": "
              << std::strerror(listen_error) << '\n';
        return -1;
    }

    return listening_socket.release();
}

} // namespace netlaglab
