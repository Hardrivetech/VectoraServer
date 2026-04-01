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

## Config Replay

For modern 1.21 clients, the easiest way to get past configuration registry sync is to replay
captured vanilla clientbound configuration packets.

The server will try these replay file locations in order:

1. `VECTORA_CONFIG_REPLAY` environment variable, if set
2. `replay/config_packets.hex`
3. `../replay/config_packets.hex`
4. `config_packets.hex`
5. `../config_packets.hex`

Replay file format:

1. One packet per line
2. Hex bytes only for raw `Packet ID + Data`
3. Do not include packet length or compression framing
4. `#` starts a comment

Example:

`0C 01 11 6D 69 6E 65 63 72 61 66 74 3A 76 61 6E 69 6C 6C 61`

The server automatically wraps replayed packets in the post-compression format.

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
