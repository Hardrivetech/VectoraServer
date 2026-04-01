#include "uuid_utils.h"
#include <string.h>

// Generate a random UUID (version 4, RFC 4122 compliant)
#include <stdlib.h>
#include <time.h>
void write_dummy_uuid(uint8_t *buf) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    for (int i = 0; i < 16; ++i) buf[i] = rand() & 0xFF;
    buf[6] = (buf[6] & 0x0F) | 0x40; // Version 4
    buf[8] = (buf[8] & 0x3F) | 0x80; // Variant RFC 4122
}
