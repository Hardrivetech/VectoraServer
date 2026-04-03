#include "protocol.h"
#include <stdint.h>
#include <stddef.h>

// Reads a VarInt from a buffer, returns the value and advances the pointer
int32_t read_varint(const uint8_t **buffer, size_t *buflen) {
    int32_t value = 0;
    int position = 0;
    uint8_t current_byte;
    do {
        if (*buffer == NULL || buflen == NULL || *buflen == 0 || position >= 5) {
            return -1; // Invalid or too long
        }
        current_byte = **buffer;
        (*buffer)++;
        (*buflen)--;
        value |= ((int32_t)(current_byte & 0x7F)) << (7 * position);
        position++;
    } while (current_byte & 0x80);
    return value;
}
