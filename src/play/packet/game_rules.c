#include "game_rules.h"
#include "packet.h"
#include <string.h>

static size_t write_string(uint8_t *dst, const char *str) {
    size_t len = strlen(str);
    size_t offset = 0;
    offset += write_varint(dst + offset, (int32_t)len);
    memcpy(dst + offset, str, len);
    return offset + len;
}

static size_t write_bool_rule(uint8_t *outbuf, const char *rule_name, int value) {
    size_t offset = 0;
    offset += write_string(outbuf + offset, rule_name);
    outbuf[offset++] = 0x01; // Type: boolean
    outbuf[offset++] = value ? 0x01 : 0x00;
    return offset;
}

static size_t write_int_rule(uint8_t *outbuf, const char *rule_name, int32_t value) {
    size_t offset = 0;
    offset += write_string(outbuf + offset, rule_name);
    outbuf[offset++] = 0x02; // Type: integer
    offset += write_varint(outbuf + offset, value);
    return offset;
}

size_t build_game_rules_packet(uint8_t *outbuf, size_t outbuf_size, const game_rules_t *rules) {
    uint8_t temp_rules[4000];
    size_t temp_offset = 0;
    size_t rules_count = 0;

    (void)outbuf_size;

    if (rules != NULL) {
        // doDaylightCycle
        temp_offset += write_bool_rule(temp_rules + temp_offset, "doDaylightCycle", rules->do_daylight_cycle);
        rules_count++;

        // doMobSpawning
        temp_offset += write_bool_rule(temp_rules + temp_offset, "doMobSpawning", rules->do_mob_spawning);
        rules_count++;

        // doFireTick
        temp_offset += write_bool_rule(temp_rules + temp_offset, "doFireTick", rules->do_fire_tick);
        rules_count++;

        // doEnvironmentDamage
        temp_offset += write_bool_rule(temp_rules + temp_offset, "doEnvironmentDamage", rules->do_environment_damage);
        rules_count++;

        // keepInventory
        temp_offset += write_bool_rule(temp_rules + temp_offset, "keepInventory", rules->keep_inventory);
        rules_count++;

        // doImmediateRespawn
        temp_offset += write_bool_rule(temp_rules + temp_offset, "doImmediateRespawn", rules->do_immediate_respawn);
        rules_count++;

        // showDeathMessages
        temp_offset += write_bool_rule(temp_rules + temp_offset, "showDeathMessages", rules->show_death_messages);
        rules_count++;

        // sendCommandFeedback
        temp_offset += write_bool_rule(temp_rules + temp_offset, "sendCommandFeedback", rules->send_command_feedback);
        rules_count++;

        // logAdminCommands
        temp_offset += write_bool_rule(temp_rules + temp_offset, "logAdminCommands", rules->log_admin_commands);
        rules_count++;

        // announceAdvancements
        temp_offset += write_bool_rule(temp_rules + temp_offset, "announceAdvancements", rules->announce_advancements);
        rules_count++;

        // disableElytraMovementCheck
        temp_offset += write_bool_rule(temp_rules + temp_offset, "disableElytraMovementCheck", rules->disable_elytra_movement_check);
        rules_count++;

        // maxCommandChainLength
        temp_offset += write_int_rule(temp_rules + temp_offset, "maxCommandChainLength", rules->max_command_chain_length);
        rules_count++;

        // maxEntityCramming
        temp_offset += write_int_rule(temp_rules + temp_offset, "maxEntityCramming", rules->max_entity_cramming);
        rules_count++;

        // randomTickSpeed
        temp_offset += write_int_rule(temp_rules + temp_offset, "randomTickSpeed", rules->random_tick_speed);
        rules_count++;
    }

    // Now build the final packet with proper framing
    size_t offset = 0;
    offset += write_varint(outbuf + offset, 0x5D);  // Packet ID
    offset += write_varint(outbuf + offset, (int32_t)rules_count);  // Rule count
    memcpy(outbuf + offset, temp_rules, temp_offset);  // Rule data
    offset += temp_offset;

    return offset;
}

void get_default_game_rules(game_rules_t *rules) {
    if (rules == NULL) {
        return;
    }

    rules->do_daylight_cycle = 1;
    rules->do_mob_spawning = 1;
    rules->do_fire_tick = 1;
    rules->do_environment_damage = 1;
    rules->keep_inventory = 0;
    rules->do_immediate_respawn = 0;
    rules->show_death_messages = 1;
    rules->send_command_feedback = 1;
    rules->log_admin_commands = 1;
    rules->announce_advancements = 1;
    rules->disable_elytra_movement_check = 0;
    rules->max_command_chain_length = 65536;
    rules->max_entity_cramming = 24;
    rules->random_tick_speed = 3;
}
