#ifndef UUID_UTILS_H
#define UUID_UTILS_H

#include <stdint.h>
#include <stddef.h>

// Writes a dummy UUID (all zeroes) to a buffer
void write_dummy_uuid(uint8_t *buf);

#endif // UUID_UTILS_H
