#include "player_pos.h"
#include "packet.h"
#include <string.h>
#include <stdint.h>

static void write_u32_be(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)((v >> 24) & 0xFF);
    dst[1] = (uint8_t)((v >> 16) & 0xFF);
    dst[2] = (uint8_t)((v >> 8) & 0xFF);
    dst[3] = (uint8_t)(v & 0xFF);
}

static void write_u64_be(uint8_t *dst, uint64_t v) {
    dst[0] = (uint8_t)((v >> 56) & 0xFF);
    dst[1] = (uint8_t)((v >> 48) & 0xFF);
    dst[2] = (uint8_t)((v >> 40) & 0xFF);
    dst[3] = (uint8_t)((v >> 32) & 0xFF);
    dst[4] = (uint8_t)((v >> 24) & 0xFF);
    dst[5] = (uint8_t)((v >> 16) & 0xFF);
    dst[6] = (uint8_t)((v >> 8) & 0xFF);
    dst[7] = (uint8_t)(v & 0xFF);
}

static void write_f32_be(uint8_t *dst, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    write_u32_be(dst, bits);
}

static void write_f64_be(uint8_t *dst, double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    write_u64_be(dst, bits);
}

size_t build_player_pos_packet_ex(uint8_t *outbuf, size_t outbuf_size, const player_pos_params_t *params) {
    uint8_t packet[64];
    size_t offset = 0;

    (void)outbuf_size;
    offset += write_varint(packet + offset, 0x46); // Synchronize Player Position

    // Teleport ID
    offset += write_varint(packet + offset, params != NULL ? params->teleport_id : 1);

    // X, Y, Z (double, big-endian)
    write_f64_be(packet + offset, params != NULL ? params->x : 0.0); offset += 8;
    write_f64_be(packet + offset, params != NULL ? params->y : 64.0); offset += 8;
    write_f64_be(packet + offset, params != NULL ? params->z : 0.0); offset += 8;

    // Velocity X, Y, Z (double, big-endian)
    write_f64_be(packet + offset, params != NULL ? params->velocity_x : 0.0); offset += 8;
    write_f64_be(packet + offset, params != NULL ? params->velocity_y : 0.0); offset += 8;
    write_f64_be(packet + offset, params != NULL ? params->velocity_z : 0.0); offset += 8;

    // Yaw, Pitch (float, big-endian)
    write_f32_be(packet + offset, params != NULL ? params->yaw : 0.0f); offset += 4;
    write_f32_be(packet + offset, params != NULL ? params->pitch : 0.0f); offset += 4;

    // Teleport Flags (int, 0 = absolute everything)
    write_u32_be(packet + offset, params != NULL ? params->flags : 0);
    offset += 4;

    // Return raw packet bytes; caller applies post-compression framing.
    memcpy(outbuf, packet, offset);
    return offset;
}

size_t build_player_pos_packet(uint8_t *outbuf, size_t outbuf_size) {
    player_pos_params_t defaults;

    memset(&defaults, 0, sizeof(defaults));
    defaults.teleport_id = 1;
    defaults.y = 64.0;

    return build_player_pos_packet_ex(outbuf, outbuf_size, &defaults);
}
