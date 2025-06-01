cmake_minimum_required(VERSION 3.20)
project(Tutorial-Event LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(USE_JEMALLOC "Use jemalloc instead of system malloc" OFF)
option(BUILD_EXAMPLES "Compile project with examples" ON)
option(BUILD_VERBOSE "Compile project in verbose mode" OFF)
option(BUILD_DEBG "Compile project in DEBUG mode" OFF)

# Optional malloc (example, adjust as needed)
if (USE_JEMALLOC)
    find_library(JEMALLOC_LIB jemalloc REQUIRED)
    add_compile_definitions(USE_JEMALLOC)
endif()

# Global compile flags
set(CXX_FLAGS
    -Wall
    -Wextra
    -Werror
    -Wold-style-cast
    -Woverloaded-virtual
    -Wpointer-arith
    -Wshadow
    -Wno-unused-parameter
    -Wwrite-strings
    -march=native
    -rdynamic
    -pedantic
    -stdlib=libc++ # Use libc++ for C++ standard library
    )

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "using gcc")
    #GCC-Specific flags
    add_compile_options(-Wpedantic)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "using clang")
    add_compile_options(-Wpendantic -Wno-unknown-warning-option)
endif()

set(CMAKE_VERBOSE_MAKEFILE OFF)

# Add subdirectories
add_subdirectory(reactor)
add_subdirectory(logging)
add_subdirectory(utilities)
add_subdirectory(examples)

# Add libriary

# Executables
# add_executable(backoff_main backoff_main.cpp)
# target_link_libraries(backoff_main PRIVATE cow_reactor cow_logging cow_utilities)

# Executables
# add_executable(reactor_benchmark reactor_benchmark.cpp)
# target_link_libraries(reactor_benchmark PRIVATE cow_reactor cow_logging)
# target_compile_options(reactor_benchmark PRIVATE -pthread)

# add_executable(echo_main echo_main.cpp)
# target_link_libraries(echo_main PRIVATE cow_reactor)
