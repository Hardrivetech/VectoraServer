#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <stddef.h>
#include "../../play/packet/game_rules.h"

enum {
    ONLINE_PLAYERS_MODE_FIXED = 0,
    ONLINE_PLAYERS_MODE_ZERO = 1,
    ONLINE_PLAYERS_MODE_CONNECTED = 2
};

typedef struct {
    int port;
    int max_players;
    int max_connections;
    int online_players_mode;
    int online_players_display;
    int protocol_number;
    int compression_threshold;
    int chunk_stream_radius;
    int keep_alive_interval_seconds;
    int play_idle_timeout_seconds;
    int idle_timeout_counts_keep_alive;
    int serverbound_keep_alive_packet_id;
    int idle_timeout_requires_position_change;
    int idle_position_epsilon_milliblocks;
    int send_idle_disconnect_packet;
    int play_disconnect_packet_id;
    int view_distance;
    int simulation_distance;
    int game_mode;
    int difficulty;
    int is_hardcore;
    int pvp_enabled;
    int spawn_protection_radius;
    game_rules_t game_rules;
    int force_debug_spawn;
    int enable_real_chunks;
    int allow_debug_chunk_fallback;
    int send_brand_packet;
    int enable_experimental_entities;
    int enable_experimental_entity_packets;
    int send_game_rules_packet;
    int send_wait_for_level_chunks_event;
    int game_event_respawn_screen_value;
    int game_event_limited_crafting_value;
    int reject_protocol_mismatch;
    int log_packet_framing;
    int log_play_packets;
    int log_play_session_summary;
    int log_entity_events;
    int log_chunk_sends;
    int offline_mode;
    char server_brand[64];
    char protocol_name[64];
    char motd[256];
    char idle_disconnect_reason[256];
    char world_path[512];
} server_config_t;

void set_server_config_defaults(server_config_t *config);
int load_server_config_from_file(const char *path, server_config_t *config, char *error, size_t error_size);
int load_server_config_with_fallbacks(server_config_t *config, const char **loaded_path, char *error, size_t error_size);

#endif