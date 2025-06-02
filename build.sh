#!/bin/bash

# Default configurations
BUILD_TYPE="Debug"
BUILD_EXAMPLES="OFF"
BUILD_TESTS="OFF"

# Function to print usage
print_usage() {
    echo "Usage: $0 [--debug | --release] [--build-examples] [--build-tests]"
    echo "Options:"
    echo "  --debug           Build in Debug mode (default)"
    echo "  --release         Build in Release mode"
    echo "  --build-examples  Enable building examples"
    echo "  --build-tests     Enable building tests"
    exit 1
}

# Function to parse input arguments
parse_arguments() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --release)
                BUILD_TYPE="Release"
                shift
                ;;
            --debug)
                BUILD_TYPE="Debug"
                shift
                ;;
            --build-examples)
                BUILD_EXAMPLES="ON"
                shift
                ;;
            --build-tests)
                BUILD_TESTS="ON"
                shift
                ;;
            *)
                echo "Unknown option: $1"
                print_usage
                ;;
        esac
    done
}

# Function to configure the build
configure_build() {
    echo "Building in ${BUILD_TYPE} mode..."
    echo "Build examples: ${BUILD_EXAMPLES}"
    echo "Build tests: ${BUILD_TESTS}"

    # Create and navigate to the build directory
    mkdir -p build
    cd build

    # Clean the build directory
    rm -rf *

    # Run CMake with the specified options
    cmake -DCMAKE_CXX_COMPILER=clang++ \
          -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
          -DBUILD_EXAMPLES=${BUILD_EXAMPLES} \
          -DBUILD_TESTS=${BUILD_TESTS} ..
}

# Function to build the project
build_project() {
    # Build the project using all available cores
    make -j
}

# Function to clean up and return to the previous directory
cleanup() {
    cd -
}

# Main script execution
parse_arguments "$@"
configure_build
build_project
cleanup