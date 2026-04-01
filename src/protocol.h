#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// Reads a VarInt from a buffer, returns the value and advances the pointer
int32_t read_varint(const uint8_t **buffer, size_t *buflen);

#endif // PROTOCOL_H
