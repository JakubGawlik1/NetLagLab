#include <gtest/gtest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view authorization_variable{
    "NETLAGLAB_ALLOW_PRIVILEGED_TESTS"};
constexpr std::string_view token_variable{"NLL_SESSION_QUALIFICATION_TOKEN"};
constexpr std::string_view uid_variable{"NLL_SESSION_QUALIFICATION_UID"};
constexpr std::string_view gid_variable{"NLL_SESSION_QUALIFICATION_GID"};
constexpr std::string_view groups_variable{"NLL_SESSION_QUALIFICATION_GROUPS"};
constexpr std::string_view cwd_variable{"NLL_SESSION_QUALIFICATION_CWD"};
constexpr std::string_view host_namespace_variable{
    "NLL_SESSION_QUALIFICATION_HOST_NETNS"};
constexpr std::string_view udp_port_variable{
    "NLL_SESSION_QUALIFICATION_UDP_PORT"};
constexpr std::string_view marker_variable{
    "NLL_SESSION_QUALIFICATION_SIGNAL_MARKER"};
constexpr std::string_view stdin_variable{"NLL_SESSION_QUALIFICATION_STDIN"};
constexpr std::string_view stdin_value{"session-stdin-check"};
constexpr std::string_view request_prefix{"nll-session-request:"};
constexpr std::string_view response_prefix{"nll-session-response:"};
constexpr std::string_view ready_prefix{"NLL_SESSION_PROBE_READY:"};
constexpr std::string_view probe_stdout{"NLL_SESSION_PROBE_STDOUT\n"};
constexpr std::string_view probe_stderr{"NLL_SESSION_PROBE_STDERR\n"};
constexpr std::string_view udp_result_prefix{"NLL_SESSION_PROBE_UDP:"};

[[nodiscard]] int report_probe_failure(
    const int status,
    const std::string_view reason)
{
    std::cerr << "Session qualification probe failed: " << reason << '\n';
    return status;
}

volatile std::sig_atomic_t termination_requested{};

extern "C" void handle_termination(int)
{
    termination_requested = 1;
}

[[nodiscard]] std::string make_group_list()
{
    const int count{getgroups(0, nullptr)};
    if (count < 0) {
        return {};
    }
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (count != 0 && getgroups(count, groups.data()) != count) {
        return {};
    }
    std::ostringstream result;
    for (std::size_t index{}; index < groups.size(); ++index) {
        if (index != 0) {
            result << ',';
        }
        result << groups[index];
    }
    return result.str();
}

[[nodiscard]] const char* required_environment(const std::string_view name)
{
    const std::string owned_name{name};
    return std::getenv(owned_name.c_str());
}

