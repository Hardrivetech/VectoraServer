#include "server_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static FILE *open_file_read_binary(const char *path) {
#ifdef _WIN32
    FILE *fp = NULL;
    if (fopen_s(&fp, path, "rb") != 0) {
        return NULL;
    }
    return fp;
#else
    return fopen(path, "rb");
#endif
}

static char *dup_env_value(const char *name) {
#ifdef _WIN32
    char *value = NULL;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0) {
        return NULL;
    }
    return value;
#else
    const char *value = getenv(name);
    if (value == NULL) {
        return NULL;
    }
    return strdup(value);
#endif
}

static void set_error(char *error, size_t error_size, const char *message, const char *path, size_t line_no) {
    if (error == NULL || error_size == 0) {
        return;
    }

    if (path != NULL && line_no != 0) {
        snprintf(error, error_size, "%s (%s:%zu)", message, path, line_no);
        return;
    }

    if (path != NULL) {
        snprintf(error, error_size, "%s (%s)", message, path);
        return;
    }

    snprintf(error, error_size, "%s", message);
}

static char *trim_left(char *text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text += 1;
    }
    return text;
}

static void trim_right(char *text) {
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        len -= 1;
    }
}

static void strip_inline_comment(char *text) {
    while (*text != '\0') {
        if (*text == '#' || *text == ';') {
            *text = '\0';
            break;
        }
        text += 1;
    }
}

static int parse_bool_value(const char *value, int *out_value) {
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
        *out_value = 1;
        return 1;
    }

    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 || strcmp(value, "off") == 0) {
        *out_value = 0;
        return 1;
    }

    return 0;
}

static int parse_int_in_range(const char *value, int min_value, int max_value, int *out_value) {
    char *end = NULL;
    long parsed;

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return 0;
    }
    if (parsed < min_value || parsed > max_value) {
        return 0;
    }

    *out_value = (int)parsed;
    return 1;
}

static int parse_string_value(const char *value, char *out_value, size_t out_value_size) {
    size_t len;

    if (value == NULL || out_value == NULL || out_value_size == 0) {
        return 0;
    }

    len = strlen(value);
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        value += 1;
        len -= 2;
    }

    if (len == 0 || len >= out_value_size) {
        return 0;
    }

    memcpy(out_value, value, len);
    out_value[len] = '\0';
    return 1;
}

