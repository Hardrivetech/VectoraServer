#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <stddef.h>

enum {
    ONLINE_PLAYERS_MODE_FIXED = 0,
    ONLINE_PLAYERS_MODE_ZERO = 1,
    ONLINE_PLAYERS_MODE_CONNECTED = 2
};

typedef struct {
    int port;
    int max_players;
    int online_players_mode;
    int online_players_display;
    int protocol_number;
    int compression_threshold;
    int view_distance;
    int simulation_distance;
    int force_debug_spawn;
    int enable_real_chunks;
    int allow_debug_chunk_fallback;
    int send_brand_packet;
    int send_wait_for_level_chunks_event;
    int log_packet_framing;
    int log_chunk_sends;
    char server_brand[64];
    char protocol_name[64];
    char motd[256];
    char world_path[512];
} server_config_t;

void set_server_config_defaults(server_config_t *config);
int load_server_config_from_file(const char *path, server_config_t *config, char *error, size_t error_size);
int load_server_config_with_fallbacks(server_config_t *config, const char **loaded_path, char *error, size_t error_size);

#endif