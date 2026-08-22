cmake_minimum_required(VERSION 3.20)

project(NetLagLab VERSION 0.1.0 LANGUAGES CXX)

add_executable(netlaglab src/main.cpp)

target_compile_features(netlaglab PRIVATE cxx_std_20)
target_compile_definitions(netlaglab PRIVATE NETLAGLAB_VERSION="${PROJECT_VERSION}")
set_target_properties(netlaglab PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netlaglab PRIVATE -Wall -Wextra -Wpedantic)
endif()
