#pragma once

#include <iosfwd>
#include <sys/types.h>

namespace netlaglab {

[[nodiscard]] bool validate_runtime_directory(
    int descriptor,
    uid_t expected_owner,
    std::ostream& error);

[[nodiscard]] bool validate_session_directory(
    int descriptor,
    uid_t expected_owner,
    std::ostream& error);

} // namespace netlaglab
