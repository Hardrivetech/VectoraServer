cmake_minimum_required(VERSION 3.15)
project(Vectora)

set(CMAKE_CXX_STANDARD 20)

add_executable(Vectora
    src/main.cpp
    src/network/NetworkServer.cpp
    src/protocol/PacketHandler.cpp
    src/world/World.cpp
)

include_directories(include)
