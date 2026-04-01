#ifndef CONFIG_REPLAY_H
#define CONFIG_REPLAY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t **packets;
    size_t *lengths;
    size_t count;
} config_replay_t;

int load_config_replay_from_file(const char *path, config_replay_t *replay, char *error, size_t error_size);
void free_config_replay(config_replay_t *replay);

#endif