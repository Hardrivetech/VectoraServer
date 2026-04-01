#ifndef PLAY_PACKETS_H
#define PLAY_PACKETS_H

#include <stddef.h>
#include <stdint.h>

size_t build_set_center_chunk_packet(uint8_t *outbuf, size_t outbuf_size, int32_t chunk_x, int32_t chunk_z);
size_t build_set_default_spawn_packet(uint8_t *outbuf, size_t outbuf_size, const char *dimension_name, int32_t x, int32_t y, int32_t z, float yaw, float pitch);
size_t build_update_time_packet(uint8_t *outbuf, size_t outbuf_size, int64_t world_age, int64_t time_of_day, int increasing);
size_t build_game_event_packet(uint8_t *outbuf, size_t outbuf_size, uint8_t event_id, float value);
size_t build_keep_alive_packet(uint8_t *outbuf, size_t outbuf_size, int64_t keep_alive_id);
size_t build_brand_packet(uint8_t *outbuf, size_t outbuf_size, const char *brand_name);

#endif