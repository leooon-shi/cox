#!/bin/bash

# Default build type
BUILD_TYPE="Debug"

# Parse input arguments
if [[ "$1" == "--release" ]]; then
    BUILD_TYPE="Release"
elif [[ "$1" == "--debug" ]]; then
    BUILD_TYPE="Debug"
fi

echo "Building in ${BUILD_TYPE} mode..."

# Create and navigate to the build directory
mkdir -p build
cd build

# Clean the build directory
rm -rf *

# Run CMake with the specified build type
cmake -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=${BUILD_TYPE} ..

# Build the project using all available cores
make -j

# Return to the previous directory
cd -
