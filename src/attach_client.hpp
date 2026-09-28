#pragma once

#include <iosfwd>

namespace netlaglab {

int run_attached_controller(
    int socket_descriptor,
    std::ostream& output,
    std::ostream& error);
int attach_to_session(std::ostream& output, std::ostream& error);

} // namespace netlaglab
