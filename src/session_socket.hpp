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

    [[nodiscard]] bool remove_stale(std::ostream& error) const;
    [[nodiscard]] int create_listening_socket(std::ostream& error);
    [[nodiscard]] int remove_owned();

private:
    int session_directory_descriptor_;
    const std::string socket_name_;
    const std::string socket_path_;
    bool owned_{false};
};

} // namespace netlaglab
