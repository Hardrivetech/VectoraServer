#ifndef ENTITY_MANAGER_H
#define ENTITY_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "entity_registry.h"

enum {
    ENTITY_EVENT_SPAWN = 1,
    ENTITY_EVENT_UPDATE = 2,
    ENTITY_EVENT_REMOVE = 3
};

typedef struct {
    int event_kind;
    int32_t entity_id;
    int entity_kind;
    double x;
    double y;
    double z;
} entity_event_t;

typedef struct {
    entity_event_t pending[1024];
    size_t pending_count;
    size_t dropped_events;
    int32_t next_entity_id;
} entity_manager_t;

void entity_manager_init(entity_manager_t *manager);
int entity_manager_queue_spawn(entity_registry_t *registry,
                               entity_manager_t *manager,
                               int32_t entity_id,
                               int entity_kind,
                               double x,
                               double y,
                               double z);
int entity_manager_queue_spawn_auto(entity_registry_t *registry,
                                    entity_manager_t *manager,
                                    int entity_kind,
                                    double x,
                                    double y,
                                    double z,
                                    int32_t *out_entity_id);
int entity_manager_queue_update_xz(entity_registry_t *registry,
                                   entity_manager_t *manager,
                                   int32_t entity_id,
                                   double x,
                                   double z);
int entity_manager_queue_update_xyz(entity_registry_t *registry,
                                    entity_manager_t *manager,
                                    int32_t entity_id,
                                    double x,
                                    double y,
                                    double z);
int entity_manager_queue_remove(entity_registry_t *registry,
                                entity_manager_t *manager,
                                int32_t entity_id);
size_t entity_manager_pending_count(const entity_manager_t *manager);
size_t entity_manager_dropped_count(const entity_manager_t *manager);
void entity_manager_clear(entity_manager_t *manager);

#endif
