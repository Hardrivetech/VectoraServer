#include "entity_manager.h"

#include <string.h>

static int push_event(entity_manager_t *manager,
                      int event_kind,
                      int32_t entity_id,
                      int entity_kind,
                      double x,
                      double y,
                      double z) {
    entity_event_t *evt;

    if (manager == NULL) {
        return 0;
    }

    if (manager->pending_count >= (sizeof(manager->pending) / sizeof(manager->pending[0]))) {
        manager->dropped_events += 1;
        return 0;
    }

    evt = &manager->pending[manager->pending_count++];
    evt->event_kind = event_kind;
    evt->entity_id = entity_id;
    evt->entity_kind = entity_kind;
    evt->x = x;
    evt->y = y;
    evt->z = z;
    return 1;
}

void entity_manager_init(entity_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    memset(manager, 0, sizeof(*manager));
    manager->next_entity_id = 1000;
}

int entity_manager_queue_spawn(entity_registry_t *registry,
                               entity_manager_t *manager,
                               int32_t entity_id,
                               int entity_kind,
                               double x,
                               double y,
                               double z) {
    if (!entity_registry_upsert(registry, entity_id, entity_kind, x, y, z)) {
        return 0;
    }

    return push_event(manager, ENTITY_EVENT_SPAWN, entity_id, entity_kind, x, y, z);
}

int entity_manager_queue_spawn_auto(entity_registry_t *registry,
                                    entity_manager_t *manager,
                                    int entity_kind,
                                    double x,
                                    double y,
                                    double z,
                                    int32_t *out_entity_id) {
    int32_t entity_id;

    if (manager == NULL) {
        return 0;
    }

    entity_id = manager->next_entity_id++;
    if (manager->next_entity_id < 1000) {
        manager->next_entity_id = 1000;
    }

    if (!entity_manager_queue_spawn(registry, manager, entity_id, entity_kind, x, y, z)) {
        return 0;
    }

    if (out_entity_id != NULL) {
        *out_entity_id = entity_id;
    }

    return 1;
}

int entity_manager_queue_update_xz(entity_registry_t *registry,
                                   entity_manager_t *manager,
                                   int32_t entity_id,
                                   double x,
                                   double z) {
    int idx;
    double y = 0.0;

    if (registry != NULL) {
        for (idx = 0; idx < (int)(sizeof(registry->entries) / sizeof(registry->entries[0])); ++idx) {
            if (registry->entries[idx].active && registry->entries[idx].entity_id == entity_id) {
                y = registry->entries[idx].y;
                break;
            }
        }
    }

    return entity_manager_queue_update_xyz(registry, manager, entity_id, x, y, z);
}

int entity_manager_queue_update_xyz(entity_registry_t *registry,
                                    entity_manager_t *manager,
                                    int32_t entity_id,
                                    double x,
                                    double y,
                                    double z) {
    if (!entity_registry_update_xyz(registry, entity_id, x, y, z)) {
        return 0;
    }

    if (manager != NULL && manager->pending_count > 0) {
        entity_event_t *last = &manager->pending[manager->pending_count - 1];
        if (last->event_kind == ENTITY_EVENT_UPDATE && last->entity_id == entity_id) {
            last->x = x;
            last->y = y;
            last->z = z;
            return 1;
        }
    }

    return push_event(manager, ENTITY_EVENT_UPDATE, entity_id, ENTITY_KIND_UNKNOWN, x, y, z);
}

int entity_manager_queue_remove(entity_registry_t *registry,
                                entity_manager_t *manager,
                                int32_t entity_id) {
    if (!entity_registry_remove(registry, entity_id)) {
        return 0;
    }

    return push_event(manager, ENTITY_EVENT_REMOVE, entity_id, ENTITY_KIND_UNKNOWN, 0.0, 0.0, 0.0);
}

size_t entity_manager_pending_count(const entity_manager_t *manager) {
    if (manager == NULL) {
        return 0;
    }

    return manager->pending_count;
}

size_t entity_manager_dropped_count(const entity_manager_t *manager) {
    if (manager == NULL) {
        return 0;
    }

    return manager->dropped_events;
}

void entity_manager_clear(entity_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    manager->pending_count = 0;
}
