#include "session_paths.hpp"

#include <cstdlib>
#include <ostream>

namespace netlaglab {

std::optional<SessionPaths> make_session_paths_from_environment(std::ostream& error)
{
    const char* const runtime_path{std::getenv("XDG_RUNTIME_DIR")};
    if (runtime_path == nullptr || runtime_path[0] == '\0') {
        error << "NetLagLab: XDG_RUNTIME_DIR is not set\n";
        return std::nullopt;
    }

    if (runtime_path[0] != '/') {
        error << "NetLagLab: XDG_RUNTIME_DIR must be an absolute path\n";
        return std::nullopt;
    }

    return SessionPaths{runtime_path};
}

} // namespace netlaglab