[[nodiscard]] bool parse_unsigned(
    const std::string_view text,
    std::uint64_t& result)
{
    const auto parsed{std::from_chars(text.data(), text.data() + text.size(), result)};
    return !text.empty() && parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool verify_identity()
{
    const char* expected_uid{required_environment(uid_variable)};
    const char* expected_gid{required_environment(gid_variable)};
    const char* expected_groups{required_environment(groups_variable)};
    if (expected_uid == nullptr || expected_gid == nullptr
        || expected_groups == nullptr) {
        return false;
    }
    std::uint64_t uid{};
    std::uint64_t gid{};
    if (!parse_unsigned(expected_uid, uid) || !parse_unsigned(expected_gid, gid)
        || getuid() != uid || geteuid() != uid || getgid() != gid || getegid() != gid
        || make_group_list() != expected_groups) {
        return false;
    }
    return true;
}

[[nodiscard]] bool verify_arguments(
    const int argc,
    char* const argv[],
    const std::string_view expected_mode,
    const std::string_view expected_port,
    const std::string_view expected_token)
{
    if (argc != 5 || std::string_view{argv[0]} != NETLAGLAB_SESSION_PROBE_PATH
        || std::string_view{argv[1]} != "--probe"
        || std::string_view{argv[2]} != expected_mode
        || std::string_view{argv[3]} != expected_port
        || std::string_view{argv[4]} != expected_token) {
        return false;
    }
    const char* expected_token_environment{required_environment(token_variable)};
    return expected_token_environment != nullptr
        && expected_token == expected_token_environment;
}

[[nodiscard]] bool verify_working_directory()
{
    const char* expected{required_environment(cwd_variable)};
    if (expected == nullptr) {
        return false;
    }
    char current[4096]{};
    return getcwd(current, sizeof(current)) != nullptr
        && std::string_view{current} == expected;
}

[[nodiscard]] bool verify_network_namespaces(std::uint64_t& session_inode)
{
    const char* expected_host{required_environment(host_namespace_variable)};
    if (expected_host == nullptr) {
        return false;
    }
    std::uint64_t host_inode{};
    if (!parse_unsigned(expected_host, host_inode)) {
        return false;
    }
    struct stat workload_namespace {};
    struct stat helper_namespace {};
    const std::string helper_namespace_path{
        "/proc/" + std::to_string(getppid()) + "/ns/net"};
    if (stat("/proc/self/ns/net", &workload_namespace) == -1
        || stat(helper_namespace_path.c_str(), &helper_namespace) == -1) {
        return false;
    }
    session_inode = static_cast<std::uint64_t>(workload_namespace.st_ino);
    return session_inode != host_inode
        && static_cast<std::uint64_t>(helper_namespace.st_ino) == host_inode;
}

[[nodiscard]] bool verify_session_address()
{
    ifaddrs* raw_addresses{};
    if (getifaddrs(&raw_addresses) == -1) {
        return false;
    }
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses{
        raw_addresses, freeifaddrs};
    in_addr expected_address{};
    in_addr expected_mask{};
    if (inet_pton(AF_INET, "10.200.0.2", &expected_address) != 1
        || inet_pton(AF_INET, "255.255.255.252", &expected_mask) != 1) {
        return false;
    }
    for (const ifaddrs* entry{addresses.get()}; entry != nullptr;
         entry = entry->ifa_next) {
        if (entry->ifa_name == nullptr
            || std::strcmp(entry->ifa_name, "nll-app") != 0
            || entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr
            || entry->ifa_addr->sa_family != AF_INET
            || entry->ifa_netmask->sa_family != AF_INET) {
            continue;
        }
        const auto* address{
            reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)};
        const auto* mask{
            reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)};
        if (address->sin_addr.s_addr == expected_address.s_addr
            && mask->sin_addr.s_addr == expected_mask.s_addr) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool verify_no_inherited_privileged_descriptor()
{
    DIR* directory{opendir("/proc/self/fd")};
    if (directory == nullptr) {
        return false;
    }
    bool found_privileged_descriptor{};
    while (dirent* entry{readdir(directory)}) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string descriptor_path{
            std::string{"/proc/self/fd/"} + entry->d_name};
        char target[256]{};
        const ssize_t size{readlink(
            descriptor_path.c_str(), target, sizeof(target) - 1)};
        if (size == -1) {
            if (errno != ENOENT) {
                found_privileged_descriptor = true;
                break;
            }
            continue;
        }
        const std::string_view value{target, static_cast<std::size_t>(size)};
        if ((value.starts_with("net:[") || value == "/run/netns/netlaglab")
            || value == "/run/netlaglab/host.lock"
            || value.starts_with("socket:[")) {
            found_privileged_descriptor = true;
            break;
        }
    }
    const int close_result{closedir(directory)};
    return close_result == 0 && !found_privileged_descriptor;
}

