#ifndef PLAY_PACKETS_H
#define PLAY_PACKETS_H

#include <stddef.h>
#include <stdint.h>
#include "game_rules.h"

size_t build_set_center_chunk_packet(uint8_t *outbuf, size_t outbuf_size, int32_t chunk_x, int32_t chunk_z);
size_t build_set_default_spawn_packet(uint8_t *outbuf, size_t outbuf_size, const char *dimension_name, int32_t x, int32_t y, int32_t z, float yaw, float pitch);
size_t build_update_time_packet(uint8_t *outbuf, size_t outbuf_size, int64_t world_age, int64_t time_of_day, int increasing);
size_t build_game_event_packet(uint8_t *outbuf, size_t outbuf_size, uint8_t event_id, float value);
size_t build_keep_alive_packet(uint8_t *outbuf, size_t outbuf_size, int64_t keep_alive_id);
size_t build_brand_packet(uint8_t *outbuf, size_t outbuf_size, const char *brand_name);
size_t build_game_rules_packet(uint8_t *outbuf, size_t outbuf_size, const game_rules_t *rules);
size_t build_spawn_experience_orb_packet(uint8_t *outbuf,
										 size_t outbuf_size,
										 int32_t entity_id,
										 double x,
										 double y,
										 double z,
										 int16_t count);
size_t build_entity_destroy_packet(uint8_t *outbuf,
								   size_t outbuf_size,
								   const int32_t *entity_ids,
								   size_t entity_count);
size_t build_spawn_entity_packet(uint8_t *outbuf,
                                 size_t outbuf_size,
                                 int32_t entity_id,
                                 int32_t entity_type,
                                 double x,
                                 double y,
                                 double z,
                                 int8_t pitch,
                                 int8_t yaw,
                                 int8_t head_yaw,
                                 int32_t data);
size_t build_move_entity_pos_packet(uint8_t *outbuf,
                                    size_t outbuf_size,
                                    int32_t entity_id,
                                    int16_t dx,
                                    int16_t dy,
                                    int16_t dz,
                                    int on_ground);
size_t build_entity_position_sync_packet(uint8_t *outbuf,
                                         size_t outbuf_size,
                                         int32_t entity_id,
                                         double x,
                                         double y,
                                         double z,
                                         double vx,
                                         double vy,
                                         double vz,
                                         float yaw,
                                         float pitch,
                                         int on_ground);
size_t build_set_head_rotation_packet(uint8_t *outbuf,
                                      size_t outbuf_size,
                                      int32_t entity_id,
                                      float head_yaw);
size_t build_set_entity_glowing_packet(uint8_t *outbuf,
                                       size_t outbuf_size,
                                       int32_t entity_id,
                                       int glowing);

#endif