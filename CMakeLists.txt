cmake_minimum_required(VERSION 3.20)

project(NetLagLab VERSION 0.1.0 LANGUAGES CXX)

add_library(netlaglab_core src/network_profile.cpp)

target_include_directories(netlaglab_core PUBLIC include)
target_compile_features(netlaglab_core PUBLIC cxx_std_20)
set_target_properties(netlaglab_core PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netlaglab_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(netlaglab src/main.cpp)

target_compile_features(netlaglab PRIVATE cxx_std_20)
target_compile_definitions(netlaglab PRIVATE NETLAGLAB_VERSION="${PROJECT_VERSION}")
target_link_libraries(netlaglab PRIVATE netlaglab_core)
set_target_properties(netlaglab PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netlaglab PRIVATE -Wall -Wextra -Wpedantic)
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
endif()
