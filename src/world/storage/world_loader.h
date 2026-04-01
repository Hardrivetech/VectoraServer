#ifndef WORLD_LOADER_H
#define WORLD_LOADER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char world_path[512];
    char dimension_name[64];
    int32_t spawn_x;
    int32_t spawn_y;
    int32_t spawn_z;
    float spawn_yaw;
    float spawn_pitch;
    int32_t spawn_chunk_x;
    int32_t spawn_chunk_z;
    int32_t sea_level;
    int is_flat;
    int has_spawn_chunk;
    uint8_t *spawn_chunk_nbt;
    size_t spawn_chunk_nbt_len;
} world_info_t;

int load_world_info(world_info_t *info, char *error, size_t error_size);
void free_world_info(world_info_t *info);

/*
 * Loads a specific chunk NBT payload from region files.
 * Caller owns *out_chunk_nbt and must free() it.
 * Returns 1 on success, 0 on failure.
 */
int load_chunk_nbt_at(const world_info_t *info,
                      int32_t chunk_x,
                      int32_t chunk_z,
                      uint8_t **out_chunk_nbt,
                      size_t *out_chunk_nbt_len);

#endif