#pragma once

#include <iosfwd>

namespace netlaglab {

int attach_to_session(std::ostream& output, std::ostream& error);
int run_session(char* const child_arguments[], std::ostream& error);

} // namespace netlaglab
