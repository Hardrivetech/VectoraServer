@echo off
REM Build script for Vectora (Windows)

REM Remove previous build directory if it exists
if exist build rmdir /s /q build

REM Create build directory
mkdir build
cd build

REM Run CMake and build
cmake ..\Vectora
cmake --build .

cd ..
echo Build complete. To run: build\Vectora.exe