[[nodiscard]] bool perform_udp_exchange(
    const std::uint16_t port,
    const std::string_view token)
{
    const int descriptor{socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (descriptor == -1) {
        return false;
    }
    sockaddr_in host{};
    host.sin_family = AF_INET;
    host.sin_port = htons(port);
    if (inet_pton(AF_INET, "10.200.0.1", &host.sin_addr) != 1) {
        (void)close(descriptor);
        return false;
    }
    const std::string request{std::string{request_prefix} + std::string{token}};
    const ssize_t sent{sendto(
        descriptor,
        request.data(),
        request.size(),
        0,
        reinterpret_cast<const sockaddr*>(&host),
        sizeof(host))};
    if (sent != static_cast<ssize_t>(request.size())) {
        (void)close(descriptor);
        return false;
    }

    pollfd watched{descriptor, POLLIN, 0};
    int ready{};
    do {
        ready = poll(&watched, 1, 5000);
    } while (ready == -1 && errno == EINTR);
    char response[256]{};
    const ssize_t received{ready > 0 && (watched.revents & POLLIN) != 0
            ? recv(descriptor, response, sizeof(response), 0)
            : -1};
    const std::string expected{
        std::string{response_prefix} + std::string{token}};
    const bool succeeded{received == static_cast<ssize_t>(expected.size())
        && std::string_view{response, static_cast<std::size_t>(received)} == expected};
    (void)close(descriptor);
    return succeeded;
}

[[nodiscard]] int run_workload_probe(const int argc, char* argv[])
{
    if (argc != 5) {
        return report_probe_failure(80, "unexpected probe argument count");
    }
    const std::string_view mode{argv[2]};
    const std::string_view port_text{argv[3]};
    const std::string_view token{argv[4]};
    if (mode != "natural" && mode != "hold") {
        return report_probe_failure(81, "unexpected probe mode");
    }
    std::uint64_t port_value{};
    if (!parse_unsigned(port_text, port_value) || port_value == 0
        || port_value > 65535) {
        return report_probe_failure(81, "invalid UDP port");
    }
    if (!verify_arguments(argc, argv, mode, port_text, token)) {
        return report_probe_failure(82, "argv or token environment mismatch");
    }
    if (!verify_identity()) {
        return report_probe_failure(82, "restored UID, GID, or supplementary groups mismatch");
    }
    if (!verify_working_directory()) {
        return report_probe_failure(82, "working directory mismatch");
    }
    if (!verify_no_inherited_privileged_descriptor()) {
        return report_probe_failure(82, "inherited privileged descriptor detected");
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        return report_probe_failure(82, "no-new-privileges is not enabled");
    }
    std::uint64_t session_inode{};
    if (!verify_network_namespaces(session_inode)) {
        return report_probe_failure(83, "network namespace identity mismatch");
    }
    if (!verify_session_address()) {
        return report_probe_failure(83, "expected nll-app IPv4 address is missing");
    }
    const char* expected_stdin{required_environment(stdin_variable)};
    std::string stdin_line;
    if (expected_stdin == nullptr || !std::getline(std::cin, stdin_line)
        || stdin_line != expected_stdin) {
        return report_probe_failure(84, "transferred stdin mismatch");
    }
    const bool udp_succeeded{
        perform_udp_exchange(static_cast<std::uint16_t>(port_value), token)};
    if (!udp_succeeded) {
        std::cerr << "Session qualification probe failed: bounded host UDP exchange failed\n";
    }
    if (mode == "hold") {
        struct sigaction action {};
        action.sa_handler = handle_termination;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, nullptr) == -1) {
            return report_probe_failure(86, "could not install SIGTERM handler");
        }
    }
    std::cout << ready_prefix << token << ':' << session_inode << '\n'
              << probe_stdout << udp_result_prefix << token << ':'
              << (udp_succeeded ? "success" : "unavailable") << '\n';
    std::cerr << probe_stderr;
    std::cout.flush();
    std::cerr.flush();
    if (mode == "natural") {
        return 37;
    }
    while (termination_requested == 0) {
        (void)pause();
    }
    const char* marker{required_environment(marker_variable)};
    if (marker == nullptr) {
        return 87;
    }
    const int descriptor{open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)};
    if (descriptor == -1) {
        return 88;
    }
    constexpr char marker_contents[]{"SIGTERM_OBSERVED\n"};
    const ssize_t written{write(descriptor, marker_contents, sizeof(marker_contents) - 1)};
    const int close_result{close(descriptor)};
    return written == static_cast<ssize_t>(sizeof(marker_contents) - 1)
            && close_result == 0
        ? 0
        : 89;
}

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(
        const std::vector<std::pair<std::string, std::string>>& values)
    {
        for (const auto& [name, value] : values) {
            const char* previous{std::getenv(name.c_str())};
            previous_.emplace_back(name, previous == nullptr
                    ? std::optional<std::string>{}
                    : std::optional<std::string>{previous});
            if (setenv(name.c_str(), value.c_str(), 1) == -1) {
                valid_ = false;
                return;
            }
        }
    }

    ~ScopedEnvironment()
    {
        for (auto entry{previous_.rbegin()}; entry != previous_.rend(); ++entry) {
            if (entry->second.has_value()) {
                (void)setenv(entry->first.c_str(), entry->second->c_str(), 1);
            } else {
                (void)unsetenv(entry->first.c_str());
            }
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> previous_;
    bool valid_{true};
};

class SessionDirectory {
public:
    SessionDirectory()
    {
        std::string pattern{"/tmp/netlaglab-session-qualification-XXXXXX"};
        char* created{mkdtemp(pattern.data())};
        if (created == nullptr) {
            return;
        }
        root_ = created;
        runtime_ = root_ / "runtime";
        work_ = root_ / "work";
        std::error_code error;
        valid_ = std::filesystem::create_directory(runtime_, error) && !error;
        error.clear();
        valid_ = valid_ && std::filesystem::create_directory(work_, error) && !error;
        if (valid_) {
            valid_ = chmod(runtime_.c_str(), 0700) == 0
                && chmod(work_.c_str(), 0700) == 0;
        }
    }

    ~SessionDirectory()
    {
        if (!root_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const std::filesystem::path& runtime() const noexcept { return runtime_; }
    [[nodiscard]] const std::filesystem::path& work() const noexcept { return work_; }

private:
    std::filesystem::path root_;
    std::filesystem::path runtime_;
    std::filesystem::path work_;
    bool valid_{};
};

class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() { terminate_and_reap(); }

    [[nodiscard]] bool start(
        const std::vector<std::string>& arguments,
        const std::filesystem::path& working_directory,
        const std::string_view standard_input = {},
        const bool keep_standard_input_open = false)
    {
        int input_pipe[2]{-1, -1};
        int output_pipe[2]{-1, -1};
        int error_pipe[2]{-1, -1};
        if (pipe2(input_pipe, O_CLOEXEC) == -1
            || pipe2(output_pipe, O_CLOEXEC) == -1
            || pipe2(error_pipe, O_CLOEXEC) == -1) {
            close_pipes(input_pipe, output_pipe, error_pipe);
            return false;
        }
        const pid_t child{fork()};
        if (child == -1) {
            close_pipes(input_pipe, output_pipe, error_pipe);
            return false;
        }
        if (child == 0) {
            if (dup2(input_pipe[0], STDIN_FILENO) == -1
                || dup2(output_pipe[1], STDOUT_FILENO) == -1
                || dup2(error_pipe[1], STDERR_FILENO) == -1) {
                _exit(126);
            }
            close_pipes(input_pipe, output_pipe, error_pipe);
            if (chdir(working_directory.c_str()) == -1) {
                _exit(126);
            }
            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);
            for (const std::string& argument : arguments) {
                argv.push_back(const_cast<char*>(argument.c_str()));
            }
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(errno == ENOENT ? 127 : 126);
        }
        pid_ = child;
        input_.reset(input_pipe[1]);
        output_.reset(output_pipe[0]);
        error_.reset(error_pipe[0]);
        (void)fcntl(output_.get(), F_SETFL, O_NONBLOCK);
        (void)fcntl(error_.get(), F_SETFL, O_NONBLOCK);
        (void)close(input_pipe[0]);
        (void)close(output_pipe[1]);
        (void)close(error_pipe[1]);
        if (!standard_input.empty()) {
            std::size_t written{};
            while (written < standard_input.size()) {
                const ssize_t amount{write(
                    input_.get(),
                    standard_input.data() + written,
                    standard_input.size() - written)};
                if (amount > 0) {
                    written += static_cast<std::size_t>(amount);
                } else if (amount == -1 && errno == EINTR) {
                    continue;
                } else {
                    return false;
                }
            }
        }
        if (!keep_standard_input_open) {
            input_.reset();
        }
        return true;
    }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    [[nodiscard]] bool wait_for_ready(
        const int udp_descriptor,
        const std::string_view token,
        std::string& stdout_text,
        std::string& stderr_text,
        std::uint64_t& session_inode,
        bool& host_peer_received_datagram,
        bool& udp_exchange_succeeded,
        const std::chrono::milliseconds timeout = 20000ms)
    {
        const auto deadline{std::chrono::steady_clock::now() + timeout};
        bool udp_seen{};
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd descriptors[3]{{output_.get(), POLLIN, 0},
                                  {error_.get(), POLLIN, 0},
                                  {udp_descriptor, POLLIN, 0}};
            const auto remaining{std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())};
            int polled{};
            do {
                polled = poll(
                    descriptors,
                    3,
                    static_cast<int>(std::max<std::int64_t>(0, remaining.count())));
            } while (polled == -1 && errno == EINTR);
            if (polled <= 0) {
                return false;
            }
            if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0
                && !read_available(output_.get(), stdout_text)) {
                return false;
            }
            if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0
                && !read_available(error_.get(), stderr_text)) {
                return false;
            }
            if ((descriptors[2].revents & POLLIN) != 0) {
                udp_seen = echo_one_datagram(udp_descriptor, token);
                if (!udp_seen) {
                    return false;
                }
                host_peer_received_datagram = true;
            }
            const std::string expected_line{
                std::string{ready_prefix} + std::string{token} + ':'};
            const std::string expected_udp_line{
                std::string{udp_result_prefix} + std::string{token} + ':'};
            const std::size_t ready_at{stdout_text.find(expected_line)};
            const std::size_t udp_result_at{stdout_text.find(expected_udp_line)};
            if (ready_at != std::string::npos
                && stdout_text.find(probe_stdout) != std::string::npos
                && stderr_text.find(probe_stderr) != std::string::npos
                && udp_result_at != std::string::npos) {
                const std::size_t value_start{ready_at + expected_line.size()};
                const std::size_t value_end{stdout_text.find('\n', value_start)};
                const std::size_t udp_value_start{
                    udp_result_at + expected_udp_line.size()};
                const std::size_t udp_value_end{
                    stdout_text.find('\n', udp_value_start)};
                if (value_end == std::string::npos
                    || udp_value_end == std::string::npos) {
                    return false;
                }
                const std::string_view udp_result{
                    stdout_text.data() + udp_value_start,
                    udp_value_end - udp_value_start};
                if (udp_result != "success" && udp_result != "unavailable") {
                    return false;
                }
                udp_exchange_succeeded = udp_result == "success";
                return parse_unsigned(
                        std::string_view{stdout_text}.substr(
                            value_start, value_end - value_start),
                        session_inode);
            }
            if ((descriptors[0].revents & POLLHUP) != 0
                && (descriptors[1].revents & POLLHUP) != 0) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<int> wait_for_exit(
        std::string& stdout_text,
        std::string& stderr_text,
        const std::chrono::milliseconds timeout = 20000ms,
        std::uint64_t* session_namespace_inode = nullptr)
    {
        const auto deadline{std::chrono::steady_clock::now() + timeout};
        while (std::chrono::steady_clock::now() < deadline) {
            (void)read_available(output_.get(), stdout_text);
            (void)read_available(error_.get(), stderr_text);
            if (session_namespace_inode != nullptr
                && *session_namespace_inode == 0) {
                struct stat namespace_status {};
                if (stat("/run/netns/netlaglab", &namespace_status) == 0) {
                    *session_namespace_inode =
                        static_cast<std::uint64_t>(namespace_status.st_ino);
                }
            }
            int status{};
            const pid_t result{waitpid(pid_, &status, WNOHANG)};
            if (result == pid_) {
                pid_ = -1;
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                }
                return 128 + WTERMSIG(status);
            }
            if (result == -1 && errno != EINTR) {
                return std::nullopt;
            }
            (void)poll(nullptr, 0, 10);
        }
        return std::nullopt;
    }

    [[nodiscard]] bool kill_and_reap(const int signal_number)
    {
        if (pid_ <= 0 || kill(pid_, signal_number) == -1) {
            return false;
        }
        int status{};
        pid_t result{};
        do {
            result = waitpid(pid_, &status, 0);
        } while (result == -1 && errno == EINTR);
        if (result != pid_) {
            return false;
        }
        pid_ = -1;
        return WIFSIGNALED(status) && WTERMSIG(status) == signal_number;
    }

