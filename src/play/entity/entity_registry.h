#ifndef ENTITY_REGISTRY_H
#define ENTITY_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

enum {
    ENTITY_KIND_UNKNOWN = 0,
    ENTITY_KIND_PLAYER = 1,
    ENTITY_KIND_MOB = 2,
    ENTITY_KIND_OBJECT = 3
};

typedef struct {
    int active;
    int32_t entity_id;
    int kind;
    double x;
    double y;
    double z;
} entity_state_t;

typedef struct {
    entity_state_t entries[512];
} entity_registry_t;

void entity_registry_init(entity_registry_t *registry);
int entity_registry_upsert(entity_registry_t *registry,
                           int32_t entity_id,
                           int kind,
                           double x,
                           double y,
                           double z);
int entity_registry_update_xz(entity_registry_t *registry,
                              int32_t entity_id,
                              double x,
                              double z);
int entity_registry_update_xyz(entity_registry_t *registry,
                               int32_t entity_id,
                               double x,
                               double y,
                               double z);
int entity_registry_remove(entity_registry_t *registry, int32_t entity_id);
size_t entity_registry_count(const entity_registry_t *registry);

#endif
