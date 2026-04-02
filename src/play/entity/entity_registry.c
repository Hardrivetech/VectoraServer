#include "entity_registry.h"

#include <string.h>

static int find_index(const entity_registry_t *registry, int32_t entity_id) {
    if (registry == NULL) {
        return -1;
    }

    for (int i = 0; i < (int)(sizeof(registry->entries) / sizeof(registry->entries[0])); ++i) {
        if (registry->entries[i].active && registry->entries[i].entity_id == entity_id) {
            return i;
        }
    }

    return -1;
}

static int find_free_index(const entity_registry_t *registry) {
    if (registry == NULL) {
        return -1;
    }

    for (int i = 0; i < (int)(sizeof(registry->entries) / sizeof(registry->entries[0])); ++i) {
        if (!registry->entries[i].active) {
            return i;
        }
    }

    return -1;
}

void entity_registry_init(entity_registry_t *registry) {
    if (registry == NULL) {
        return;
    }

    memset(registry, 0, sizeof(*registry));
}

int entity_registry_upsert(entity_registry_t *registry,
                           int32_t entity_id,
                           int kind,
                           double x,
                           double y,
                           double z) {
    int idx;

    if (registry == NULL) {
        return 0;
    }

    idx = find_index(registry, entity_id);
    if (idx < 0) {
        idx = find_free_index(registry);
        if (idx < 0) {
            return 0;
        }
    }

    registry->entries[idx].active = 1;
    registry->entries[idx].entity_id = entity_id;
    registry->entries[idx].kind = kind;
    registry->entries[idx].x = x;
    registry->entries[idx].y = y;
    registry->entries[idx].z = z;
    return 1;
}

int entity_registry_update_xz(entity_registry_t *registry,
                              int32_t entity_id,
                              double x,
                              double z) {
    int idx;

    if (registry == NULL) {
        return 0;
    }

    idx = find_index(registry, entity_id);
    if (idx < 0) {
        return 0;
    }

    registry->entries[idx].x = x;
    registry->entries[idx].z = z;
    return 1;
}

int entity_registry_remove(entity_registry_t *registry, int32_t entity_id) {
    int idx;

    if (registry == NULL) {
        return 0;
    }

    idx = find_index(registry, entity_id);
    if (idx < 0) {
        return 0;
    }

    registry->entries[idx].active = 0;
    return 1;
}

size_t entity_registry_count(const entity_registry_t *registry) {
    size_t count = 0;

    if (registry == NULL) {
        return 0;
    }

    for (size_t i = 0; i < (sizeof(registry->entries) / sizeof(registry->entries[0])); ++i) {
        if (registry->entries[i].active) {
            count += 1;
        }
    }

    return count;
}
