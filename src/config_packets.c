#include "config_packets.h"
#include "packet.h"
#include <string.h>
#include <stdio.h>

static size_t write_mc_string(uint8_t *outbuf, const char *s) {
    size_t slen = strlen(s);
    size_t off = 0;
    off += write_varint(outbuf + off, (int)slen);
    memcpy(outbuf + off, s, slen);
    off += slen;
    return off;
}

size_t build_known_packs_packet(uint8_t *outbuf, size_t outbuf_size) {
    // Packet ID for Clientbound Known Packs (1.21): 0x0E
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x0E);
    offset += write_varint(outbuf + offset, 1); // one known pack
    offset += write_mc_string(outbuf + offset, "minecraft");
    offset += write_mc_string(outbuf + offset, "core");
    offset += write_mc_string(outbuf + offset, "1.21.11");
    return offset;
}

size_t build_feature_flags_packet(uint8_t *outbuf, size_t outbuf_size) {
    // Packet ID for Feature Flags (1.21): 0x0C
    // Minimal: 1 feature: "minecraft:vanilla"
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x0C);
    offset += write_varint(outbuf + offset, 1); // Feature count
    // Write "minecraft:vanilla" as MC String
    const char *flag = "minecraft:vanilla";
    size_t flag_len = strlen(flag);
    offset += write_varint(outbuf + offset, (int)flag_len);
    memcpy(outbuf + offset, flag, flag_len);
    offset += flag_len;
    return offset;
}

size_t build_registry_data_packet(uint8_t *outbuf, size_t outbuf_size) {
    // Packet ID for Registry Data (1.21): 0x07
    // This builds the dimension_type registry packet.
    uint8_t packet[256];
    size_t offset = 0;

    // --- First registry: dimension_type ---
    offset += write_varint(packet + offset, 0x07); // Packet ID
    const char *regkey1 = "minecraft:dimension_type";
    size_t regkey1_len = strlen(regkey1);
    offset += write_varint(packet + offset, (int)regkey1_len);
    memcpy(packet + offset, regkey1, regkey1_len);
    offset += regkey1_len;
    offset += write_varint(packet + offset, 1); // 1 entry
    const char *entrykey1 = "minecraft:overworld";
    size_t entrykey1_len = strlen(entrykey1);
    offset += write_varint(packet + offset, (int)entrykey1_len);
    memcpy(packet + offset, entrykey1, entrykey1_len);
    offset += entrykey1_len;
    packet[offset++] = 0; // Data absent (use known built-in data)

    // Return raw packet bytes; caller applies post-compression framing.
    memcpy(outbuf, packet, offset);
    return offset;
}