private:
    class Descriptor {
    public:
        Descriptor() = default;
        explicit Descriptor(const int value) : value_{value} {}
        ~Descriptor() { reset(); }
        Descriptor(const Descriptor&) = delete;
        Descriptor& operator=(const Descriptor&) = delete;
        Descriptor(Descriptor&& other) noexcept
            : value_{std::exchange(other.value_, -1)}
        {
        }
        Descriptor& operator=(Descriptor&& other) noexcept
        {
            if (this != &other) {
                reset();
                value_ = std::exchange(other.value_, -1);
            }
            return *this;
        }
        [[nodiscard]] int get() const noexcept { return value_; }
        void reset(const int value = -1) noexcept
        {
            if (value_ != -1) {
                (void)close(value_);
            }
            value_ = value;
        }

    private:
        int value_{-1};
    };

    static void close_pipes(int (&input)[2], int (&output)[2], int (&error)[2])
    {
        for (int descriptor : input) {
            if (descriptor != -1) {
                (void)close(descriptor);
            }
        }
        for (int descriptor : output) {
            if (descriptor != -1) {
                (void)close(descriptor);
            }
        }
        for (int descriptor : error) {
            if (descriptor != -1) {
                (void)close(descriptor);
            }
        }
    }

    static bool read_available(const int descriptor, std::string& destination)
    {
        char buffer[4096]{};
        while (true) {
            const ssize_t size{read(descriptor, buffer, sizeof(buffer))};
            if (size > 0) {
                destination.append(buffer, static_cast<std::size_t>(size));
                continue;
            }
            if (size == 0 || (size == -1 && errno == EAGAIN)) {
                return true;
            }
            return size == -1 && errno == EINTR;
        }
    }

    static bool echo_one_datagram(const int descriptor, const std::string_view token)
    {
        char buffer[256]{};
        sockaddr_in peer{};
        socklen_t peer_size{sizeof(peer)};
        const ssize_t size{recvfrom(
            descriptor,
            buffer,
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_size)};
        const std::string expected_request{
            std::string{request_prefix} + std::string{token}};
        in_addr expected_source{};
        if (size != static_cast<ssize_t>(expected_request.size())
            || std::string_view{buffer, static_cast<std::size_t>(size)}
                != expected_request
            || peer.sin_family != AF_INET || peer_size != sizeof(peer)
            || inet_pton(AF_INET, "10.200.0.2", &expected_source) != 1
            || peer.sin_addr.s_addr != expected_source.s_addr) {
            return false;
        }
        const std::string response{
            std::string{response_prefix} + std::string{token}};
        const ssize_t sent{sendto(
            descriptor,
            response.data(),
            response.size(),
            0,
            reinterpret_cast<const sockaddr*>(&peer),
            peer_size)};
        return sent == static_cast<ssize_t>(response.size());
    }

    void terminate_and_reap() noexcept
    {
        if (pid_ <= 0) {
            return;
        }
        (void)kill(pid_, SIGKILL);
        int status{};
        while (waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
        }
        pid_ = -1;
    }

    pid_t pid_{-1};
    Descriptor input_;
    Descriptor output_;
    Descriptor error_;
};

