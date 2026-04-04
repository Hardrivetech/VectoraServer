#include "play_packets.h"

#include "packet.h"
#include "protocol_ids.h"

#include <string.h>

static int has_room(size_t offset, size_t need, size_t capacity) {
    return offset <= capacity && need <= (capacity - offset);
}

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

static void write_f64_be(uint8_t *dst, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_u64_be(dst, bits);
}

static void write_u16_be(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)((value >> 8) & 0xFF);
    dst[1] = (uint8_t)(value & 0xFF);
}

static void write_i16_be(uint8_t *dst, int16_t value) {
    write_u16_be(dst, (uint16_t)value);
}

static void write_position_be(uint8_t *dst, int32_t x, int32_t y, int32_t z) {
    uint64_t packed = (((uint64_t)x & 0x3FFFFFFu) << 38) |
                      (((uint64_t)z & 0x3FFFFFFu) << 12) |
                      ((uint64_t)y & 0xFFFu);
    write_u64_be(dst, packed);
}

size_t build_set_center_chunk_packet(uint8_t *outbuf, size_t outbuf_size, int32_t chunk_x, int32_t chunk_z) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 15) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY_PKT_SET_CENTER_CHUNK);
    offset += write_varint(outbuf + offset, chunk_x);
    offset += write_varint(outbuf + offset, chunk_z);
    return offset;
}

size_t build_set_default_spawn_packet(uint8_t *outbuf, size_t outbuf_size, const char *dimension_name, int32_t x, int32_t y, int32_t z, float yaw, float pitch) {
    size_t offset = 0;
    size_t name_len = strlen(dimension_name);

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, PLAY_PKT_SET_DEFAULT_SPAWN);
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
    offset += write_varint(outbuf + offset, PLAY_PKT_UPDATE_TIME);
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
    offset += write_varint(outbuf + offset, PLAY_PKT_GAME_EVENT);
    outbuf[offset++] = event_id;
    write_f32_be(outbuf + offset, value);
    offset += 4;
    return offset;
}

size_t build_keep_alive_packet(uint8_t *outbuf, size_t outbuf_size, int64_t keep_alive_id) {
    size_t offset = 0;

    (void)outbuf_size;
    offset += write_varint(outbuf + offset, PLAY_PKT_KEEP_ALIVE);
    write_i64_be(outbuf + offset, (uint64_t)keep_alive_id);
    offset += 8;
    return offset;
}

size_t build_brand_packet(uint8_t *outbuf, size_t outbuf_size, const char *brand_name) {
    size_t offset = 0;
    const char *channel = "minecraft:brand";
    size_t channel_len = strlen(channel);
    size_t brand_len = brand_name ? strlen(brand_name) : 0;

    (void)outbuf_size;

    /* Clientbound Plugin Message (play) */
    offset += write_varint(outbuf + offset, PLAY_PKT_PLUGIN_MESSAGE);

    /* Channel identifier */
    offset += write_varint(outbuf + offset, (int32_t)channel_len);
    memcpy(outbuf + offset, channel, channel_len);
    offset += channel_len;

    /* Payload for minecraft:brand is a String */
    offset += write_varint(outbuf + offset, (int32_t)brand_len);
    if (brand_len > 0) {
        memcpy(outbuf + offset, brand_name, brand_len);
        offset += brand_len;
    }

    return offset;
}

size_t build_ack_block_change_packet(uint8_t *outbuf,
                                     size_t outbuf_size,
                                     int32_t sequence_id) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 8) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_ACK_BLOCK_CHANGE);
    offset += write_varint(outbuf + offset, sequence_id);
    return offset;
}

size_t build_block_update_packet(uint8_t *outbuf,
                                 size_t outbuf_size,
                                 int32_t x,
                                 int32_t y,
                                 int32_t z,
                                 int32_t block_state_id) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 16) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_BLOCK_UPDATE);
    write_position_be(outbuf + offset, x, y, z);
    offset += 8;
    offset += write_varint(outbuf + offset, block_state_id);
    return offset;
}

size_t build_spawn_experience_orb_packet(uint8_t *outbuf,
                                         size_t outbuf_size,
                                         int32_t entity_id,
                                         double x,
                                         double y,
                                         double z,
                                         int16_t count) {
    size_t offset = 0;

    (void)outbuf_size;
    // Experience orbs use Spawn Entity (0x01) in protocol 774; this builder
    // is retained for reference but must not be called until the entity type
    // field is verified against the minecraft:entity_type registry.
    offset += write_varint(outbuf + offset, PLAY774_PKT_SPAWN_ENTITY);
    offset += write_varint(outbuf + offset, entity_id);
    write_f64_be(outbuf + offset, x);
    offset += 8;
    write_f64_be(outbuf + offset, y);
    offset += 8;
    write_f64_be(outbuf + offset, z);
    offset += 8;
    write_u16_be(outbuf + offset, (uint16_t)count);
    offset += 2;
    return offset;
}

