#pragma once

#include <iosfwd>

namespace netlaglab {

int run_session(char* const child_arguments[], std::ostream& error);

} // namespace netlaglab
