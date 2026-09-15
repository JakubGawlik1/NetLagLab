#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

namespace netlaglab {

class SocketPathOwner {
public:
    SocketPathOwner(
        int session_directory_descriptor,
        std::string_view socket_name,
        const std::string& socket_path);

    ~SocketPathOwner();

    SocketPathOwner(const SocketPathOwner&) = delete;
    SocketPathOwner& operator=(const SocketPathOwner&) = delete;

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] bool remove_stale(std::ostream& error) const;

    void mark_owned();
    [[nodiscard]] int remove_owned();

private:
    int session_directory_descriptor_;
    const std::string socket_name_;
    const std::string socket_path_;
    bool owned_{false};
};

[[nodiscard]] int create_listening_socket(
    int session_directory_descriptor,
    SocketPathOwner& socket_path_owner,
    std::ostream& error);

} // namespace netlaglab
