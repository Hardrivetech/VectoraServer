cmake_minimum_required(VERSION 3.15)
set(CMAKE_TOOLCHAIN_FILE "C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")


project(Vectora)

set(CMAKE_CXX_STANDARD 20)

add_executable(Vectora
    src/main.cpp
    src/network/NetworkServer.cpp
    src/protocol/PacketHandler.cpp
    src/world/World.cpp
    src/world/anvil/AnvilRegion.cpp
    src/world/anvil/ChunkParser.cpp
    src/world/anvil/ChunkSerializer.cpp
    src/world/anvil/NBT.cpp
    src/world/anvil/ZlibUtil.cpp
    src/world/anvil/BlockStateParser.cpp
    appicon.rc
)

include_directories(include)

find_package(ZLIB REQUIRED)
target_link_libraries(Vectora PRIVATE ZLIB::ZLIB)
