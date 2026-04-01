#ifndef PLAYER_POSITION_H
#define PLAYER_POSITION_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	int32_t teleport_id;
	double x;
	double y;
	double z;
	double velocity_x;
	double velocity_y;
	double velocity_z;
	float yaw;
	float pitch;
	uint32_t flags;
} player_pos_params_t;

// Writes a minimal Player Position and Look packet to outbuf, returns length
size_t build_player_pos_packet(uint8_t *outbuf, size_t outbuf_size);
size_t build_player_pos_packet_ex(uint8_t *outbuf, size_t outbuf_size, const player_pos_params_t *params);

#endif // PLAYER_POSITION_H
