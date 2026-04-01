#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stddef.h>

// Writes a VarInt to a buffer, returns number of bytes written
size_t write_varint(uint8_t *buffer, int32_t value);

#endif // PACKET_H
