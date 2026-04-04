#include "packet.h"
#include <stdint.h>
#include <stddef.h>

size_t write_varint(uint8_t *buffer, int32_t value) {
    size_t i = 0;
    uint32_t uvalue;

    if (buffer == NULL) {
        return 0;
    }

    // Minecraft VarInt uses two's-complement 32-bit values and must encode
    // with logical right shifts to terminate for negative numbers.
    uvalue = (uint32_t)value;
    do {
        uint8_t temp = (uint8_t)(uvalue & 0x7F);
        uvalue >>= 7;
        if (uvalue != 0) {
            temp |= 0x80;
        }
        buffer[i++] = temp;
    } while (uvalue != 0 && i < 5);
    return i;
}
