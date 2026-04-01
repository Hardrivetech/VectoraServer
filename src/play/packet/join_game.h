#ifndef JOIN_GAME_H
#define JOIN_GAME_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	int32_t entity_id;
	const char *dimension_name;
	int32_t max_players;
	int32_t view_distance;
	int32_t simulation_distance;
	int32_t dimension_type_id;
	uint64_t hashed_seed;
	uint8_t game_mode;
	int8_t previous_game_mode;
	uint8_t difficulty;
	int is_debug;
	int is_flat;
	int portal_cooldown;
	int32_t sea_level;
	int enforces_secure_chat;
} join_game_params_t;

// Writes a minimal Join Game packet to outbuf, returns length
size_t build_join_game_packet(uint8_t *outbuf, size_t outbuf_size);
size_t build_join_game_packet_ex(uint8_t *outbuf, size_t outbuf_size, const join_game_params_t *params);

#endif // JOIN_GAME_H
