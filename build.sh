#!/bin/bash
#
# Convenience wrapper around CMake presets (see CMakePresets.json).
#
# Build configuration lives in the presets, not here, so that CI, this script
# and a manual `cmake --preset ...` all produce identical builds. Each preset
# uses its own directory under build/, so configurations no longer clobber one
# another and rebuilds are incremental (this script used to `rm -rf` the build
# directory on every invocation).

set -euo pipefail

BUILD_TYPE="debug"
COMPILER="clang"
SANITIZER=""
BUILD_EXAMPLES="OFF"
RUN_TESTS="OFF"
CLEAN="OFF"

print_usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

Options:
    --debug             Debug build (default)
    --release           Release build
    --gcc               Build with g++ (default: clang++)
    --clang             Build with clang++
    --asan              AddressSanitizer + LeakSanitizer build
    --tsan              ThreadSanitizer build
    --ubsan             UndefinedBehaviorSanitizer build
    --build-examples    Also build examples/benchmarks
    --test              Run the test suite (ctest) after building
    --clean             Delete this configuration's build dir first
    -h, --help          Show this help

Examples:
    ./build.sh --release --test
    ./build.sh --tsan --test            #hunt data races
    ./build.sh --gcc --release --test   #GCC sees warnings clang does not

Equivalent raw commands (CI uses these directly):
    cmake --preset clang-debug
    cmake --build --preset clang-debug --parallel
    cmake --preset clang-debug
EOF
    exit "${1:-1}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)          BUILD_TYPE="release"; shift ;;
        --debug)            BUILD_TYPE="debug"; shift ;;
        --gcc)              COMPILER="gcc"; shift ;;
        --clang)            COMPILER="clang"; shift ;;
        --asan)             SANITIZER="asan"; shift ;;
        --tsan)             SANITIZER="tsan"; shift ;;
        --ubsan)            SANITIZER="ubsan"; shift ;;
        --build-examples)   BUILD_EXAMPLES="ON"; shift ;;
        --test)             RUN_TESTS="ON"; shift ;;
        --clean)            CLEAN="ON"; shift ;;
        -h|--help)          print usage 0 ;;
        *)                  echo "Unknown option: $1"; print_usage ;;
    esac
done

# Map the flags onto a preset name.
if [[ -n "$SANITIZER" ]]; then
    PRESET="$SANITIZER"             # sanitizer presets are Debug + clang
elif [[ "$BUILD_EXAMPLES" == "ON" ]]; then
    PRESET="examples"               # Release + clang + examples
else
    PRESET="${COMPILER}-${BUILD_TYPE}"
fi

echo "preset: ${PRESET}"

if [[ "$CLEAN" == "ON" ]]; then
    echo "removing build/${PRESET}"
    rm -rf "build/${PRESET}"
fi

cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}" --parallel

if [[ "${RUN_TESTS}" == "ON" ]]; then
    ctest --preset "${PRESET}"
fi