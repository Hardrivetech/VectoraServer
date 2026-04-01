@echo off
REM Build script for Vectora
if not exist build (
    echo Creating build directory...
    mkdir build
) else (
    echo Build directory already exists.
    echo Removing old build files...
    rmdir /s /q build
    echo Creating build directory...
    mkdir build
)
echo Building Vectora...
cd build
cmake ..
cmake --build .
cd ..
echo Build complete. Executable is Vectora(.exe) in the build directory.
pause
