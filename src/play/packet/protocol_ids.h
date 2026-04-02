#ifndef PROTOCOL_IDS_H
#define PROTOCOL_IDS_H

// Clientbound play packet IDs for protocol 774 (Minecraft 1.21.11).
// Source: https://minecraft.wiki/w/Java_Edition_protocol/Packets (verified 2026-04-02).
// Keep these centralized so protocol migrations are explicit and safer.

// ---- Active packets used by this codebase ----
#define PLAY_PKT_SET_CENTER_CHUNK    0x5C  // set_chunk_cache_center
#define PLAY_PKT_SET_DEFAULT_SPAWN   0x5F  // set_default_spawn_position
#define PLAY_PKT_UPDATE_TIME         0x6F  // set_time
#define PLAY_PKT_GAME_EVENT          0x26  // game_event
#define PLAY_PKT_KEEP_ALIVE          0x2B  // keep_alive
#define PLAY_PKT_PLUGIN_MESSAGE      0x18  // custom_payload

// ---- Verified protocol-774 entity packet IDs ----
// Confirmed against minecraft.wiki protocol 774 / 1.21.11 table.
// Previous values (from stale minecraft-data 1.21.1) were wrong and caused
// client DecoderExceptions. The wiki page confirms this is protocol 774.

#define PLAY774_PKT_BUNDLE_DELIMITER      0x00  // bundle_delimiter
#define PLAY774_PKT_SPAWN_ENTITY          0x01  // add_entity
// 0x02 = animate (Entity Animation) — NOT an orb spawn
#define PLAY774_PKT_ANIMATE               0x02  // animate
#define PLAY774_PKT_ENTITY_POSITION_SYNC  0x23  // entity_position_sync (teleport, >8 blocks)
#define PLAY774_PKT_PLAYER_INFO_REMOVE    0x43  // player_info_remove
#define PLAY774_PKT_PLAYER_INFO_UPDATE    0x44  // player_info_update
#define PLAY774_PKT_SET_HEAD_ROTATION     0x51  // rotate_head
#define PLAY774_PKT_REMOVE_ENTITIES       0x4B  // remove_entities (was wrongly 0x42)
// 0x42 = player_combat_kill (Combat Death) — NOT entity destroy
#define PLAY774_PKT_SET_ENTITY_METADATA   0x61  // set_entity_data
#define PLAY774_PKT_SET_ENTITY_VELOCITY   0x63  // set_entity_motion
#define PLAY774_PKT_MOVE_ENTITY_POS       0x33  // move_entity_pos (delta, <=8 blocks)
#define PLAY774_PKT_MOVE_ENTITY_POS_ROT   0x34  // move_entity_pos_rot
#define PLAY774_PKT_MOVE_ENTITY_ROT       0x36  // move_entity_rot
// Synchronize Vehicle Position (0x7B) is separate from entity_position_sync

#endif