size_t build_entity_destroy_packet(uint8_t *outbuf,
                                   size_t outbuf_size,
                                   const int32_t *entity_ids,
                                   size_t entity_count) {
    size_t offset = 0;

    if (outbuf == NULL || entity_ids == NULL || entity_count == 0) {
        return 0;
    }

    if (outbuf_size < 2) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_REMOVE_ENTITIES);
    if (offset > outbuf_size) {
        return 0;
    }
    offset += write_varint(outbuf + offset, (int32_t)entity_count);
    if (offset > outbuf_size) {
        return 0;
    }
    for (size_t i = 0; i < entity_count; ++i) {
        if (!has_room(offset, 5, outbuf_size)) {
            return 0;
        }
        offset += write_varint(outbuf + offset, entity_ids[i]);
        if (offset > outbuf_size) {
            return 0;
        }
    }
    return offset;
}

size_t build_spawn_entity_packet(uint8_t *outbuf,
                                 size_t outbuf_size,
                                 int32_t entity_id,
                                 int32_t entity_type,
                                 double x,
                                 double y,
                                 double z,
                                 int8_t pitch,
                                 int8_t yaw,
                                 int8_t head_yaw,
                                 int32_t data) {
    size_t offset = 0;

    if (outbuf == NULL) {
        return 0;
    }

    // Worst-case encoded size is comfortably below 64 bytes.
    if (outbuf_size < 64) {
        return 0;
    }

    // 0x01 add_entity (Spawn Entity) — verified for protocol 774 from minecraft.wiki
    offset += write_varint(outbuf + offset, PLAY774_PKT_SPAWN_ENTITY);

    // Entity ID
    offset += write_varint(outbuf + offset, entity_id);

    // UUID: 16 bytes. Derive from entity_id for test entities (not persisted).
    // Bytes 0-3 = big-endian entity_id, byte 6 = version nibble 4, byte 8 = variant 2, rest zero.
    {
        uint8_t uuid[16];
        memset(uuid, 0, 16);
        write_u32_be(uuid, (uint32_t)entity_id);
        uuid[6] = (uuid[6] & 0x0F) | 0x40; // version 4
        uuid[8] = (uuid[8] & 0x3F) | 0x80; // variant 2
        memcpy(outbuf + offset, uuid, 16);
        offset += 16;
    }

    // Entity type (VarInt): ID in the minecraft:entity_type registry.
    // Placeholder: 2 = armor_stand (verify against actual registry for protocol 774).
    offset += write_varint(outbuf + offset, entity_type);

    // Position (X, Y, Z as doubles)
    write_f64_be(outbuf + offset, x); offset += 8;
    write_f64_be(outbuf + offset, y); offset += 8;
    write_f64_be(outbuf + offset, z); offset += 8;

    // Velocity: Optional<LpVec3> in protocol 774 (1.21.11).
    // Format: bool (1 byte) — false = no velocity, true = Short×3 follows.
    // Despite the wiki listing "LpVec3" (= Short×3), the actual wire format is optional.
    // Confirmed from "5 bytes extra" DecoderException: client reads the first 0x00 byte as
    // the boolean false flag, then reads angles + data, leaving 5 stray bytes.
    outbuf[offset++] = 0x00; // hasVelocity = false (always zero for spawned mobs)

    // Angles: pitch, yaw, head_yaw (each 1 byte, 256 units = 360 degrees)
    outbuf[offset++] = (uint8_t)pitch;
    outbuf[offset++] = (uint8_t)yaw;
    outbuf[offset++] = (uint8_t)head_yaw;

    // Data (VarInt): 0 for most entity types; arrows/fishing hooks use this for owner entity ID.
    offset += write_varint(outbuf + offset, data);

    return offset;
}

// Update Entity Position (0x33 / move_entity_pos) — delta move ≤ 8 blocks.
// Delta encoding: dx = (int16_t)((new_x - old_x) * 4096.0), same for dy and dz.
// Max representable delta: ±8 blocks (32768 units, but Short wraps; use ≤7.999 in practice).
// Use build_entity_position_sync_packet for moves exceeding this range.
size_t build_move_entity_pos_packet(uint8_t *outbuf,
                                    size_t outbuf_size,
                                    int32_t entity_id,
                                    int16_t dx,
                                    int16_t dy,
                                    int16_t dz,
                                    int on_ground) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 16) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_MOVE_ENTITY_POS);
    offset += write_varint(outbuf + offset, entity_id);
    write_i16_be(outbuf + offset, dx); offset += 2;
    write_i16_be(outbuf + offset, dy); offset += 2;
    write_i16_be(outbuf + offset, dz); offset += 2;
    outbuf[offset++] = on_ground ? 1 : 0;

    return offset;
}

