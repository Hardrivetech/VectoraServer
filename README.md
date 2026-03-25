
# Vectora

This project is licensed under the [MIT License](LICENSE).

**Project Roadmap:**
Track features, progress, and ideas on the [Vectora Roadmap Project Board](https://github.com/users/Hardrivetech/projects/3).

Vectora is a high-performance custom Minecraft Java Edition 1.21.11 server written in modern C++20.

## Current Status

- **Protocol-correct:** Accepts real Minecraft clients, appears in the multiplayer server list, and supports login/play states.
- **Dynamic chunk/world loading:** Loads and serves real Minecraft world data from Anvil region files (`.mca`).
- **Block state unpacking:** Correctly unpacks and serializes chunk block data for client rendering.
- **Per-client state:** Tracks protocol state, login, and play for each connected client.
- **Server-to-client chat:** Echoes chat messages back to the sender using the correct packet format.
- **Player movement and chat stubs:** Handles and logs player movement and chat packets, reducing unhandled packet logs.
- **Robust error handling:** Logs malformed packets, unsupported compression, and chunk loading issues.
- **Modular, extensible codebase:** Clean separation of protocol, world, and network logic for easy extension.

## Features

- Modular architecture
- High-performance networking
- Extensible protocol and world logic
- Real world/chunk loading from region files
- Protocol stubs for chat and movement
- C++20, CMake, and cross-platform support


## Build Instructions

### Windows

1. Install CMake (version 3.15+), a C++20-compatible compiler (e.g., MSVC), and any required dependencies.
2. Clone/download this repository.
3. From the `Vectora` directory, you can use the provided batch script for a quick build:
   ```
   build.bat
   ```
   This will configure and build the project automatically.
4. Alternatively, you can build manually:
   ```
   mkdir build
   cd build
   cmake ..
   cmake --build .
   ```
5. Run the server:
   ```
   .\Vectora.exe
   ```

### Linux/macOS

1. Install CMake (version 3.15+), a C++20 compiler (e.g., GCC, Clang), and any required dependencies.
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
- `world/` — Minecraft world/region files

## Recent Progress

- Protocol pipeline, status/MOTD, and per-client state implemented
- Dynamic chunk serving and block state unpacking
- Real client support (login, play, chunk requests)
- Server-to-client chat echo and protocol stubs
- Improved error handling and debug output

## Next Steps

- Implement more play state packet handlers (block breaking, inventory, entity interaction)
- Add server-to-client chat broadcast and system messages
- Track and update player state (positions, world interaction)
- Add basic world interaction (block updates, entity handling)
- Expand protocol support for more Minecraft features
