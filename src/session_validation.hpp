#pragma once

#include "file_descriptor.hpp"

#include <iosfwd>
#include <optional>
#include <string>
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

[[nodiscard]] std::optional<FileDescriptor> open_and_validate_runtime_directory(
    const std::string& path,
    uid_t expected_owner,
    std::ostream& error);

[[nodiscard]] std::optional<FileDescriptor> open_and_validate_session_directory(
    int runtime_directory_descriptor,
    uid_t expected_owner,
    std::ostream& error);

} // namespace netlaglab
