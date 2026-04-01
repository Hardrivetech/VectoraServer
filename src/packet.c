#include "packet.h"
#include <stdint.h>
#include <stddef.h>

size_t write_varint(uint8_t *buffer, int32_t value) {
    size_t i = 0;
    do {
        uint8_t temp = value & 0x7F;
        value >>= 7;
        if (value != 0) temp |= 0x80;
        buffer[i++] = temp;
    } while (value != 0);
    return i;
}
