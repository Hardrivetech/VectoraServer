#include "join_game.h"
#include "packet.h"
#include <string.h>

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

size_t build_join_game_packet_ex(uint8_t *outbuf, size_t outbuf_size, const join_game_params_t *params) {
    uint8_t packet[1024];
    size_t offset = 0;
    const char *world = "minecraft:overworld";
    size_t world_len = strlen(world);

    (void)outbuf_size;
    if (params != NULL && params->dimension_name != NULL) {
        world = params->dimension_name;
        world_len = strlen(world);
    }

    offset += write_varint(packet + offset, 0x30); // Login (play)

    // Entity ID (int, big-endian)
    write_u32_be(packet + offset, (uint32_t)(params != NULL ? params->entity_id : 1));
    offset += 4;

    // Is hardcore (bool)
    packet[offset++] = 0x00;

    // Dimension Names (Prefixed Array of Identifier)
    offset += write_varint(packet + offset, 1);
    offset += write_varint(packet + offset, (int)world_len);
    memcpy(packet + offset, world, world_len);
    offset += world_len;

    // Max Players, View Distance, Simulation Distance
    offset += write_varint(packet + offset, params != NULL ? params->max_players : 20);
    offset += write_varint(packet + offset, params != NULL ? params->view_distance : 10);
    offset += write_varint(packet + offset, params != NULL ? params->simulation_distance : 10);

    // Reduced Debug Info, Enable Respawn Screen, Do Limited Crafting
    packet[offset++] = 0x00;
    packet[offset++] = 0x01;
    packet[offset++] = 0x00;

    // Dimension Type (VarInt registry id)
    offset += write_varint(packet + offset, params != NULL ? params->dimension_type_id : 0);

    // Dimension Name (Identifier)
    offset += write_varint(packet + offset, (int)world_len);
    memcpy(packet + offset, world, world_len);
    offset += world_len;

    // Hashed seed (long, big-endian)
    write_u64_be(packet + offset, params != NULL ? params->hashed_seed : 0);
    offset += 8;

    // Game mode (ubyte), Previous game mode (byte), Difficulty (ubyte)
    packet[offset++] = params != NULL ? params->game_mode : 0x00;
    packet[offset++] = (uint8_t)(params != NULL ? params->previous_game_mode : -1);
    packet[offset++] = params != NULL ? params->difficulty : 0x02;

    // Is Debug, Is Flat, Has Death Location
    packet[offset++] = params != NULL && params->is_debug ? 0x01 : 0x00;
    packet[offset++] = params != NULL && params->is_flat ? 0x01 : 0x00;
    packet[offset++] = 0x00;

    // Portal cooldown (VarInt)
    offset += write_varint(packet + offset, params != NULL ? params->portal_cooldown : 0);

    // Sea level (VarInt), Enforces Secure Chat (bool)
    offset += write_varint(packet + offset, params != NULL ? params->sea_level : 63);
    packet[offset++] = params != NULL && params->enforces_secure_chat ? 0x01 : 0x00;

    // Return raw packet bytes; caller applies post-compression framing.
    memcpy(outbuf, packet, offset);
    return offset;
}

size_t build_join_game_packet(uint8_t *outbuf, size_t outbuf_size) {
    join_game_params_t defaults;

    memset(&defaults, 0, sizeof(defaults));
    defaults.entity_id = 1;
    defaults.dimension_name = "minecraft:overworld";
    defaults.max_players = 20;
    defaults.view_distance = 10;
    defaults.simulation_distance = 10;
    defaults.previous_game_mode = -1;
    defaults.sea_level = 63;

    return build_join_game_packet_ex(outbuf, outbuf_size, &defaults);
}
