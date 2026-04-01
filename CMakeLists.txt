cmake_minimum_required(VERSION 3.10)
project(Vectora C)

set(CMAKE_C_STANDARD 11)

if (WIN32)
	enable_language(RC)
	set(VECTORA_ICON src/platform/windows/vectora.rc src/platform/windows/dummy_win32_icon.c)
else()
	set(VECTORA_ICON)
endif()

add_executable(Vectora
	src/app/main.c
	src/core/network/protocol.c
	src/core/common/util.c
	src/core/network/packet.c
	src/core/common/status.c
	src/core/identity/uuid_utils.c
	src/config/packet/configuration_packets.c
	src/config/replay/configuration_replay.c
	src/config/runtime/server_config.c
	src/world/storage/world_loader.c
	src/play/packet/join_game.c
	src/play/packet/game_rules.c
	src/play/player/player_position.c
	src/play/packet/play_packets.c
	src/play/chunk/chunk_sender.c
	${VECTORA_ICON}
)

target_include_directories(Vectora PRIVATE
	src/core/network
	src/core/common
	src/core/identity
	src/config/packet
	src/config/replay
	src/config/runtime
	src/world/storage
	src/play/packet
	src/play/player
	src/play/chunk
)