static int assign_config_value(server_config_t *config, const char *key, const char *value) {
    int parsed = 0;

    if (strcmp(key, "port") == 0) {
        return parse_int_in_range(value, 1, 65535, &config->port);
    }
    if (strcmp(key, "max_players") == 0) {
        return parse_int_in_range(value, 1, 1000, &config->max_players);
    }
    if (strcmp(key, "max_connections") == 0) {
        return parse_int_in_range(value, 1, 10000, &config->max_connections);
    }
    if (strcmp(key, "online_players_mode") == 0) {
        if (strcmp(value, "fixed") == 0) {
            config->online_players_mode = ONLINE_PLAYERS_MODE_FIXED;
            return 1;
        }
        if (strcmp(value, "zero") == 0) {
            config->online_players_mode = ONLINE_PLAYERS_MODE_ZERO;
            return 1;
        }
        if (strcmp(value, "connected") == 0) {
            config->online_players_mode = ONLINE_PLAYERS_MODE_CONNECTED;
            return 1;
        }
        return 0;
    }
    if (strcmp(key, "online_players_display") == 0) {
        return parse_int_in_range(value, 0, 1000, &config->online_players_display);
    }
    if (strcmp(key, "protocol_number") == 0) {
        return parse_int_in_range(value, 0, 1000000, &config->protocol_number);
    }
    if (strcmp(key, "compression_threshold") == 0) {
        return parse_int_in_range(value, 0, 1048576, &config->compression_threshold);
    }
    if (strcmp(key, "chunk_stream_radius") == 0) {
        return parse_int_in_range(value, 0, 12, &config->chunk_stream_radius);
    }
    if (strcmp(key, "keep_alive_interval_seconds") == 0) {
        return parse_int_in_range(value, 1, 300, &config->keep_alive_interval_seconds);
    }
    if (strcmp(key, "play_idle_timeout_seconds") == 0) {
        return parse_int_in_range(value, 0, 3600, &config->play_idle_timeout_seconds);
    }
    if (strcmp(key, "serverbound_keep_alive_packet_id") == 0) {
        return parse_int_in_range(value, 0, 255, &config->serverbound_keep_alive_packet_id);
    }
    if (strcmp(key, "idle_position_epsilon_milliblocks") == 0) {
        return parse_int_in_range(value, 0, 10000, &config->idle_position_epsilon_milliblocks);
    }
    if (strcmp(key, "play_disconnect_packet_id") == 0) {
        return parse_int_in_range(value, 0, 255, &config->play_disconnect_packet_id);
    }
    if (strcmp(key, "view_distance") == 0) {
        return parse_int_in_range(value, 2, 32, &config->view_distance);
    }
    if (strcmp(key, "simulation_distance") == 0) {
        return parse_int_in_range(value, 2, 32, &config->simulation_distance);
    }
    if (strcmp(key, "game_mode") == 0) {
        return parse_int_in_range(value, 0, 3, &config->game_mode);
    }
    if (strcmp(key, "difficulty") == 0) {
        return parse_int_in_range(value, 0, 3, &config->difficulty);
    }
    if (strcmp(key, "is_hardcore") == 0) {
        return parse_bool_value(value, &config->is_hardcore);
    }
    if (strcmp(key, "pvp_enabled") == 0) {
        return parse_bool_value(value, &config->pvp_enabled);
    }
    if (strcmp(key, "spawn_protection_radius") == 0) {
        return parse_int_in_range(value, 0, 1000, &config->spawn_protection_radius);
    }
    if (strcmp(key, "game_event_respawn_screen_value") == 0) {
        return parse_int_in_range(value, 0, 1, &config->game_event_respawn_screen_value);
    }
    if (strcmp(key, "game_event_limited_crafting_value") == 0) {
        return parse_int_in_range(value, 0, 1, &config->game_event_limited_crafting_value);
    }
    if (strcmp(key, "rule_do_daylight_cycle") == 0) {
        return parse_bool_value(value, &config->game_rules.do_daylight_cycle);
    }
    if (strcmp(key, "rule_do_mob_spawning") == 0) {
        return parse_bool_value(value, &config->game_rules.do_mob_spawning);
    }
    if (strcmp(key, "rule_do_fire_tick") == 0) {
        return parse_bool_value(value, &config->game_rules.do_fire_tick);
    }
    if (strcmp(key, "rule_do_environment_damage") == 0) {
        return parse_bool_value(value, &config->game_rules.do_environment_damage);
    }
    if (strcmp(key, "rule_keep_inventory") == 0) {
        return parse_bool_value(value, &config->game_rules.keep_inventory);
    }
    if (strcmp(key, "rule_do_immediate_respawn") == 0) {
        return parse_bool_value(value, &config->game_rules.do_immediate_respawn);
    }
    if (strcmp(key, "rule_show_death_messages") == 0) {
        return parse_bool_value(value, &config->game_rules.show_death_messages);
    }
    if (strcmp(key, "rule_send_command_feedback") == 0) {
        return parse_bool_value(value, &config->game_rules.send_command_feedback);
    }
    if (strcmp(key, "rule_log_admin_commands") == 0) {
        return parse_bool_value(value, &config->game_rules.log_admin_commands);
    }
    if (strcmp(key, "rule_announce_advancements") == 0) {
        return parse_bool_value(value, &config->game_rules.announce_advancements);
    }
    if (strcmp(key, "rule_disable_elytra_movement_check") == 0) {
        return parse_bool_value(value, &config->game_rules.disable_elytra_movement_check);
    }
    if (strcmp(key, "rule_max_command_chain_length") == 0) {
        return parse_int_in_range(value, 0, 2147483647, &config->game_rules.max_command_chain_length);
    }
    if (strcmp(key, "rule_max_entity_cramming") == 0) {
        return parse_int_in_range(value, 0, 1000, &config->game_rules.max_entity_cramming);
    }
    if (strcmp(key, "rule_random_tick_speed") == 0) {
        return parse_int_in_range(value, 0, 1000, &config->game_rules.random_tick_speed);
    }
    if (strcmp(key, "server_brand") == 0) {
        return parse_string_value(value, config->server_brand, sizeof(config->server_brand));
    }
    if (strcmp(key, "protocol_name") == 0) {
        return parse_string_value(value, config->protocol_name, sizeof(config->protocol_name));
    }
    if (strcmp(key, "motd") == 0) {
        return parse_string_value(value, config->motd, sizeof(config->motd));
    }
    if (strcmp(key, "idle_disconnect_reason") == 0) {
        return parse_string_value(value, config->idle_disconnect_reason, sizeof(config->idle_disconnect_reason));
    }
    if (strcmp(key, "world_path") == 0) {
        return parse_string_value(value, config->world_path, sizeof(config->world_path));
    }

    if (!parse_bool_value(value, &parsed)) {
        return 0;
    }

    if (strcmp(key, "force_debug_spawn") == 0) {
        config->force_debug_spawn = parsed;
        return 1;
    }
    if (strcmp(key, "enable_real_chunks") == 0) {
        config->enable_real_chunks = parsed;
        return 1;
    }
    if (strcmp(key, "allow_debug_chunk_fallback") == 0) {
        config->allow_debug_chunk_fallback = parsed;
        return 1;
    }
    if (strcmp(key, "send_brand_packet") == 0) {
        config->send_brand_packet = parsed;
        return 1;
    }
    if (strcmp(key, "enable_experimental_entities") == 0) {
        config->enable_experimental_entities = parsed;
        return 1;
    }
    if (strcmp(key, "enable_experimental_entity_packets") == 0) {
        config->enable_experimental_entity_packets = parsed;
        return 1;
    }
    if (strcmp(key, "send_game_rules_packet") == 0) {
        config->send_game_rules_packet = parsed;
        return 1;
    }
    if (strcmp(key, "send_wait_for_level_chunks_event") == 0) {
        config->send_wait_for_level_chunks_event = parsed;
        return 1;
    }
    if (strcmp(key, "send_idle_disconnect_packet") == 0) {
        config->send_idle_disconnect_packet = parsed;
        return 1;
    }
    if (strcmp(key, "idle_timeout_counts_keep_alive") == 0) {
        config->idle_timeout_counts_keep_alive = parsed;
        return 1;
    }
    if (strcmp(key, "idle_timeout_requires_position_change") == 0) {
        config->idle_timeout_requires_position_change = parsed;
        return 1;
    }
    if (strcmp(key, "reject_protocol_mismatch") == 0) {
        config->reject_protocol_mismatch = parsed;
        return 1;
    }
    if (strcmp(key, "log_packet_framing") == 0) {
        config->log_packet_framing = parsed;
        return 1;
    }
    if (strcmp(key, "log_play_packets") == 0) {
        config->log_play_packets = parsed;
        return 1;
    }
    if (strcmp(key, "log_play_session_summary") == 0) {
        config->log_play_session_summary = parsed;
        return 1;
    }
    if (strcmp(key, "log_entity_events") == 0) {
        config->log_entity_events = parsed;
        return 1;
    }
    if (strcmp(key, "log_chunk_sends") == 0) {
        config->log_chunk_sends = parsed;
        return 1;
    }
    if (strcmp(key, "offline_mode") == 0) {
        config->offline_mode = parsed;
        return 1;
    }

    return 0;
}

