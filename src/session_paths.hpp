#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace netlaglab {

class SessionPaths {
public:
    static constexpr std::string_view session_directory_name{"netlaglab"};
    static constexpr std::string_view lock_file_name{"session.lock"};
    static constexpr std::string_view control_socket_name{"control.sock"};
    static constexpr std::string_view helper_socket_name{"helper.sock"};

    explicit SessionPaths(std::string xdg_runtime_directory)
        : xdg_runtime_directory_{std::move(xdg_runtime_directory)},
          session_directory_{join(xdg_runtime_directory_, session_directory_name)},
          lock_file_{join(session_directory_, lock_file_name)},
          control_socket_{join(session_directory_, control_socket_name)},
          helper_socket_{join(session_directory_, helper_socket_name)}
    {
    }

    [[nodiscard]] const std::string& xdg_runtime_directory() const noexcept
    {
        return xdg_runtime_directory_;
    }

    [[nodiscard]] const std::string& session_directory() const noexcept
    {
        return session_directory_;
    }

    [[nodiscard]] const std::string& lock_file() const noexcept
    {
        return lock_file_;
    }

    [[nodiscard]] const std::string& control_socket() const noexcept
    {
        return control_socket_;
    }

    [[nodiscard]] const std::string& helper_socket() const noexcept
    {
        return helper_socket_;
    }

private:
    [[nodiscard]] static std::string join(
        const std::string_view parent,
        const std::string_view child)
    {
        std::string path{parent};
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        path.append(child);
        return path;
    }

    std::string xdg_runtime_directory_;
    std::string session_directory_;
    std::string lock_file_;
    std::string control_socket_;
    std::string helper_socket_;
};

[[nodiscard]] std::optional<SessionPaths> make_session_paths_from_environment(
    std::ostream& error);

} // namespace netlaglab
