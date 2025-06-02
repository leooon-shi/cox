cmake_minimum_required(VERSION 3.20)
project(Tutorial-Event LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(USE_JEMALLOC "Use jemalloc instead of system malloc" OFF)
option(BUILD_EXAMPLES "Compile project with examples" OFF)
option(BUILD_TESTS "To enable compile tests" OFF)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

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
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_VERBOSE_MAKEFILE ON)
    add_compile_options( -g -O0 -DDEBUG)
else()
    set(CMAKE_VERBOSE_MAKEFILE OFF)
endif()
# Set output directories
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS}")
# set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add subdirectories
add_subdirectory(reactor)
add_subdirectory(logging)
add_subdirectory(utilities)

if (BUILD_TESTS STREQUAL ON)
    # Add Google Test
    add_subdirectory(googletest)
    add_subdirectory(tests)
endif()

if (BUILD_EXAMPLES STREQUAL ON)
    # Add examples
    add_subdirectory(examples)
endif()
