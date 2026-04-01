#include "configuration_replay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int append_packet(config_replay_t *replay, const uint8_t *packet, size_t packet_len) {
    uint8_t **new_packets;
    size_t *new_lengths;
    uint8_t *packet_copy;

    packet_copy = (uint8_t *)malloc(packet_len);
    if (packet_copy == NULL) {
        return 0;
    }
    memcpy(packet_copy, packet, packet_len);

    new_packets = (uint8_t **)malloc(sizeof(uint8_t *) * (replay->count + 1));
    if (new_packets == NULL) {
        free(packet_copy);
        return 0;
    }

    new_lengths = (size_t *)malloc(sizeof(size_t) * (replay->count + 1));
    if (new_lengths == NULL) {
        free(new_packets);
        free(packet_copy);
        return 0;
    }

    for (size_t i = 0; i < replay->count; ++i) {
        new_packets[i] = replay->packets[i];
        new_lengths[i] = replay->lengths[i];
    }

    free(replay->packets);
    free(replay->lengths);
    replay->packets = new_packets;
    replay->lengths = new_lengths;
    replay->packets[replay->count] = packet_copy;
    replay->lengths[replay->count] = packet_len;
    replay->count += 1;
    return 1;
}

int load_config_replay_from_file(const char *path, config_replay_t *replay, char *error, size_t error_size) {
    FILE *fp;
    char line[8192];
    size_t line_no = 0;

    if (replay == NULL || path == NULL) {
        set_error(error, error_size, "invalid replay arguments", NULL, 0);
        return 0;
    }

    replay->packets = NULL;
    replay->lengths = NULL;
    replay->count = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        set_error(error, error_size, "could not open replay file", path, 0);
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char hexbuf[8192];
        uint8_t packet[4096];
        size_t hexlen = 0;
        size_t packet_len = 0;
        const char *p = line;

        line_no += 1;

        while (*p != '\0') {
            unsigned char ch = (unsigned char)*p;

            if (ch == '#') {
                break;
            }
            if (isxdigit(ch)) {
                if (hexlen + 1 >= sizeof(hexbuf)) {
                    fclose(fp);
                    free_config_replay(replay);
                    set_error(error, error_size, "hex line too long", path, line_no);
                    return 0;
                }
                hexbuf[hexlen++] = (char)ch;
            } else if (!isspace(ch) && ch != ',' && ch != ':') {
                fclose(fp);
                free_config_replay(replay);
                set_error(error, error_size, "invalid character in replay file", path, line_no);
                return 0;
            }
            p += 1;
        }

        if (hexlen == 0) {
            continue;
        }
        if ((hexlen % 2) != 0) {
            fclose(fp);
            free_config_replay(replay);
            set_error(error, error_size, "odd number of hex digits", path, line_no);
            return 0;
        }

        for (size_t i = 0; i < hexlen; i += 2) {
            char byte_str[3];
            byte_str[0] = hexbuf[i];
            byte_str[1] = hexbuf[i + 1];
            byte_str[2] = '\0';
            packet[packet_len++] = (uint8_t)strtoul(byte_str, NULL, 16);
        }

        if (!append_packet(replay, packet, packet_len)) {
            fclose(fp);
            free_config_replay(replay);
            set_error(error, error_size, "out of memory while loading replay", path, line_no);
            return 0;
        }
    }

    fclose(fp);
    if (replay->count == 0) {
        free_config_replay(replay);
        set_error(error, error_size, "replay file contained no packets", path, 0);
        return 0;
    }

    return 1;
}

void free_config_replay(config_replay_t *replay) {
    if (replay == NULL) {
        return;
    }

    for (size_t i = 0; i < replay->count; ++i) {
        free(replay->packets[i]);
    }
    free(replay->packets);
    free(replay->lengths);
    replay->packets = NULL;
    replay->lengths = NULL;
    replay->count = 0;
}