// Teleport Entity / Entity Position Sync (0x23 / entity_position_sync) — authoritative
// absolute position, used when the delta would exceed the ±8 block limit of 0x33.
// Velocities are in blocks/tick (typically 0 for stationary mobs).
size_t build_entity_position_sync_packet(uint8_t *outbuf,
                                         size_t outbuf_size,
                                         int32_t entity_id,
                                         double x,
                                         double y,
                                         double z,
                                         double vx,
                                         double vy,
                                         double vz,
                                         float yaw,
                                         float pitch,
                                         int on_ground) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 64) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_ENTITY_POSITION_SYNC);
    offset += write_varint(outbuf + offset, entity_id);
    write_f64_be(outbuf + offset, x);  offset += 8;
    write_f64_be(outbuf + offset, y);  offset += 8;
    write_f64_be(outbuf + offset, z);  offset += 8;
    write_f64_be(outbuf + offset, vx); offset += 8;
    write_f64_be(outbuf + offset, vy); offset += 8;
    write_f64_be(outbuf + offset, vz); offset += 8;
    write_f32_be(outbuf + offset, yaw);   offset += 4;
    write_f32_be(outbuf + offset, pitch); offset += 4;
    outbuf[offset++] = on_ground ? 1 : 0;

    return offset;
}

// Set Head Rotation (0x51 / rotate_head) — rotates entity's head independently.
// Head Yaw is encoded as a single byte where 256 units = 360 degrees.
size_t build_set_head_rotation_packet(uint8_t *outbuf,
                                      size_t outbuf_size,
                                      int32_t entity_id,
                                      float head_yaw) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 8) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_SET_HEAD_ROTATION);
    offset += write_varint(outbuf + offset, entity_id);

    // Convert yaw in degrees (0-360) to angle byte (0-255).
    // head_yaw is in degrees; multiply by 256/360 to get byte value.
    uint8_t angle_byte = (uint8_t)(((head_yaw / 360.0f) * 256.0f));
    outbuf[offset++] = angle_byte;

    return offset;
}

// Set Entity Metadata (0x61 / set_entity_data) for base entity flags.
// This helper updates metadata index 0 (Byte bitmask) to toggle glowing.
size_t build_set_entity_glowing_packet(uint8_t *outbuf,
                                       size_t outbuf_size,
                                       int32_t entity_id,
                                       int glowing) {
    size_t offset = 0;
    uint8_t flags = glowing ? 0x40 : 0x00;

    if (outbuf == NULL || outbuf_size < 16) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_SET_ENTITY_METADATA);
    offset += write_varint(outbuf + offset, entity_id);

    // Metadata entry: index=0 (entity flags), type=0 (Byte), value=flags.
    outbuf[offset++] = 0x00;
    offset += write_varint(outbuf + offset, 0);
    outbuf[offset++] = flags;

    // Metadata terminator.
    outbuf[offset++] = 0xFF;

    return offset;
}

// Set Entity Metadata (0x61 / set_entity_data) for item entities.
// Writes metadata index 8 (Slot) so dropped item entities render on the client.
size_t build_set_item_entity_slot_packet(uint8_t *outbuf,
                                         size_t outbuf_size,
                                         int32_t entity_id,
                                         int32_t item_id,
                                         int32_t item_count) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 32) {
        return 0;
    }

    if (item_count <= 0) {
        return 0;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_SET_ENTITY_METADATA);
    offset += write_varint(outbuf + offset, entity_id);

    // Metadata entry: index=8 (Item entity slot), type=7 (Slot).
    outbuf[offset++] = 0x08;
    offset += write_varint(outbuf + offset, 7);

    // Slot format (1.21+): count, item id, add component count, remove component count.
    offset += write_varint(outbuf + offset, item_count);
    offset += write_varint(outbuf + offset, item_id);
    offset += write_varint(outbuf + offset, 0);
    offset += write_varint(outbuf + offset, 0);

    // Metadata terminator.
    outbuf[offset++] = 0xFF;

    return offset;
}

size_t build_take_item_entity_packet(uint8_t *outbuf,
                                     size_t outbuf_size,
                                     int32_t collected_entity_id,
                                     int32_t collector_entity_id,
                                     int32_t item_count) {
    size_t offset = 0;

    if (outbuf == NULL || outbuf_size < 16) {
        return 0;
    }

    if (item_count < 1) {
        item_count = 1;
    }

    offset += write_varint(outbuf + offset, PLAY774_PKT_TAKE_ITEM_ENTITY);
    offset += write_varint(outbuf + offset, collected_entity_id);
    offset += write_varint(outbuf + offset, collector_entity_id);
    offset += write_varint(outbuf + offset, item_count);
    return offset;
}