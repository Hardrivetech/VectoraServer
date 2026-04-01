#ifndef CONFIGURATION_PACKETS_H
#define CONFIGURATION_PACKETS_H

#include <stdint.h>
#include <stddef.h>

// Writes minimal Feature Flags packet
size_t build_feature_flags_packet(uint8_t *outbuf, size_t outbuf_size);
// Writes minimal Known Packs packet
size_t build_known_packs_packet(uint8_t *outbuf, size_t outbuf_size);
// Writes minimal Registry Data packet
size_t build_registry_data_packet(uint8_t *outbuf, size_t outbuf_size);
// Writes minimal biome Registry Data packet
size_t build_registry_data_biome_packet(uint8_t *outbuf, size_t outbuf_size);
// Writes Registry Data (0x07) for all 25 required minecraft:damage_type entries
size_t build_registry_data_damage_type(uint8_t *outbuf, size_t outbuf_size);
// Writes minimal Finish Configuration packet
size_t build_finish_config_packet(uint8_t *outbuf, size_t outbuf_size);
// Writes minimal Update Tags packet
size_t build_update_tags_packet(uint8_t *outbuf, size_t outbuf_size);

// Writes Registry Data (0x07) for one named registry entry (data absent = use built-in)
size_t build_registry_data_one(uint8_t *outbuf, size_t outbuf_size, const char *registry, const char *entry);
// Writes Registry Data (0x07) for one named registry entry with inline empty NBT compound
size_t build_registry_data_inline_empty(uint8_t *outbuf, size_t outbuf_size, const char *registry, const char *entry);
// Writes Registry Data (0x07) for one named registry entry with inline {asset_id: "..."} NBT
size_t build_registry_data_with_asset_id(uint8_t *outbuf, size_t outbuf_size, const char *registry, const char *entry, const char *asset_id);
// Writes Update Tags (0x0D) with minecraft:timeline/in_overworld bound to entry 0
size_t build_update_tags_with_timeline(uint8_t *outbuf, size_t outbuf_size);
// Writes minimal Set Compression packet (Login state, 1.21)
size_t build_set_compression_packet(uint8_t *outbuf, size_t outbuf_size, int threshold);

#endif // CONFIGURATION_PACKETS_H
