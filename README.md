# Vectora

A high-performance custom Minecraft Java Edition 1.21.11 server written in C++.

## Features
- Modular architecture
- High-performance networking
- Extensible protocol and world logic

## Build Instructions

1. Install CMake (version 3.15+), a C++20 compiler, and any required dependencies for your platform.
2. Clone/download this repository.
3. From the project root, run:
   ```
   mkdir build
   cd build
   cmake ..
   cmake --build .
   ```
4. Run the server:
   ```
   ./Vectora
   ```

## Project Structure
- `src/` — Source files
- `include/` — Header files
- `CMakeLists.txt` — Build configuration

## Next Steps
- Implement networking in `src/network/NetworkServer.cpp`
- Add packet handling in `src/protocol/PacketHandler.cpp`
- Develop world logic in `src/world/World.cpp`
