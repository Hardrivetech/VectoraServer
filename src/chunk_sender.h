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

#endif /* CHUNK_SENDER_H */