size_t build_registry_data_biome_packet(uint8_t *outbuf, size_t outbuf_size) {
    // Registry Data 0x07: minecraft:biome with inline plains NBT.
    // Biomes are resolved to Holders during ClientLevel init via getOrThrow(), so
    // data_absent=0x00 only works when both sides confirmed the exact same core pack.
    // Sending inline NBT is unconditionally safe.
    uint8_t packet[256];
    size_t offset = 0;

    offset += write_varint(packet + offset, 0x07);
    const char *regkey = "minecraft:worldgen/biome"; // protocol wire name (not the shorthand)
    size_t regkey_len = strlen(regkey);
    offset += write_varint(packet + offset, (int)regkey_len);
    memcpy(packet + offset, regkey, regkey_len);
    offset += regkey_len;

    offset += write_varint(packet + offset, 1); // 1 entry
    const char *entrykey = "minecraft:plains";
    size_t entrykey_len = strlen(entrykey);
    offset += write_varint(packet + offset, (int)entrykey_len);
    memcpy(packet + offset, entrykey, entrykey_len);
    offset += entrykey_len;

    packet[offset++] = 0x01; // has_data = true — inline NBT follows

    // --- Network NBT (no root name in 1.20.2+ network format) ---
    packet[offset++] = 0x0A; // TAG_Compound root

    // TAG_Byte "has_precipitation" = 1
    packet[offset++] = 0x01; packet[offset++] = 0x00; packet[offset++] = 0x11;
    memcpy(packet + offset, "has_precipitation", 17); offset += 17;
    packet[offset++] = 0x01;

    // TAG_Float "temperature" = 0.8f (0x3F4CCCCD big-endian)
    packet[offset++] = 0x05; packet[offset++] = 0x00; packet[offset++] = 0x0B;
    memcpy(packet + offset, "temperature", 11); offset += 11;
    packet[offset++] = 0x3F; packet[offset++] = 0x4C; packet[offset++] = 0xCC; packet[offset++] = 0xCD;

    // TAG_Float "downfall" = 0.4f (0x3ECCCCCD big-endian)
    packet[offset++] = 0x05; packet[offset++] = 0x00; packet[offset++] = 0x08;
    memcpy(packet + offset, "downfall", 8); offset += 8;
    packet[offset++] = 0x3E; packet[offset++] = 0xCC; packet[offset++] = 0xCC; packet[offset++] = 0xCD;

    // TAG_Compound "effects"
    packet[offset++] = 0x0A; packet[offset++] = 0x00; packet[offset++] = 0x07;
    memcpy(packet + offset, "effects", 7); offset += 7;

    // TAG_Int "fog_color" = 12638463 (0x00C0D8FF)
    packet[offset++] = 0x03; packet[offset++] = 0x00; packet[offset++] = 0x09;
    memcpy(packet + offset, "fog_color", 9); offset += 9;
    packet[offset++] = 0x00; packet[offset++] = 0xC0; packet[offset++] = 0xD8; packet[offset++] = 0xFF;

    // TAG_Int "water_color" = 4159204 (0x003F76E4)
    packet[offset++] = 0x03; packet[offset++] = 0x00; packet[offset++] = 0x0B;
    memcpy(packet + offset, "water_color", 11); offset += 11;
    packet[offset++] = 0x00; packet[offset++] = 0x3F; packet[offset++] = 0x76; packet[offset++] = 0xE4;

    // TAG_Int "water_fog_color" = 329011 (0x00050533)
    packet[offset++] = 0x03; packet[offset++] = 0x00; packet[offset++] = 0x0F;
    memcpy(packet + offset, "water_fog_color", 15); offset += 15;
    packet[offset++] = 0x00; packet[offset++] = 0x05; packet[offset++] = 0x05; packet[offset++] = 0x33;

    // TAG_Int "sky_color" = 7907327 (0x0078A7FF)
    packet[offset++] = 0x03; packet[offset++] = 0x00; packet[offset++] = 0x09;
    memcpy(packet + offset, "sky_color", 9); offset += 9;
    packet[offset++] = 0x00; packet[offset++] = 0x78; packet[offset++] = 0xA7; packet[offset++] = 0xFF;

    packet[offset++] = 0x00; // TAG_End (close effects)
    packet[offset++] = 0x00; // TAG_End (close root)
    // --- End NBT --- (total ~174 bytes, well within 256-byte local buffer)

    memcpy(outbuf, packet, offset);
    return offset;
}

size_t build_registry_data_damage_type(uint8_t *outbuf, size_t outbuf_size) {
    // Registry Data 0x07: minecraft:damage_type, all 25 entries required by the client
    // before it accepts a Login (play) packet. Data absent — sourced from core pack.
    static const char *entries[] = {
        "minecraft:cactus",
        "minecraft:campfire",
        "minecraft:cramming",
        "minecraft:dragon_breath",
        "minecraft:drown",
        "minecraft:dry_out",
        "minecraft:ender_pearl",
        "minecraft:fall",
        "minecraft:fly_into_wall",
        "minecraft:freeze",
        "minecraft:generic",
        "minecraft:generic_kill",
        "minecraft:hot_floor",
        "minecraft:in_fire",
        "minecraft:in_wall",
        "minecraft:lava",
        "minecraft:lightning_bolt",
        "minecraft:magic",
        "minecraft:on_fire",
        "minecraft:out_of_world",
        "minecraft:outside_border",
        "minecraft:stalagmite",
        "minecraft:starve",
        "minecraft:sweet_berry_bush",
        "minecraft:wither",
    };
    const int num_entries = 25;
    uint8_t packet[1024];
    size_t offset = 0;
    offset += write_varint(packet + offset, 0x07);
    offset += write_mc_string(packet + offset, "minecraft:damage_type");
    offset += write_varint(packet + offset, num_entries);
    for (int i = 0; i < num_entries; i++) {
        offset += write_mc_string(packet + offset, entries[i]);
        packet[offset++] = 0x00; // data absent — client loads from core pack
    }
    memcpy(outbuf, packet, offset);
    return offset;
}

size_t build_finish_config_packet(uint8_t *outbuf, size_t outbuf_size) {
    // Packet ID for Finish Configuration (1.21): 0x03
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x03);
    return offset;
}

size_t build_update_tags_packet(uint8_t *outbuf, size_t outbuf_size) {
    // Packet ID for Update Tags (configuration, 1.21): 0x0D
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x0D);
    offset += write_varint(outbuf + offset, 0); // zero tagged registries
    return offset;
}

