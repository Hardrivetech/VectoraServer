#ifndef GAME_RULES_H
#define GAME_RULES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int do_daylight_cycle;
    int do_mob_spawning;
    int do_fire_tick;
    int do_environment_damage;
    int keep_inventory;
    int do_immediate_respawn;
    int show_death_messages;
    int send_command_feedback;
    int log_admin_commands;
    int announce_advancements;
    int disable_elytra_movement_check;
    int max_command_chain_length;
    int max_entity_cramming;
    int random_tick_speed;
} game_rules_t;

// Build a Game Rules packet with all configured rules
// Returns the packet length
size_t build_game_rules_packet(uint8_t *outbuf, size_t outbuf_size, const game_rules_t *rules);

// Create default game rules
void get_default_game_rules(game_rules_t *rules);

#endif
