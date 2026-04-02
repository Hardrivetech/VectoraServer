#ifndef CHUNK_SENDER_H
#define CHUNK_SENDER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Build a Chunk Data and Update Light packet (0x2C) from raw decompressed
 * chunk NBT (as loaded by world_loader).
 *
 * Returns a heap-allocated buffer containing the packet payload (packet ID +
 * data) whose length is written to *out_len.  The caller must free() it.
 * Returns NULL on failure.
 *
 * chunk_x / chunk_z are the chunk coordinates in chunk space (block / 16).
 */
uint8_t *build_chunk_data_packet(const uint8_t *nbt, size_t nbt_len,
                                  int32_t chunk_x, int32_t chunk_z,
                                  size_t *out_len);

/*
 * Build a synthetic debug chunk with a full-stone section at y=64..79.
 * Useful to validate clientbound chunk transport and packet sequencing.
 */
uint8_t *build_debug_flat_chunk_packet(int32_t chunk_x, int32_t chunk_z,
                                       size_t *out_len);

/*
 * Build a simple procedurally generated overworld chunk used when no real
 * chunk data is available. Terrain is deterministic from chunk coordinates.
 */
uint8_t *build_generated_overworld_chunk_packet(int32_t chunk_x, int32_t chunk_z,
                                                size_t *out_len);

/*
 * Compute the generated terrain surface Y for a block position.
 */
int32_t generated_world_surface_y(int32_t block_x, int32_t block_z);

#endif /* CHUNK_SENDER_H */
