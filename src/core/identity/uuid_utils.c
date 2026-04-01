#include "uuid_utils.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

// Generate a random UUID (version 4, RFC 4122 compliant)
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

// Generate a deterministic UUID from a username for offline mode
// Uses FNV-1a hash function to create consistent UUIDs from usernames
void write_offline_mode_uuid(uint8_t *buf, const char *username) {
    if (!buf || !username) {
        write_dummy_uuid(buf);
        return;
    }

    // FNV-1a 64-bit hash
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint64_t fnv_prime = 0x100000001B3ULL;

    for (const char *c = username; *c != '\0'; ++c) {
        hash ^= (uint8_t)*c;
        hash = (hash * fnv_prime) & 0xFFFFFFFFFFFFFFFFULL;
    }

    // Use hash to fill UUID bytes deterministically
    // Format: 8 bytes from hash, 8 bytes from hash continuation
    for (int i = 0; i < 8; ++i) {
        buf[i] = (uint8_t)((hash >> (i * 8)) & 0xFF);
    }

    // Second half: hash variant of username length
    uint64_t hash2 = 0xCBF29CE484222325ULL;
    size_t len = strlen(username);
    for (size_t i = 0; i < len; ++i) {
        hash2 ^= (uint8_t)(username[i] + i);
        hash2 = (hash2 * fnv_prime) & 0xFFFFFFFFFFFFFFFFULL;
    }

    for (int i = 0; i < 8; ++i) {
        buf[8 + i] = (uint8_t)((hash2 >> (i * 8)) & 0xFF);
    }

    // Mark as version 3 (deterministic from name)
    buf[6] = (buf[6] & 0x0F) | 0x30;
    buf[8] = (buf[8] & 0x3F) | 0x80;
}