class UdpPeer {
public:
    UdpPeer()
    {
        descriptor_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (descriptor_ == -1) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = 0;
        if (bind(
                descriptor_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == -1) {
            return;
        }
        socklen_t size{sizeof(address)};
        if (getsockname(
                descriptor_, reinterpret_cast<sockaddr*>(&address), &size) == -1) {
            return;
        }
        port_ = ntohs(address.sin_port);
        valid_ = port_ != 0;
    }
    ~UdpPeer()
    {
        if (descriptor_ != -1) {
            (void)close(descriptor_);
        }
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    int descriptor_{-1};
    std::uint16_t port_{};
    bool valid_{};
};

[[nodiscard]] bool namespace_roots_absent(std::uint64_t session_inode)
{
    struct stat namespace_path {};
    if (lstat("/run/netns/netlaglab", &namespace_path) == 0 || errno != ENOENT) {
        return false;
    }
    errno = 0;
    if (if_nametoindex("nll-host") != 0 || errno != ENODEV) {
        return false;
    }

    if (session_inode == 0) {
        return true;
    }
    DIR* processes{opendir("/proc")};
    if (processes == nullptr) {
        return false;
    }
    const std::string expected_link{"net:[" + std::to_string(session_inode) + "]"};
    bool absent{true};
    while (dirent* process{readdir(processes)}) {
        std::uint64_t pid{};
        if (!parse_unsigned(process->d_name, pid)) {
            continue;
        }
        const std::string namespace_link{
            "/proc/" + std::to_string(pid) + "/ns/net"};
        char target[256]{};
        const ssize_t size{readlink(
            namespace_link.c_str(), target, sizeof(target) - 1)};
        if (size == -1 && errno != ENOENT && errno != ESRCH) {
            absent = false;
            break;
        }
        if (size > 0
            && std::string_view{target, static_cast<std::size_t>(size)}
                == expected_link) {
            absent = false;
            break;
        }
        const std::string descriptor_directory{
            "/proc/" + std::to_string(pid) + "/fd"};
        DIR* descriptors{opendir(descriptor_directory.c_str())};
        if (descriptors == nullptr) {
            if (errno != ENOENT && errno != ESRCH) {
                absent = false;
                break;
            }
            continue;
        }
        while (dirent* entry{readdir(descriptors)}) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            const std::string path{
                descriptor_directory + '/' + entry->d_name};
            const ssize_t target_size{readlink(path.c_str(), target, sizeof(target) - 1)};
            if (target_size == -1 && errno != ENOENT && errno != ESRCH) {
                absent = false;
                break;
            }
            if (target_size > 0
                && std::string_view{target, static_cast<std::size_t>(target_size)}
                    == expected_link) {
                absent = false;
                break;
            }
        }
        (void)closedir(descriptors);
        if (!absent) {
            break;
        }
    }
    (void)closedir(processes);
    return absent;
}

[[nodiscard]] bool host_lock_available()
{
    const int descriptor{open(
        "/run/netlaglab/host.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
    if (descriptor == -1) {
        return false;
    }
    const bool locked{flock(descriptor, LOCK_EX | LOCK_NB) == 0};
    if (locked) {
        (void)flock(descriptor, LOCK_UN);
    }
    (void)close(descriptor);
    return locked;
}

[[nodiscard]] bool wait_for_cleanup(
    const std::uint64_t session_inode,
    const std::chrono::milliseconds timeout = 10000ms)
{
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (std::chrono::steady_clock::now() < deadline) {
        if (namespace_roots_absent(session_inode) && host_lock_available()) {
            return true;
        }
        (void)poll(nullptr, 0, 50);
    }
    return false;
}

[[nodiscard]] std::optional<std::uint64_t> current_namespace_inode()
{
    struct stat status {};
    if (stat("/proc/self/ns/net", &status) == -1) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(status.st_ino);
}

class Scenario {
public:
    Scenario() : token_{"qualification-" + std::to_string(getpid())}
    {
        if (!directory_.valid() || !peer_.valid()) {
            return;
        }
        const std::optional<std::uint64_t> host_inode{current_namespace_inode()};
        if (!host_inode.has_value()) {
            return;
        }
        host_inode_ = *host_inode;
        marker_ = directory_.root() / "sigterm.marker";
        const std::vector<std::pair<std::string, std::string>> values{
            {"XDG_RUNTIME_DIR", directory_.runtime().string()},
            {std::string{token_variable}, token_},
            {std::string{uid_variable}, std::to_string(getuid())},
            {std::string{gid_variable}, std::to_string(getgid())},
            {std::string{groups_variable}, make_group_list()},
            {std::string{cwd_variable}, directory_.work().string()},
            {std::string{host_namespace_variable}, std::to_string(host_inode_)},
            {std::string{udp_port_variable}, std::to_string(peer_.port())},
            {std::string{marker_variable}, marker_.string()},
            {std::string{stdin_variable}, std::string{stdin_value}},
        };
        environment_ = std::make_unique<ScopedEnvironment>(values);
        valid_ = environment_->valid();
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& token() const noexcept { return token_; }
    [[nodiscard]] std::uint64_t host_inode() const noexcept { return host_inode_; }
    [[nodiscard]] const std::filesystem::path& work_directory() const noexcept
    {
        return directory_.work();
    }
    [[nodiscard]] const std::filesystem::path& signal_marker() const noexcept
    {
        return marker_;
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return peer_.port(); }
    [[nodiscard]] int udp_descriptor() const noexcept { return peer_.descriptor(); }

    [[nodiscard]] std::vector<std::string> run_arguments(
        const std::string_view mode) const
    {
        return {NETLAGLAB_EXECUTABLE_PATH,
                "run",
                "--",
                NETLAGLAB_SESSION_PROBE_PATH,
                "--probe",
                std::string{mode},
                std::to_string(peer_.port()),
                token_};
    }

    [[nodiscard]] std::vector<std::string> failed_exec_arguments() const
    {
        return {NETLAGLAB_EXECUTABLE_PATH,
                "run",
                "--",
                "/netlaglab-session-qualification/nonexistent-program"};
    }

    [[nodiscard]] bool launch_and_wait_for_probe(
        ChildProcess& process,
        const std::string_view mode,
        std::string& stdout_text,
        std::string& stderr_text,
        std::uint64_t& session_inode,
        bool& host_peer_received_datagram,
        bool& udp_exchange_succeeded) const
    {
        if (!process.start(
                run_arguments(mode), work_directory(), std::string{stdin_value} + "\n")) {
            return false;
        }
        return process.wait_for_ready(
            udp_descriptor(),
            token_,
            stdout_text,
            stderr_text,
            session_inode,
            host_peer_received_datagram,
            udp_exchange_succeeded);
    }

private:
    SessionDirectory directory_;
    UdpPeer peer_;
    std::string token_;
    std::filesystem::path marker_;
    std::uint64_t host_inode_{};
    std::unique_ptr<ScopedEnvironment> environment_;
    bool valid_{};
};

[[nodiscard]] bool has_authorization()
{
    const char* value{required_environment(authorization_variable)};
    return value != nullptr && std::string_view{value} == "1";
}

[[nodiscard]] bool wait_for_signal_marker(const std::filesystem::path& marker)
{
    const auto deadline{std::chrono::steady_clock::now() + 10s};
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input{marker};
        std::string contents;
        std::getline(input, contents);
        if (contents == "SIGTERM_OBSERVED") {
            return true;
        }
        (void)poll(nullptr, 0, 25);
    }
    return false;
}

[[nodiscard]] std::optional<int> run_controller_stop(
    const std::filesystem::path& working_directory,
    std::string& stdout_text,
    std::string& stderr_text)
{
    const char* runtime_directory{std::getenv("XDG_RUNTIME_DIR")};
    if (runtime_directory == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path control_socket{
        std::filesystem::path{runtime_directory} / "netlaglab" / "control.sock"};
    const auto deadline{std::chrono::steady_clock::now() + 5s};
    struct stat status {};
    while (std::chrono::steady_clock::now() < deadline
           && lstat(control_socket.c_str(), &status) == -1) {
        (void)poll(nullptr, 0, 10);
    }
    if (!S_ISSOCK(status.st_mode)) {
        return std::nullopt;
    }
    ChildProcess controller;
    if (!controller.start(
            {NETLAGLAB_EXECUTABLE_PATH, "attach"},
            working_directory,
            "stop\n",
            true)) {
        return std::nullopt;
    }
    return controller.wait_for_exit(stdout_text, stderr_text, 15000ms);
}

TEST(SessionNetworkQualification, NaturalExitPreservesContextUdpAndCleanup)
{
    if (!has_authorization()) {
        GTEST_SKIP() << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 only for an explicitly "
                        "authorized local networking qualification";
    }
    if (geteuid() != 0) {
        GTEST_SKIP() << "run this CTest directly as root; the qualification harness "
                        "does not invoke sudo";
    }
    Scenario scenario;
    ASSERT_TRUE(scenario.valid());
    ChildProcess session;
    std::string output;
    std::string error;
    std::uint64_t session_inode{};
    bool host_peer_received_datagram{};
    bool udp_exchange_succeeded{};
    ASSERT_TRUE(scenario.launch_and_wait_for_probe(
        session,
        "natural",
        output,
        error,
        session_inode,
        host_peer_received_datagram,
        udp_exchange_succeeded))
        << "Workload did not report its namespace, context, and UDP probe result\n"
        << error << output;

    const std::optional<int> status{session.wait_for_exit(output, error)};
    ASSERT_TRUE(status.has_value()) << "Session did not finish naturally\n" << error;
    ASSERT_EQ(*status, 37) << error;
    ASSERT_TRUE(wait_for_cleanup(session_inode))
        << "owned namespace/veth roots or the host lock remained after natural exit";
    if (!udp_exchange_succeeded) {
        GTEST_SKIP() << "host UDP peer received no reply from the Session; the packet "
                        "was observed at nll-host, so UDP qualification is limited by "
                        "this host's receive path";
    }
    EXPECT_TRUE(host_peer_received_datagram)
        << "Workload received an echo but the host peer did not report its request";
}

TEST(SessionNetworkQualification, ControllerStopReapsWorkloadBeforeCleanup)
{
    if (!has_authorization()) {
        GTEST_SKIP() << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 only for an explicitly "
                        "authorized local networking qualification";
    }
    if (geteuid() != 0) {
        GTEST_SKIP() << "run this CTest directly as root; the qualification harness "
                        "does not invoke sudo";
    }
    Scenario scenario;
    ASSERT_TRUE(scenario.valid());
    ChildProcess session;
    std::string session_output;
    std::string session_error;
    std::uint64_t session_inode{};
    bool host_peer_received_datagram{};
    bool udp_exchange_succeeded{};
    ASSERT_TRUE(scenario.launch_and_wait_for_probe(
        session,
        "hold",
        session_output,
        session_error,
        session_inode,
        host_peer_received_datagram,
        udp_exchange_succeeded))
        << "Workload did not report its namespace, context, and UDP probe result\n"
        << session_error << session_output;
    EXPECT_TRUE(!udp_exchange_succeeded || host_peer_received_datagram)
        << "Workload received a UDP echo that the host peer did not observe";

    std::string controller_output;
    std::string controller_error;
    const std::optional<int> controller_status{run_controller_stop(
        scenario.work_directory(), controller_output, controller_error)};
    ASSERT_TRUE(controller_status.has_value())
        << "attach did not complete after sending stop\n" << controller_error;
    EXPECT_EQ(*controller_status, 0) << controller_error;
    EXPECT_NE(
        controller_output.find("Workload exit code: 0"), std::string::npos)
        << controller_output << controller_error;
    EXPECT_TRUE(wait_for_signal_marker(scenario.signal_marker()))
        << "the directly managed Workload did not observe SIGTERM";

    const std::optional<int> session_status{
        session.wait_for_exit(session_output, session_error)};
    ASSERT_TRUE(session_status.has_value()) << session_error;
    EXPECT_EQ(*session_status, 0) << session_error;
    EXPECT_TRUE(wait_for_cleanup(session_inode))
        << "owned resources or the host lock remained after Controller stop";
}

TEST(SessionNetworkQualification, SupervisorLossStopsWorkloadAndCleansTopology)
{
    if (!has_authorization()) {
        GTEST_SKIP() << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 only for an explicitly "
                        "authorized local networking qualification";
    }
    if (geteuid() != 0) {
        GTEST_SKIP() << "run this CTest directly as root; the qualification harness "
                        "does not invoke sudo";
    }
    Scenario scenario;
    ASSERT_TRUE(scenario.valid());
    ChildProcess session;
    std::string output;
    std::string error;
    std::uint64_t session_inode{};
    bool host_peer_received_datagram{};
    bool udp_exchange_succeeded{};
    ASSERT_TRUE(scenario.launch_and_wait_for_probe(
        session,
        "hold",
        output,
        error,
        session_inode,
        host_peer_received_datagram,
        udp_exchange_succeeded))
        << "Workload did not report its namespace, context, and UDP probe result\n"
        << error << output;
    EXPECT_TRUE(!udp_exchange_succeeded || host_peer_received_datagram)
        << "Workload received a UDP echo that the host peer did not observe";

    ASSERT_TRUE(session.kill_and_reap(SIGKILL));
    EXPECT_TRUE(wait_for_signal_marker(scenario.signal_marker()))
        << "helper did not stop the Workload after Supervisor loss";
    EXPECT_TRUE(wait_for_cleanup(session_inode))
        << "owned resources or the host lock remained after Supervisor loss";
}

TEST(SessionNetworkQualification, FailedExecPreservesLookupStatusAndCleansTopology)
{
    if (!has_authorization()) {
        GTEST_SKIP() << "set NETLAGLAB_ALLOW_PRIVILEGED_TESTS=1 only for an explicitly "
                        "authorized local networking qualification";
    }
    if (geteuid() != 0) {
        GTEST_SKIP() << "run this CTest directly as root; the qualification harness "
                        "does not invoke sudo";
    }
    Scenario scenario;
    ASSERT_TRUE(scenario.valid());
    ChildProcess session;
    ASSERT_TRUE(session.start(
        scenario.failed_exec_arguments(), scenario.work_directory()));
    std::string output;
    std::string error;
    std::uint64_t session_inode{};
    const std::optional<int> status{
        session.wait_for_exit(output, error, 30000ms, &session_inode)};
    ASSERT_TRUE(status.has_value()) << "failed-exec Session did not finish\n" << error;
    EXPECT_EQ(*status, 127) << error;
    EXPECT_NE(session_inode, 0U)
        << "could not observe the owned Session namespace during preparation";
    EXPECT_TRUE(wait_for_cleanup(session_inode))
        << "owned namespace/veth roots or the host lock remained after failed exec";
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1 && std::string_view{argv[1]} == "--probe") {
        return run_workload_probe(argc, argv);
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
