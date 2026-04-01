#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>

// Reads a Minecraft String (VarInt length + UTF-8 bytes)
// Returns pointer to malloc'd null-terminated string, advances buffer pointer and buflen
char *read_mc_string(const uint8_t **buffer, size_t *buflen);

#endif // UTIL_H
