cmake_minimum_required(VERSION 3.16)
project(Cow CXX)

# Compiler & Flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    )

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Using Clang/Apple-Clang")
    list(APPEND CXX_FLAGS "-Wno-null-dereference")
    list(APPEND CXX_FLAGS "-Wno-sign-conversion")
    list(APPEND CXX_FLAGS "-Wno-unused-local-typedef")
    list(APPEND CXX_FLAGS "-Wthread-safety")
    list(REMOVE_ITEM CXX_FLAGS "-rdynamic")
else()
    message(FATAL_ERROR "Only Clang/Apple-Clang is supported")
endif()


if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CXX_FLAGS} -O1 -g")
else()
    set(CMAKE_CXX_FLAGS "${CXX_FLAGS} -O3")
endif()

# Memory allocator option
option(USE_JEMALLOC "Use jemalloc instead of tcmalloc" OFF)

if (USE_JEMALLOC)
    find_library(JEMALLOC_LIB jemalloc REQUIRED)
    set(ALLOC_LIB ${JEMALLOC_LIB})
else()
    # TODO enable later
    # find_library(TCMALLOC_LIB tcmalloc REQUIRED)
    set(ALLOC_LIB ${TCMALLOC_LIB})
endif()

# Subdirectories
add_subdirectory(framework)
add_subdirectory(resourceMonitor)
add_subdirectory(examples)
add_subdirectory(docs)

# Place all compiled libraries and binaries into build/bin and build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)