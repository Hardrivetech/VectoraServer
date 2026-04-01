cmake_minimum_required(VERSION 3.10)
project(Vectora C)

set(CMAKE_C_STANDARD 11)

if (WIN32)
	enable_language(RC)
	set(VECTORA_ICON src/vectora.rc src/dummy_win32_icon.c)
else()
	set(VECTORA_ICON)
endif()

add_executable(Vectora src/main.c src/protocol.c src/util.c src/packet.c src/status.c src/uuid_util.c src/join_game.c src/player_pos.c src/config_packets.c src/config_replay.c src/world_loader.c src/play_packets.c ${VECTORA_ICON})
