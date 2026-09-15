#include "session_validation.hpp"

#include "session_paths.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ostream>
#include <sys/stat.h>

namespace netlaglab {

bool validate_runtime_directory(
    const int descriptor,
    const uid_t expected_owner,
    std::ostream& error)
{
    struct stat directory_status {};
    if (fstat(descriptor, &directory_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab: failed to inspect XDG_RUNTIME_DIR: "
              << std::strerror(status_error) << '\n';
        return false;
    }

    if (!S_ISDIR(directory_status.st_mode)) {
        error << "NetLagLab: XDG_RUNTIME_DIR is not a directory\n";
        return false;
    }

    if (directory_status.st_uid != expected_owner) {
        error << "NetLagLab: XDG_RUNTIME_DIR is not owned by the expected user\n";
        return false;
    }

    if ((directory_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        error << "NetLagLab: XDG_RUNTIME_DIR must not be accessible by group or other users\n";
        return false;
    }

    return true;
}

bool validate_session_directory(
    const int descriptor,
    const uid_t expected_owner,
    std::ostream& error)
{
    struct stat directory_status {};
    if (fstat(descriptor, &directory_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab: failed to inspect the netlaglab runtime directory: "
              << std::strerror(status_error) << '\n';
        return false;
    }

    if (!S_ISDIR(directory_status.st_mode)) {
        error << "NetLagLab: the netlaglab runtime path is not a directory\n";
        return false;
    }

    if (directory_status.st_uid != expected_owner) {
        error << "NetLagLab: the netlaglab runtime directory has an unexpected owner\n";
        return false;
    }

    if ((directory_status.st_mode & 0777) != 0700) {
        error << "NetLagLab: the netlaglab runtime directory must have permissions 0700\n";
        return false;
    }

    return true;
}

std::optional<FileDescriptor> open_and_validate_runtime_directory(
    const std::string& path,
    const uid_t expected_owner,
    std::ostream& error)
{
    const int descriptor{
        open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor == -1) {
        const int open_error{errno};
        error << "NetLagLab: failed to open XDG_RUNTIME_DIR: "
              << std::strerror(open_error) << '\n';
        return std::nullopt;
    }

    FileDescriptor directory{descriptor};
    if (!validate_runtime_directory(directory.get(), expected_owner, error)) {
        return std::nullopt;
    }

    return directory;
}

std::optional<FileDescriptor> open_and_validate_session_directory(
    const int runtime_directory_descriptor,
    const uid_t expected_owner,
    std::ostream& error)
{
    const int descriptor{openat(
        runtime_directory_descriptor,
        SessionPaths::session_directory_name.data(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor == -1) {
        const int open_error{errno};
        if (open_error == ENOENT) {
            error << "NetLagLab: no active session\n";
        } else {
            error << "NetLagLab: failed to open the netlaglab runtime directory: "
                  << std::strerror(open_error) << '\n';
        }
        return std::nullopt;
    }

    FileDescriptor directory{descriptor};
    if (!validate_session_directory(directory.get(), expected_owner, error)) {
        return std::nullopt;
    }

    return directory;
}

} // namespace netlaglab