static int file_exists(const char *path) {
    FILE *fp = open_file_read_binary(path);
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static void path_dirname(char *path) {
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    len = strlen(path);
    while (len > 0) {
        char ch = path[len - 1];
        if (ch == '/' || ch == '\\') {
            path[len - 1] = '\0';
            return;
        }
        len -= 1;
    }

    path[0] = '\0';
}

static int get_executable_dir(char *buffer, size_t buffer_size) {
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (length == 0 || length >= buffer_size) {
        return 0;
    }
    path_dirname(buffer);
    return buffer[0] != '\0';
#else
    ssize_t length = readlink("/proc/self/exe", buffer, buffer_size - 1);
    if (length <= 0 || (size_t)length >= buffer_size) {
        return 0;
    }
    buffer[length] = '\0';
    path_dirname(buffer);
    return buffer[0] != '\0';
#endif
}

void set_server_config_defaults(server_config_t *config) {
    if (config == NULL) {
        return;
    }

    config->port = 25565;
    config->max_players = 20;
    config->max_connections = 100;
    config->online_players_mode = ONLINE_PLAYERS_MODE_FIXED;
    config->online_players_display = 0;
    config->protocol_number = 774;
    config->compression_threshold = 256;
    config->chunk_stream_radius = 0;
    config->keep_alive_interval_seconds = 10;
    config->play_idle_timeout_seconds = 30;
    config->idle_timeout_counts_keep_alive = 0;
    config->serverbound_keep_alive_packet_id = 0x13;
    config->idle_timeout_requires_position_change = 1;
    config->idle_position_epsilon_milliblocks = 50;
    config->send_idle_disconnect_packet = 0;
    config->play_disconnect_packet_id = 0x1A;
    config->view_distance = 10;
    config->simulation_distance = 10;
    config->game_mode = 0;
    config->difficulty = 2;
    config->is_hardcore = 0;
    config->pvp_enabled = 1;
    config->spawn_protection_radius = 16;
    get_default_game_rules(&config->game_rules);
    config->force_debug_spawn = 0;
    config->enable_real_chunks = 1;
    config->allow_debug_chunk_fallback = 1;
    config->send_brand_packet = 1;
    config->enable_experimental_entities = 0;
    config->enable_experimental_entity_packets = 0;
    config->send_game_rules_packet = 0;
    config->send_wait_for_level_chunks_event = 1;
    config->game_event_respawn_screen_value = 0;
    config->game_event_limited_crafting_value = 0;
    config->reject_protocol_mismatch = 1;
    config->log_packet_framing = 1;
    config->log_play_packets = 0;
    config->log_play_session_summary = 1;
    config->log_entity_events = 0;
    config->log_chunk_sends = 1;
    config->offline_mode = 1;
    snprintf(config->server_brand, sizeof(config->server_brand), "%s", "Vectora");
    snprintf(config->protocol_name, sizeof(config->protocol_name), "%s", "Vectora 1.21.11");
    snprintf(config->motd, sizeof(config->motd), "%s", "Welcome to Vectora!");
    snprintf(config->idle_disconnect_reason, sizeof(config->idle_disconnect_reason), "%s", "Timed out due to inactivity.");
    config->world_path[0] = '\0';
}

int load_server_config_from_file(const char *path, server_config_t *config, char *error, size_t error_size) {
    FILE *fp;
    char line[512];
    size_t line_no = 0;

    if (path == NULL || config == NULL) {
        set_error(error, error_size, "invalid server config arguments", NULL, 0);
        return 0;
    }

    fp = open_file_read_binary(path);
    if (fp == NULL) {
        set_error(error, error_size, "could not open server config", path, 0);
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key;
        char *value;
        char *equals;

        line_no += 1;
        strip_inline_comment(line);
        key = trim_left(line);
        trim_right(key);

        if (key[0] == '\0') {
            continue;
        }

        equals = strchr(key, '=');
        if (equals == NULL) {
            fclose(fp);
            set_error(error, error_size, "expected key=value entry", path, line_no);
            return 0;
        }

        *equals = '\0';
        value = trim_left(equals + 1);
        trim_right(key);
        trim_right(value);
        key = trim_left(key);

        if (key[0] == '\0' || value[0] == '\0') {
            fclose(fp);
            set_error(error, error_size, "invalid key=value entry", path, line_no);
            return 0;
        }

        if (!assign_config_value(config, key, value)) {
            fclose(fp);
            set_error(error, error_size, "unknown or invalid server config option", path, line_no);
            return 0;
        }
    }

    fclose(fp);
    return 1;
}

int load_server_config_with_fallbacks(server_config_t *config, const char **loaded_path, char *error, size_t error_size) {
    static const char *fallback_paths[] = {
        "vectora.cfg",
        "config/vectora.cfg",
        "../vectora.cfg",
        "../config/vectora.cfg"
    };
    static const char *executable_relative_paths[] = {
        "vectora.cfg",
        "config/vectora.cfg",
        "../vectora.cfg",
        "../config/vectora.cfg",
        "../../vectora.cfg",
        "../../config/vectora.cfg",
        "../../../vectora.cfg",
        "../../../config/vectora.cfg"
    };
    static char resolved_path[1024];
    char *env_path;

    if (loaded_path != NULL) {
        *loaded_path = NULL;
    }

    set_server_config_defaults(config);

    env_path = dup_env_value("VECTORA_SERVER_CONFIG");
    if (env_path != NULL && env_path[0] != '\0') {
        if (!load_server_config_from_file(env_path, config, error, error_size)) {
            free(env_path);
            return -1;
        }
        if (loaded_path != NULL) {
            *loaded_path = env_path;
        } else {
            free(env_path);
        }
        return 1;
    }

    free(env_path);

    for (size_t i = 0; i < sizeof(fallback_paths) / sizeof(fallback_paths[0]); ++i) {
        if (!file_exists(fallback_paths[i])) {
            continue;
        }
        if (!load_server_config_from_file(fallback_paths[i], config, error, error_size)) {
            return -1;
        }
        if (loaded_path != NULL) {
            *loaded_path = fallback_paths[i];
        }
        return 1;
    }

    {
        char executable_dir[1024];
        if (get_executable_dir(executable_dir, sizeof(executable_dir))) {
            for (size_t i = 0; i < sizeof(executable_relative_paths) / sizeof(executable_relative_paths[0]); ++i) {
                snprintf(resolved_path, sizeof(resolved_path), "%s/%s", executable_dir, executable_relative_paths[i]);
                if (!file_exists(resolved_path)) {
                    continue;
                }
                if (!load_server_config_from_file(resolved_path, config, error, error_size)) {
                    return -1;
                }
                if (loaded_path != NULL) {
                    *loaded_path = resolved_path;
                }
                return 1;
            }
        }
    }

    return 0;
}