#include "file_descriptor.hpp"
#include "session_paths.hpp"
#include "session_validation.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

#include <cstdlib>
#include <iostream>

namespace {

constexpr std::string_view usage{
    "Usage: netlaglab-helper --runtime-dir <absolute-path>\n"};
constexpr int usage_error_exit_code{2};

[[nodiscard]] std::optional<netlaglab::FileDescriptor> validate_session(
    const netlaglab::SessionPaths& paths,
    const uid_t expected_owner,
    std::ostream& error)
{
    std::optional<netlaglab::FileDescriptor> runtime_directory{
        netlaglab::open_and_validate_runtime_directory(
            paths.xdg_runtime_directory(), expected_owner, error)};
    if (!runtime_directory.has_value()) {
        return std::nullopt;
    }

    std::optional<netlaglab::FileDescriptor> session_directory{
        netlaglab::open_and_validate_session_directory(
            runtime_directory->get(), expected_owner, error)};
    if (!session_directory.has_value()) {
        return std::nullopt;
    }

    const int lock_descriptor{openat(
        session_directory->get(),
        netlaglab::SessionPaths::lock_file_name.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (lock_descriptor == -1) {
        const int open_error{errno};
        error << "NetLagLab helper: failed to open session.lock: "
              << std::strerror(open_error) << '\n';
        return std::nullopt;
    }
    netlaglab::FileDescriptor session_lock{lock_descriptor};

    struct stat lock_status {};
    if (fstat(session_lock.get(), &lock_status) == -1) {
        const int status_error{errno};
        error << "NetLagLab helper: failed to inspect session.lock: "
              << std::strerror(status_error) << '\n';
        return std::nullopt;
    }

    if (!S_ISREG(lock_status.st_mode)) {
        error << "NetLagLab helper: session.lock is not a regular file\n";
        return std::nullopt;
    }

    if (lock_status.st_uid != expected_owner) {
        error << "NetLagLab helper: session.lock has an unexpected owner\n";
        return std::nullopt;
    }

    if ((lock_status.st_mode & 0777) != 0600) {
        error << "NetLagLab helper: session.lock must have permissions 0600\n";
        return std::nullopt;
    }

    if (flock(session_lock.get(), LOCK_EX | LOCK_NB) == 0) {
        error << "NetLagLab helper: session.lock is not held by an active supervisor\n";
        return std::nullopt;
    }

    const int lock_error{errno};
    if (lock_error != EWOULDBLOCK && lock_error != EAGAIN) {
        error << "NetLagLab helper: failed to inspect the session lock: "
              << std::strerror(lock_error) << '\n';
        return std::nullopt;
    }

    return session_directory;
}

[[nodiscard]] std::optional<uid_t> read_sudo_uid(std::ostream& error)
{
    const char* const sudo_uid_text{std::getenv("SUDO_UID")};
    if (sudo_uid_text == nullptr || sudo_uid_text[0] == '\0') {
        error << "NetLagLab helper: SUDO_UID is not set\n";
        return std::nullopt;
    }
    const std::string_view text{sudo_uid_text};

    uid_t parsed_uid{};
    const auto result{
        std::from_chars(text.data(), text.data() + text.size(), parsed_uid)};

    const bool success{
        result.ec == std::errc{} && result.ptr == text.data() + text.size()};
    if (!success) {
        error << "NetLagLab helper: failed to parse SUDO_UID\n";
        return std::nullopt;
    }

    return parsed_uid;
}

int usage_error(const std::string_view message)
{
    std::cerr << "NetLagLab helper: " << message << '\n' << usage;
    return usage_error_exit_code;
}

int run_helper(const int argc, char* argv[])
{
    if (argc != 3 || std::string_view{argv[1]} != "--runtime-dir") {
        return usage_error("expected '--runtime-dir' followed by a path");
    }

    const std::string_view runtime_directory{argv[2]};
    if (runtime_directory.empty() || runtime_directory.front() != '/') {
        return usage_error("runtime directory must be an absolute path");
    }

    if (geteuid() != 0) {
        std::cerr << "NetLagLab helper: root privileges are required\n";
        return EXIT_FAILURE;
    }

    const std::optional<uid_t> sudo_uid{read_sudo_uid(std::cerr)};
    if (!sudo_uid.has_value()) {
        return 125;
    }

    const netlaglab::SessionPaths paths{std::string{runtime_directory}};
    const std::optional<netlaglab::FileDescriptor> session_directory{
        validate_session(paths, *sudo_uid, std::cerr)};
    if (!session_directory.has_value()) {
        return 125;
    }

    return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, char* argv[])
{
    return run_helper(argc, argv);
}
