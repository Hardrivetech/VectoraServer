#!/bin/bash
# Build script for Vectora (Linux/macOS)

# Remove previous build directory if it exists
if [ -d "build" ]; then
    rm -rf build
fi

# Create build directory
mkdir build
cd build

# Run CMake and build
cmake ../Vectora
cmake --build .

# Copy world folder
cp -r ../Vectora/world ./Debug/world/

cd ..
echo "Build complete. To run: build/Debug/Vectora"
