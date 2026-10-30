cmake_minimum_required(VERSION 3.20)
project(Tutorial-Event LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(USE_JEMALLOC "Use jemalloc instead of system malloc" OFF)
option(BUILD_EXAMPLES "Compile project with examples" OFF)
option(BUILD_TESTS "To enable compile tests" OFF)

# Optional, feature-gated third-party integrations. Default OFF so a plain
# build pulls in no extra dependencies. See third_party/README.md.
option(COX_WITH_HTTP2 "Enable HTTP/2 support via nghttp2" OFF)

# Project-local CMake modules (cox_add_vendored, ...).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR/cmake}")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

# Optional malloc (example, adjust as needed)
if (USE_JEMALLOC)
    find_library(JEMALLOC_LIB jemalloc REQUIRED)
    add_compile_definitions(USE_JEMALLOC)
endif()

# Global warning flags. These are APPLIED (see add_compile_options below) --
# they were previously declared here but never used, which silently disabled the
# project's own warning policy.
#
# Deliberately warnings-only: -march=native (breaks portable binaries),
# -rdynamic (link-time, for backtraces) and -stdlib=libc++ (ABI choice) are NOT
# included here; add them per-target or via a toolchain file if wanted.
set(COX_WARNING_FLAGS
    -Wall
    -Wextra
    -Wold-style-cast
    -Woverloaded-virtual
    -Wpointer-arith
    -Wshadow
    -Wno-unused-parameter   # Callback signatures often ignore arguments
    -Wwrite-strings
    )

# Treat warnings as errors by default so the policy cannot silently rot again.
# Disable for a one-off build with -DCOX_WERROR=OFF.
option(COX_WERROR "Treat compiler warnings as errors" ON)
if (COX_WERROR)
    list(APPEND COX_WARNING_FLAGS -Werror)
endif()

add_compile_options(${COX_WARNING_FLAGS})

# ---- Sanitizers ----
#
# COX_SANITIZE=address|thread|undefined|leak|OFF
#
# Sanitizers are the highest-value testing tool for this codebase: connection
# lifetimes are shared_ptr-managed, work is marshalled across threads with
# runInLoop, AsyncSink hands buffers to a writer thread, and raw sockaddr casts
# are everywhere. A passing test proves behavior, NOT the absence of a data race
# or a use-after-free -- only these tools do.
#
# Mutually exclusive by design: ASan and TSan cannot be combined (their runtimes
# conflict), so this is a single-valued option rather than a set of booleans
# Requires -O1 and no omitted frame pointers for usable stack traces, and
# -fno-optimize-sibling-calls so tail calls do not eat frames.
set(COX_SANITIZE "OFF" CACHE STRING
    "Enable a sanitizer: OFF, address, thread, undefined, leak")

set_property(CACHE COX_SANITIZE PROPERTY STRINGS
    OFF address thread undefined leak)

if (NOT COX_SANITIZE STREQUAL "OFF")
    set(COX_SAN_FLAGS
        -fsanitize=${COX_SANITIZE}
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -g
    )
    if (COX_SANITIZE STREQUAL "address")
        # Catch use-after-scope too; cheap and finds real bugs.
        list(APPEND COX_SAN_FLAGS -fsanitize-address-use-after-scope)
    elseif (COX_SANITIZE STREQUAL "undefined")
        # Make UBSan failures fatal; by default it only warns, which is easy to
        # miss in CI output.
        list(APPEND COX_SAN_FLAGS -fno-sanitize-recover=all)
    endif()

    add_compile_options(${COX_SAN_FLAGS})
    add_link_options(-fsanitize=${COX_SANITIZE})
    message(STATUS "sanitizer enabled: ${COX_SANITIZE}")
endif()


if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "using gcc")
    add_compile_options(-Wpedantic)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "using clang")
    add_compile_options(-Wpedantic -Wno-unknown-warning-option)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_VERBOSE_MAKEFILE ON)
    add_compile_options( -g -O0 -DDEBUG)
else()
    set(CMAKE_VERBOSE_MAKEFILE OFF)
endif()
# Set output directories

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Optional async DNS via c-ares. When found, net/ compiles the CaresResolver
# and COX_HAVE_CARES is defined; otherwise only the ThreadPool getaddrinfo
# resolver is available (always present).
find_path(CARES_INCLUDE_DIR ares.h)
find_library(CARES_LIB cares)

if (CARES_INCLUDE_DIR AND CARES_LIB)
    set(COX_HAVE_CARES ON)
    message(STATUS "c-ares found: async DNS resolver enabled")
else()
    set(COX_HAVE_CARES OFF)
    message(STATUS "c-ares not found: using ThreadPool getaddrinfo resolver")
endif()

# Add subdirectories
add_subdirectory(reactor)
add_subdirectory(logging)
add_subdirectory(utilities)
add_subdirectory(buffer)
add_subdirectory(concurrent)
add_subdirectory(flowcontrol)
add_subdirectory(fswatch)
add_subdirectory(net)
add_subdirectory(tls)
add_subdirectory(http)

if (BUILD_TESTS STREQUAL ON)
    # Register tests with CTest so the whole suite runs via `ctest`.
    enable_testing()
endif()

# Wire in third-party dependencies from one place (see third_party/README.md).
set(COX_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
add_subdirectory(third_party)

if (BUILD_TESTS STREQUAL ON)
    add_subdirectory(tests)
endif()

if (BUILD_EXAMPLES STREQUAL ON)
    # Add examples
    add_subdirectory(examples)
endif()