size_t build_registry_data_one(uint8_t *outbuf, size_t outbuf_size,
                               const char *registry, const char *entry) {
    // Packet ID 0x07: one-entry registry, data absent (client uses built-in from known packs)
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x07);
    offset += write_mc_string(outbuf + offset, registry);
    offset += write_varint(outbuf + offset, 1); // 1 entry
    offset += write_mc_string(outbuf + offset, entry);
    outbuf[offset++] = 0x00; // data absent
    return offset;
}

size_t build_registry_data_inline_empty(uint8_t *outbuf, size_t outbuf_size,
                                        const char *registry, const char *entry) {
    // Packet ID 0x07: one-entry registry with inline data.
    // Sends an empty NBT compound (has_data=0x01, compound type, TAG_End).
    // Used for registries whose data is NOT present in the vanilla core pack
    // so data_absent=0x00 would fail to load on the client.
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x07);
    offset += write_mc_string(outbuf + offset, registry);
    offset += write_varint(outbuf + offset, 1); // 1 entry
    offset += write_mc_string(outbuf + offset, entry);
    outbuf[offset++] = 0x01; // has_data = true
    outbuf[offset++] = 0x0A; // TAG_Compound (network NBT: no root name)
    outbuf[offset++] = 0x00; // TAG_End (empty compound)
    return offset;
}

size_t build_registry_data_with_asset_id(uint8_t *outbuf, size_t outbuf_size,
                                          const char *registry, const char *entry,
                                          const char *asset_id) {
    // Packet ID 0x07: one-entry registry with inline NBT compound {asset_id: "..."}.
    // Required for registries added in 1.21.5+ whose codec demands at least asset_id.
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x07);
    offset += write_mc_string(outbuf + offset, registry);
    offset += write_varint(outbuf + offset, 1); // 1 entry
    offset += write_mc_string(outbuf + offset, entry);
    outbuf[offset++] = 0x01; // has_data = true
    // Network NBT: TAG_Compound (no root name in 1.20.2+ network format)
    outbuf[offset++] = 0x0A;
    // TAG_String named "asset_id"
    outbuf[offset++] = 0x08; // TAG_String
    const char *field_name = "asset_id";
    uint16_t fn_len = (uint16_t)strlen(field_name);
    outbuf[offset++] = (uint8_t)(fn_len >> 8);
    outbuf[offset++] = (uint8_t)(fn_len & 0xFF);
    memcpy(outbuf + offset, field_name, fn_len);
    offset += fn_len;
    uint16_t val_len = (uint16_t)strlen(asset_id);
    outbuf[offset++] = (uint8_t)(val_len >> 8);
    outbuf[offset++] = (uint8_t)(val_len & 0xFF);
    memcpy(outbuf + offset, asset_id, val_len);
    offset += val_len;
    outbuf[offset++] = 0x00; // TAG_End (close compound)
    return offset;
}

size_t build_update_tags_with_timeline(uint8_t *outbuf, size_t outbuf_size) {
    // Packet ID 0x0D: 1 tagged registry (minecraft:timeline),
    // tag minecraft:in_overworld bound to entry 0 (minecraft:overworld)
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x0D);
    offset += write_varint(outbuf + offset, 1); // 1 tagged registry
    offset += write_mc_string(outbuf + offset, "minecraft:timeline");
    offset += write_varint(outbuf + offset, 1); // 1 tag
    offset += write_mc_string(outbuf + offset, "minecraft:in_overworld");
    offset += write_varint(outbuf + offset, 1); // 1 entry
    offset += write_varint(outbuf + offset, 0); // entry ID 0 = minecraft:overworld
    return offset;
}

// Writes minimal Set Compression packet (Login state, 1.21, ID 0x03)
size_t build_set_compression_packet(uint8_t *outbuf, size_t outbuf_size, int threshold) {
    printf("[DEBUG] build_set_compression_packet called: outbuf=%p, outbuf_size=%zu, threshold=%d\n", (void*)outbuf, outbuf_size, threshold);
    // Packet ID for Set Compression (Login state, 1.21): 0x03
    uint8_t packet[8];
    size_t offset = 0;
    offset += write_varint(packet + offset, 0x03);
    offset += write_varint(packet + offset, threshold); // Compression threshold
    size_t outlen = write_varint(outbuf, (int)offset);
    memcpy(outbuf + outlen, packet, offset);
    outlen += offset;
    return outlen;
}
