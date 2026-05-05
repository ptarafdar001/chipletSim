cmake_minimum_required(VERSION 3.14)
project(HexaMeshMapper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -fsanitize=address -fsanitize=undefined")

add_compile_options(
  -Wall -Wextra -Wpedantic
  -Wno-unused-parameter
)

# Include path
include_directories(${CMAKE_SOURCE_DIR}/include)

# Main executable
add_executable(hexamesh_mapper src/main.cpp)

# If compiling with g++ < 9, link libstdc++fs
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(hexamesh_mapper stdc++fs)
endif()

# Output binary to project root
set_target_properties(hexamesh_mapper PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
