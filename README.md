# Vectora

A minimal custom Minecraft Java Edition server written in C.

## Build Instructions

1. Make sure you have CMake and a C compiler installed.
2. Open a terminal in this directory.
3. Run:

   mkdir build
   cd build
   cmake ..
   cmake --build .

4. Run the server:

   ./Vectora   (on Linux/macOS)
   Vectora.exe (on Windows)

The server will listen on port 25565 by default.

## World Data

The server can also pull basic play-phase world data from a vanilla world folder.

World folder lookup order:

1. `VECTORA_WORLD_PATH` environment variable, if set
2. `world`
3. `../world`

Current world integration covers:

1. Reading `level.dat`
2. Using the real spawn position for Join Game follow-up packets
3. Loading and decompressing the spawn chunk's Anvil NBT payload for future chunk serialization work

See [wiki.vg](https://wiki.vg/Main_Page) for protocol documentation.
