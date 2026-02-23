#include "runtime_internal.h"
#include "microkernel/message.h"
#include "microkernel/namespace.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations for runtime-level broadcast */
void runtime_broadcast_registry(runtime_t *rt, msg_type_t type,
                                 const void *payload, size_t payload_size);

/* FNV-1a hash */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

/* Internal: insert into registry without broadcasting (used for remote entries) */
bool name_registry_insert(runtime_t *rt, const char *name, actor_id_t id) {
    if (!name || !name[0]) return false;
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    uint32_t h = fnv1a(name) % cap;

    for (size_t i = 0; i < cap; i++) {
        size_t idx = (h + i) % cap;
        if (!reg[idx].occupied) {
            snprintf(reg[idx].name, sizeof(reg[idx].name), "%s", name);
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

/* Public: register name and broadcast to all connected peers */
bool actor_register_name(runtime_t *rt, const char *name, actor_id_t id) {
    /* Route /-prefixed paths to namespace path table */
    if (name && name[0] == '/') {
        int rc = ns_register_path(rt, name, id);
        if (rc != NS_OK) return false;
        /* Broadcast path registration to peers */
        path_register_payload_t payload;
        memset(&payload, 0, sizeof(payload));
        snprintf(payload.path, sizeof(payload.path), "%s", name);
        payload.actor_id = id;
        runtime_broadcast_registry(rt, MSG_PATH_REGISTER,
                                    &payload, sizeof(payload));
        return true;
    }

    if (!name_registry_insert(rt, name, id)) return false;

    /* Broadcast to all peers */
    name_register_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    snprintf(payload.name, sizeof(payload.name), "%s", name);
    payload.actor_id = id;
    runtime_broadcast_registry(rt, MSG_NAME_REGISTER, &payload, sizeof(payload));
    return true;
}

actor_id_t actor_lookup(runtime_t *rt, const char *name) {
    if (!name || !name[0]) return ACTOR_ID_INVALID;
    /* Route /-prefixed paths to namespace path table */
    if (name[0] == '/') {
        return ns_lookup_path(rt, name);
    }
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

/* Internal: remove by name without broadcasting (used for incoming MSG_NAME_UNREGISTER) */
void name_registry_remove_by_name(runtime_t *rt, const char *name) {
    if (!name || !name[0]) return;
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    for (size_t i = 0; i < cap; i++) {
        if (reg[i].occupied && strcmp(reg[i].name, name) == 0) {
            memset(&reg[i], 0, sizeof(name_entry_t));
            return;
        }
    }
}

void name_registry_deregister_actor(runtime_t *rt, actor_id_t id) {
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    for (size_t i = 0; i < cap; i++) {
        if (reg[i].occupied && reg[i].actor_id == id) {
            /* Broadcast unregister to peers before clearing */
            name_unregister_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            snprintf(payload.name, sizeof(payload.name), "%s", reg[i].name);
            runtime_broadcast_registry(rt, MSG_NAME_UNREGISTER,
                                        &payload, sizeof(payload));
            memset(&reg[i], 0, sizeof(name_entry_t));
        }
    }
    /* Also clean up any /-prefixed paths in the namespace actor */
    ns_deregister_actor_paths(rt, id);
}

size_t actor_reverse_lookup(runtime_t *rt, actor_id_t id,
                            char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;

    /* Scan flat name registry */
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    for (size_t i = 0; i < cap; i++) {
        if (reg[i].occupied && reg[i].actor_id == id) {
            size_t len = strlen(reg[i].name);
            if (len >= buf_size) len = buf_size - 1;
            memcpy(buf, reg[i].name, len);
            buf[len] = '\0';
            return len;
        }
    }

    /* Try namespace path table */
    return ns_reverse_lookup_path(rt, id, buf, buf_size);
}

size_t actor_reverse_lookup_all(runtime_t *rt, actor_id_t id,
                                char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;

    size_t off = 0;

    /* Scan flat name registry */
    name_entry_t *reg = runtime_get_name_registry(rt);
    size_t cap = runtime_get_name_registry_size();
    for (size_t i = 0; i < cap; i++) {
        if (reg[i].occupied && reg[i].actor_id == id) {
            size_t len = strlen(reg[i].name);
            size_t need = (off > 0 ? 2 : 0) + len;
            if (off + need >= buf_size) continue;
            if (off > 0) { buf[off++] = ','; buf[off++] = ' '; }
            memcpy(buf + off, reg[i].name, len);
            off += len;
        }
    }

    /* Also collect namespace paths */
    ns_reverse_lookup_all_paths(rt, id, buf, buf_size, &off);

    buf[off] = '\0';
    return off;
}
