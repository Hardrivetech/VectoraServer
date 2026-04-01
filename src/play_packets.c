#include "play_packets.h"

#include "packet.h"

#include <string.h>

static void write_u32_be(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)((value >> 24) & 0xFF);
    dst[1] = (uint8_t)((value >> 16) & 0xFF);
    dst[2] = (uint8_t)((value >> 8) & 0xFF);
    dst[3] = (uint8_t)(value & 0xFF);
}

static void write_u64_be(uint8_t *dst, uint64_t value) {
    dst[0] = (uint8_t)((value >> 56) & 0xFF);
    dst[1] = (uint8_t)((value >> 48) & 0xFF);
    dst[2] = (uint8_t)((value >> 40) & 0xFF);
    dst[3] = (uint8_t)((value >> 32) & 0xFF);
    dst[4] = (uint8_t)((value >> 24) & 0xFF);
    dst[5] = (uint8_t)((value >> 16) & 0xFF);
    dst[6] = (uint8_t)((value >> 8) & 0xFF);
    dst[7] = (uint8_t)(value & 0xFF);
}

static void write_f32_be(uint8_t *dst, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_u32_be(dst, bits);
}

static void write_i64_be(uint8_t *dst, int64_t value) {
    write_u64_be(dst, (uint64_t)value);
}

static void write_position_be(uint8_t *dst, int32_t x, int32_t y, int32_t z) {
    uint64_t packed = (((uint64_t)x & 0x3FFFFFFu) << 38) |
                      (((uint64_t)z & 0x3FFFFFFu) << 12) |
                      ((uint64_t)y & 0xFFFu);
    write_u64_be(dst, packed);
}

size_t build_set_center_chunk_packet(uint8_t *outbuf, size_t outbuf_size, int32_t chunk_x, int32_t chunk_z) {
    size_t offset = 0;

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, 0x5C);
    offset += write_varint(outbuf + offset, chunk_x);
    offset += write_varint(outbuf + offset, chunk_z);
    return offset;
}

size_t build_set_default_spawn_packet(uint8_t *outbuf, size_t outbuf_size, const char *dimension_name, int32_t x, int32_t y, int32_t z, float yaw, float pitch) {
    size_t offset = 0;
    size_t name_len = strlen(dimension_name);

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, 0x5F);
    offset += write_varint(outbuf + offset, (int32_t)name_len);
    memcpy(outbuf + offset, dimension_name, name_len);
    offset += name_len;
    write_position_be(outbuf + offset, x, y, z);
    offset += 8;
    write_f32_be(outbuf + offset, yaw);
    offset += 4;
    write_f32_be(outbuf + offset, pitch);
    offset += 4;
    return offset;
}

size_t build_update_time_packet(uint8_t *outbuf, size_t outbuf_size, int64_t world_age, int64_t time_of_day, int increasing) {
    size_t offset = 0;

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, 0x6F);
    write_i64_be(outbuf + offset, world_age);
    offset += 8;
    write_i64_be(outbuf + offset, time_of_day);
    offset += 8;
    outbuf[offset++] = increasing ? 0x01 : 0x00;
    return offset;
}

size_t build_game_event_packet(uint8_t *outbuf, size_t outbuf_size, uint8_t event_id, float value) {
    size_t offset = 0;

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, 0x26);
    outbuf[offset++] = event_id;
    write_f32_be(outbuf + offset, value);
    offset += 4;
    return offset;
}

size_t build_keep_alive_packet(uint8_t *outbuf, size_t outbuf_size, int64_t keep_alive_id) {
    size_t offset = 0;

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, 0x2B);
    write_i64_be(outbuf + offset, (uint64_t)keep_alive_id);
    offset += 8;
    return offset;
}