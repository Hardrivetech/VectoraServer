#ifndef UUID_UTILS_H
#define UUID_UTILS_H

#include <stdint.h>
#include <stddef.h>

// Writes a dummy UUID (all zeroes) to a buffer
void write_dummy_uuid(uint8_t *buf);

// Generate an offline-mode UUID from a username
// Uses a deterministic algorithm based on username string hashing
void write_offline_mode_uuid(uint8_t *buf, const char *username);

#endif // UUID_UTILS_H
