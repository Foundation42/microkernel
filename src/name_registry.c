#include "runtime_internal.h"
#include <string.h>

/* FNV-1a hash */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

bool actor_register_name(runtime_t *rt, const char *name, actor_id_t id) {
    if (!name || !name[0]) return false;
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    uint32_t h = fnv1a(name) % cap;

    for (size_t i = 0; i < cap; i++) {
        size_t idx = (h + i) % cap;
        if (!reg[idx].occupied) {
            strncpy(reg[idx].name, name, 64 - 1);
            reg[idx].name[64 - 1] = '\0';
            reg[idx].actor_id = id;
            reg[idx].occupied = true;
            return true;
        }
        if (strcmp(reg[idx].name, name) == 0) {
            return false; /* duplicate name */
        }
    }
    return false; /* full */
}

actor_id_t actor_lookup(runtime_t *rt, const char *name) {
    if (!name || !name[0]) return ACTOR_ID_INVALID;
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    uint32_t h = fnv1a(name) % cap;

    for (size_t i = 0; i < cap; i++) {
        size_t idx = (h + i) % cap;
        if (!reg[idx].occupied) return ACTOR_ID_INVALID;
        if (strcmp(reg[idx].name, name) == 0) {
            return reg[idx].actor_id;
        }
    }
    return ACTOR_ID_INVALID;
}

void name_registry_deregister_actor(runtime_t *rt, actor_id_t id) {
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    for (size_t i = 0; i < cap; i++) {
        if (reg[i].occupied && reg[i].actor_id == id) {
            memset(&reg[i], 0, sizeof(name_entry_t));
        }
    }
}
