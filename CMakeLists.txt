cmake_minimum_required(VERSION 3.10)
project(Vectora C)

set(CMAKE_C_STANDARD 11)

option(VECTORA_ENABLE_ASAN "Enable AddressSanitizer instrumentation" OFF)

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
	src/play/entity/entity_registry.c
	src/play/entity/entity_manager.c
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
	src/play/entity
	src/play/chunk
)

if (VECTORA_ENABLE_ASAN)
	if (MSVC)
		target_compile_options(Vectora PRIVATE /fsanitize=address /Zi)

		# Make ASan builds runnable from the build output directory by copying
		# sanitizer runtime DLLs next to the executable when they exist.
		get_filename_component(_vectora_msvc_bin_dir "${CMAKE_C_COMPILER}" DIRECTORY)
		set(_vectora_asan_dll_candidates
			"${_vectora_msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll"
			"${_vectora_msvc_bin_dir}/clang_rt.asan_dbg_dynamic-x86_64.dll"
		)

		foreach(_vectora_asan_dll IN LISTS _vectora_asan_dll_candidates)
			if (EXISTS "${_vectora_asan_dll}")
				add_custom_command(TARGET Vectora POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"${_vectora_asan_dll}"
						"$<TARGET_FILE_DIR:Vectora>"
				)
			endif()
		endforeach()
	else()
		target_compile_options(Vectora PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
		target_link_options(Vectora PRIVATE -fsanitize=address)
	endif()
endif()
