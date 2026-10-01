cmake_minimum_required(VERSION 3.20)

project(NetLagLab VERSION 0.1.0 LANGUAGES CXX)

add_library(netlaglab_core src/network_profile.cpp)

target_include_directories(netlaglab_core PUBLIC include)
target_compile_features(netlaglab_core PUBLIC cxx_std_20)
set_target_properties(netlaglab_core PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netlaglab_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(
    netlaglab_network_environment
    STATIC
        src/network_environment/command_runner.cpp
)

target_include_directories(netlaglab_network_environment PRIVATE src)
target_compile_features(netlaglab_network_environment PUBLIC cxx_std_20)
set_target_properties(
    netlaglab_network_environment
    PROPERTIES CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        netlaglab_network_environment
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

add_executable(
    netlaglab
    src/attach_client.cpp
    src/attach_session_presentation.cpp
    src/control_plane_action_executor.cpp
    src/controller_control_plane.cpp
    src/controller_session_outcome.cpp
    src/helper_process.cpp
    src/helper_protocol.cpp
    src/main.cpp
    src/process_status.cpp
    src/session.cpp
    src/session_lifecycle.cpp
    src/session_presentation.cpp
    src/session_paths.cpp
    src/socket_io.cpp
    src/session_socket.cpp
    src/session_validation.cpp
    src/workload_context.cpp
)

target_compile_features(netlaglab PRIVATE cxx_std_20)
target_compile_definitions(netlaglab PRIVATE NETLAGLAB_VERSION="${PROJECT_VERSION}")
target_link_libraries(netlaglab PRIVATE netlaglab_core)
set_target_properties(netlaglab PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netlaglab PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(
    netlaglab-helper
    src/helper_protocol.cpp
    src/helper_main.cpp
    src/process_status.cpp
    src/session_socket.cpp
    src/session_validation.cpp
    src/socket_io.cpp
    src/workload_context.cpp
    src/workload_process.cpp
)

target_compile_features(netlaglab-helper PRIVATE cxx_std_20)
target_link_libraries(netlaglab-helper PRIVATE netlaglab_core)
set_target_properties(netlaglab-helper PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netlaglab-helper PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(netlaglab_core_tests tests/network_profile_test.cpp)
    target_link_libraries(
        netlaglab_core_tests
        PRIVATE
            netlaglab_core
            GTest::gtest_main
    )
    set_target_properties(netlaglab_core_tests PROPERTIES CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_core_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    include(GoogleTest)
    gtest_discover_tests(netlaglab_core_tests)

    add_executable(
        netlaglab-network-command-probe
        tests/network_command_probe.cpp
    )
    target_compile_features(
        netlaglab-network-command-probe
        PRIVATE cxx_std_20
    )
    set_target_properties(
        netlaglab-network-command-probe
        PROPERTIES CXX_EXTENSIONS OFF
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab-network-command-probe
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    add_executable(
        netlaglab_network_environment_tests
        tests/network_command_runner_test.cpp
    )
    target_include_directories(
        netlaglab_network_environment_tests
        PRIVATE src
    )
    target_link_libraries(
        netlaglab_network_environment_tests
        PRIVATE
            netlaglab_network_environment
            GTest::gtest_main
    )
    target_compile_definitions(
        netlaglab_network_environment_tests
        PRIVATE
            NETWORK_COMMAND_PROBE_PATH="$<TARGET_FILE:netlaglab-network-command-probe>"
    )
    add_dependencies(
        netlaglab_network_environment_tests
        netlaglab-network-command-probe
    )
    target_compile_features(
        netlaglab_network_environment_tests
        PRIVATE cxx_std_20
    )
    set_target_properties(
        netlaglab_network_environment_tests
        PROPERTIES CXX_EXTENSIONS OFF
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_network_environment_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    gtest_discover_tests(netlaglab_network_environment_tests)

    add_executable(
        netlaglab_lifecycle_tests
        src/controller_session_outcome.cpp
        src/session_lifecycle.cpp
        src/session_presentation.cpp
        src/socket_io.cpp
        tests/session_lifecycle_test.cpp
    )
    target_include_directories(netlaglab_lifecycle_tests PRIVATE src)
    target_link_libraries(netlaglab_lifecycle_tests PRIVATE GTest::gtest_main)
    target_compile_features(netlaglab_lifecycle_tests PRIVATE cxx_std_20)
    set_target_properties(netlaglab_lifecycle_tests PROPERTIES CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_lifecycle_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    gtest_discover_tests(netlaglab_lifecycle_tests)

    add_executable(
        netlaglab_protocol_tests
        src/helper_protocol.cpp
        src/workload_context.cpp
        tests/helper_protocol_test.cpp
        tests/workload_context_test.cpp
    )
    target_include_directories(netlaglab_protocol_tests PRIVATE src)
    target_link_libraries(
        netlaglab_protocol_tests
        PRIVATE
            netlaglab_core
            GTest::gtest_main
    )
    target_compile_features(netlaglab_protocol_tests PRIVATE cxx_std_20)
    set_target_properties(netlaglab_protocol_tests PROPERTIES CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_protocol_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    gtest_discover_tests(netlaglab_protocol_tests)

    add_executable(
        netlaglab_controller_protocol_tests
        src/control_plane_action_executor.cpp
        src/controller_control_plane.cpp
        src/helper_protocol.cpp
        src/socket_io.cpp
        src/workload_context.cpp
        tests/controller_control_plane_test.cpp
    )
    target_include_directories(netlaglab_controller_protocol_tests PRIVATE src)
    target_link_libraries(
        netlaglab_controller_protocol_tests
        PRIVATE
            netlaglab_core
            GTest::gtest_main
    )
    target_compile_features(netlaglab_controller_protocol_tests PRIVATE cxx_std_20)
    set_target_properties(
        netlaglab_controller_protocol_tests
        PROPERTIES CXX_EXTENSIONS OFF
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_controller_protocol_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    gtest_discover_tests(netlaglab_controller_protocol_tests)

    add_executable(
        netlaglab_controller_outcome_tests
        src/attach_client.cpp
        src/attach_session_presentation.cpp
        src/controller_session_outcome.cpp
        src/session_paths.cpp
        src/session_lifecycle.cpp
        src/session_presentation.cpp
        src/session_socket.cpp
        src/session_validation.cpp
        src/socket_io.cpp
        tests/attach_client_test.cpp
        tests/controller_session_outcome_test.cpp
    )
    target_include_directories(netlaglab_controller_outcome_tests PRIVATE src)
    target_link_libraries(
        netlaglab_controller_outcome_tests
        PRIVATE GTest::gtest_main
    )
    target_compile_features(netlaglab_controller_outcome_tests PRIVATE cxx_std_20)
    set_target_properties(
        netlaglab_controller_outcome_tests
        PROPERTIES CXX_EXTENSIONS OFF
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_controller_outcome_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    gtest_discover_tests(netlaglab_controller_outcome_tests)

    add_executable(netlaglab-workload-probe tests/workload_probe.cpp)
    target_compile_features(netlaglab-workload-probe PRIVATE cxx_std_20)
    set_target_properties(netlaglab-workload-probe PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab-workload-probe
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    add_executable(
        netlaglab_process_tests
        src/process_status.cpp
        src/workload_context.cpp
        src/workload_process.cpp
        src/socket_io.cpp
        tests/workload_process_test.cpp
        tests/process_status_test.cpp
    )
    target_include_directories(netlaglab_process_tests PRIVATE src)
    target_link_libraries(netlaglab_process_tests PRIVATE GTest::gtest_main)
    target_compile_definitions(
        netlaglab_process_tests
        PRIVATE WORKLOAD_PROBE_PATH="$<TARGET_FILE:netlaglab-workload-probe>"
    )
    add_dependencies(netlaglab_process_tests netlaglab-workload-probe)
    target_compile_features(netlaglab_process_tests PRIVATE cxx_std_20)
    set_target_properties(netlaglab_process_tests PROPERTIES CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            netlaglab_process_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    gtest_discover_tests(netlaglab_process_tests)
endif()
