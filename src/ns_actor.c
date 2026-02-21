#include "microkernel/namespace.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/supervision.h"
#include "runtime_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Path table (hierarchical /paths) ──────────────────────────────── */

#ifndef NS_MAX_PATH_ENTRIES
#define NS_MAX_PATH_ENTRIES 64
#endif

typedef struct {
    char       path[NS_PATH_MAX];
    actor_id_t actor_id;
    bool       occupied;
} path_entry_t;

/* ── Mount table ───────────────────────────────────────────────────── */

#ifndef NS_MAX_MOUNTS
#define NS_MAX_MOUNTS 16
#endif

typedef struct {
    char       mount_point[NS_PATH_MAX];
    actor_id_t target;
    bool       occupied;
} mount_entry_t;

/* ── Namespace actor state ─────────────────────────────────────────── */

typedef struct {
    path_entry_t  paths[NS_MAX_PATH_ENTRIES];
    mount_entry_t mounts[NS_MAX_MOUNTS];
} ns_state_t;

/* ── Path table operations ─────────────────────────────────────────── */

static int ns_path_register(ns_state_t *s, const char *path, actor_id_t id) {
    for (size_t i = 0; i < NS_MAX_PATH_ENTRIES; i++) {
        if (s->paths[i].occupied && strcmp(s->paths[i].path, path) == 0)
            return NS_EEXIST;
    }
    for (size_t i = 0; i < NS_MAX_PATH_ENTRIES; i++) {
        if (!s->paths[i].occupied) {
            strncpy(s->paths[i].path, path, NS_PATH_MAX - 1);
            s->paths[i].path[NS_PATH_MAX - 1] = '\0';
            s->paths[i].actor_id = id;
            s->paths[i].occupied = true;
            return NS_OK;
        }
    }
    return NS_EFULL;
}

static actor_id_t ns_path_lookup(ns_state_t *s, const char *path) {
    for (size_t i = 0; i < NS_MAX_PATH_ENTRIES; i++) {
        if (s->paths[i].occupied && strcmp(s->paths[i].path, path) == 0)
            return s->paths[i].actor_id;
    }
    return ACTOR_ID_INVALID;
}

static void ns_path_remove_actor(ns_state_t *s, actor_id_t id) {
    for (size_t i = 0; i < NS_MAX_PATH_ENTRIES; i++) {
        if (s->paths[i].occupied && s->paths[i].actor_id == id)
            memset(&s->paths[i], 0, sizeof(path_entry_t));
    }
}

/* ── Mount table operations ────────────────────────────────────────── */

static int ns_mount_add(ns_state_t *s, const char *mount_point,
                         actor_id_t target) {
    for (size_t i = 0; i < NS_MAX_MOUNTS; i++) {
        if (s->mounts[i].occupied &&
            strcmp(s->mounts[i].mount_point, mount_point) == 0)
            return NS_EEXIST;
    }
    for (size_t i = 0; i < NS_MAX_MOUNTS; i++) {
        if (!s->mounts[i].occupied) {
            strncpy(s->mounts[i].mount_point, mount_point, NS_PATH_MAX - 1);
            s->mounts[i].mount_point[NS_PATH_MAX - 1] = '\0';
            s->mounts[i].target = target;
            s->mounts[i].occupied = true;
            return NS_OK;
        }
    }
    return NS_EFULL;
}

static int ns_mount_remove(ns_state_t *s, const char *mount_point) {
    for (size_t i = 0; i < NS_MAX_MOUNTS; i++) {
        if (s->mounts[i].occupied &&
            strcmp(s->mounts[i].mount_point, mount_point) == 0) {
            memset(&s->mounts[i], 0, sizeof(mount_entry_t));
            return NS_OK;
        }
    }
    return NS_ENOENT;
}

static mount_entry_t *ns_mount_match(ns_state_t *s, const char *path) {
    mount_entry_t *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < NS_MAX_MOUNTS; i++) {
        if (!s->mounts[i].occupied) continue;
        size_t mlen = strlen(s->mounts[i].mount_point);
        if (mlen > best_len &&
            strncmp(path, s->mounts[i].mount_point, mlen) == 0 &&
            (path[mlen] == '/' || path[mlen] == '\0')) {
            best = &s->mounts[i];
            best_len = mlen;
        }
    }
    return best;
}

/* ── Namespace actor behavior ──────────────────────────────────────── */

static bool ns_behavior(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    ns_state_t *s = state;
    ns_reply_t reply;
    memset(&reply, 0, sizeof(reply));

    switch (msg->type) {

    case MSG_NS_REGISTER: {
        if (msg->payload_size < sizeof(ns_register_t)) {
            reply.status = NS_EINVAL;
            break;
        }
        const ns_register_t *req = msg->payload;
        if (req->path[0] == '/') {
            reply.status = ns_path_register(s, req->path, req->actor_id);
        } else {
            bool ok = actor_register_name(rt, req->path, req->actor_id);
            reply.status = ok ? NS_OK : NS_EEXIST;
        }
        reply.actor_id = req->actor_id;
        actor_send(rt, msg->source, MSG_NS_REPLY, &reply, sizeof(reply));
        return true;
    }

    case MSG_NS_LOOKUP: {
        if (msg->payload_size < sizeof(ns_lookup_t)) {
            reply.status = NS_EINVAL;
            break;
        }
        const ns_lookup_t *req = msg->payload;
        if (req->path[0] == '/') {
            mount_entry_t *mount = ns_mount_match(s, req->path);
            if (mount) {
                reply.status = NS_OK;
                reply.actor_id = mount->target;
                actor_send(rt, msg->source, MSG_NS_REPLY,
                           &reply, sizeof(reply));
                return true;
            }
            actor_id_t id = ns_path_lookup(s, req->path);
            if (id != ACTOR_ID_INVALID) {
                reply.status = NS_OK;
                reply.actor_id = id;
            } else {
                reply.status = NS_ENOENT;
            }
        } else {
            actor_id_t id = actor_lookup(rt, req->path);
            if (id != ACTOR_ID_INVALID) {
                reply.status = NS_OK;
                reply.actor_id = id;
            } else {
                reply.status = NS_ENOENT;
            }
        }
        actor_send(rt, msg->source, MSG_NS_REPLY, &reply, sizeof(reply));
        return true;
    }

    case MSG_NS_LIST: {
        if (msg->payload_size < sizeof(ns_list_req_t)) {
            reply.status = NS_EINVAL;
            break;
        }
        const ns_list_req_t *req = msg->payload;
        size_t prefix_len = strlen(req->prefix);
        size_t off = 0;

        for (size_t i = 0; i < NS_MAX_PATH_ENTRIES; i++) {
            if (!s->paths[i].occupied) continue;
            if (prefix_len > 0 &&
                strncmp(s->paths[i].path, req->prefix, prefix_len) != 0)
                continue;
            int n = snprintf(reply.data + off, NS_REPLY_PAYLOAD_MAX - off,
                             "%s=%llu\n", s->paths[i].path,
                             (unsigned long long)s->paths[i].actor_id);
            if (n > 0 && (size_t)n < NS_REPLY_PAYLOAD_MAX - off)
                off += (size_t)n;
        }

        if (prefix_len == 0) {
            name_entry_t *reg = runtime_get_name_registry(rt);
            size_t cap = runtime_get_name_registry_size();
            for (size_t i = 0; i < cap; i++) {
                if (!reg[i].occupied) continue;
                int n = snprintf(reply.data + off,
                                 NS_REPLY_PAYLOAD_MAX - off,
                                 "%s=%llu\n", reg[i].name,
                                 (unsigned long long)reg[i].actor_id);
                if (n > 0 && (size_t)n < NS_REPLY_PAYLOAD_MAX - off)
                    off += (size_t)n;
            }
        }

        reply.status = NS_OK;
        reply.data_len = (uint32_t)off;
        actor_send(rt, msg->source, MSG_NS_REPLY, &reply, sizeof(reply));
        return true;
    }

    case MSG_NS_MOUNT: {
        if (msg->payload_size < sizeof(ns_mount_t)) {
            reply.status = NS_EINVAL;
            break;
        }
        const ns_mount_t *req = msg->payload;
        reply.status = ns_mount_add(s, req->mount_point, req->target);
        actor_send(rt, msg->source, MSG_NS_REPLY, &reply, sizeof(reply));
        return true;
    }

    case MSG_NS_UMOUNT: {
        if (msg->payload_size < sizeof(ns_umount_t)) {
            reply.status = NS_EINVAL;
            break;
        }
        const ns_umount_t *req = msg->payload;
        reply.status = ns_mount_remove(s, req->mount_point);
        actor_send(rt, msg->source, MSG_NS_REPLY, &reply, sizeof(reply));
        return true;
    }

    case MSG_CHILD_EXIT: {
        if (msg->payload_size >= sizeof(child_exit_payload_t)) {
            const child_exit_payload_t *p = msg->payload;
            ns_path_remove_actor(s, p->child_id);
        }
        return true;
    }

    default:
        return true;
    }

    /* Fallthrough for error replies from early breaks */
    actor_send(rt, msg->source, MSG_NS_REPLY, &reply, sizeof(reply));
    return true;
}

/* ── Public API ────────────────────────────────────────────────────── */

actor_id_t ns_actor_init(runtime_t *rt) {
    ns_state_t *s = calloc(1, sizeof(ns_state_t));
    if (!s) return ACTOR_ID_INVALID;

    actor_id_t id = actor_spawn(rt, ns_behavior, s, free, 64);
    if (id == ACTOR_ID_INVALID) {
        free(s);
        return ACTOR_ID_INVALID;
    }

    runtime_set_ns_state(rt, s);

    /* Register node identity paths */
    char node_path[NS_PATH_MAX];
    snprintf(node_path, NS_PATH_MAX, "/node/%s", mk_node_identity());
    ns_path_register(s, node_path, id);
    ns_path_register(s, "/sys/ns", id);

    actor_register_name(rt, "ns", id);
    return id;
}

/* ── Direct-access path operations (bypass message queue) ──────────── */

int ns_register_path(runtime_t *rt, const char *path, actor_id_t id) {
    ns_state_t *s = runtime_get_ns_state(rt);
    if (!s) return NS_EINVAL;
    return ns_path_register(s, path, id);
}

actor_id_t ns_lookup_path(runtime_t *rt, const char *path) {
    ns_state_t *s = runtime_get_ns_state(rt);
    if (!s) return ACTOR_ID_INVALID;
    mount_entry_t *mount = ns_mount_match(s, path);
    if (mount) return mount->target;
    return ns_path_lookup(s, path);
}

void ns_deregister_actor_paths(runtime_t *rt, actor_id_t id) {
    ns_state_t *s = runtime_get_ns_state(rt);
    if (!s) return;
    ns_path_remove_actor(s, id);
}

size_t ns_list_paths(runtime_t *rt, const char *prefix, char *buf, size_t buf_size) {
    ns_state_t *s = runtime_get_ns_state(rt);
    if (!s || !buf || buf_size == 0) return 0;
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    size_t off = 0;
    for (size_t i = 0; i < NS_MAX_PATH_ENTRIES; i++) {
        if (!s->paths[i].occupied) continue;
        if (prefix_len > 0 &&
            strncmp(s->paths[i].path, prefix, prefix_len) != 0)
            continue;
        int n = snprintf(buf + off, buf_size - off, "%s=%llu\n",
                         s->paths[i].path,
                         (unsigned long long)s->paths[i].actor_id);
        if (n > 0 && (size_t)n < buf_size - off) off += (size_t)n;
    }
    return off;
}

/* ── Waiter actor for synchronous ns_call ──────────────────────────── */

/* Message type used internally to trigger the waiter to forward a request.
   Payload: waiter_trigger_t (ns actor id + original request type + payload). */
#define MSG_WAITER_TRIGGER ((msg_type_t)0xFF0000FF)

typedef struct {
    bool        got_reply;
    ns_reply_t  reply;
} waiter_state_t;

static bool waiter_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    waiter_state_t *ws = state;

    if (msg->type == MSG_NS_REPLY &&
        msg->payload_size >= sizeof(ns_reply_t)) {
        memcpy(&ws->reply, msg->payload, sizeof(ns_reply_t));
        ws->got_reply = true;
        return true;
    }

    if (msg->type == MSG_WAITER_TRIGGER && msg->payload_size > 12) {
        /* Payload layout: ns_actor_id(8) + req_type(4) + request_payload */
        const uint8_t *p = msg->payload;
        actor_id_t ns_id;
        msg_type_t req_type;
        memcpy(&ns_id, p, sizeof(actor_id_t));
        memcpy(&req_type, p + sizeof(actor_id_t), sizeof(msg_type_t));
        const void *req_payload = p + sizeof(actor_id_t) + sizeof(msg_type_t);
        size_t req_size = msg->payload_size
                          - sizeof(actor_id_t) - sizeof(msg_type_t);
        /* actor_send will set source = self->id (the waiter) */
        actor_send(rt, ns_id, req_type, req_payload, req_size);
    }

    return true;
}

int ns_call(runtime_t *rt, msg_type_t type,
            const void *payload, size_t payload_size,
            ns_reply_t *reply) {
    actor_id_t ns_id = actor_lookup(rt, "ns");
    if (ns_id == ACTOR_ID_INVALID) return -1;

    /* Spawn waiter */
    waiter_state_t *ws = calloc(1, sizeof(waiter_state_t));
    if (!ws) return -1;

    actor_id_t waiter_id = actor_spawn(rt, waiter_behavior, ws, NULL, 4);
    if (waiter_id == ACTOR_ID_INVALID) {
        free(ws);
        return -1;
    }

    /* Build trigger payload: ns_id(8) + type(4) + request_payload */
    size_t trigger_size = sizeof(actor_id_t) + sizeof(msg_type_t) + payload_size;
    uint8_t *trigger = malloc(trigger_size);
    if (!trigger) {
        actor_stop(rt, waiter_id);
        runtime_step(rt);
        free(ws);
        return -1;
    }
    memcpy(trigger, &ns_id, sizeof(actor_id_t));
    memcpy(trigger + sizeof(actor_id_t), &type, sizeof(msg_type_t));
    if (payload_size > 0)
        memcpy(trigger + sizeof(actor_id_t) + sizeof(msg_type_t),
               payload, payload_size);

    /* Deliver trigger to waiter via runtime_deliver_msg */
    runtime_deliver_msg(rt, waiter_id, MSG_WAITER_TRIGGER,
                        trigger, trigger_size);
    free(trigger);

    /* Pump scheduler until waiter gets its reply */
    int steps = 0;
    while (!ws->got_reply && steps < 1000) {
        runtime_step(rt);
        steps++;
    }

    int result;
    if (ws->got_reply) {
        if (reply) *reply = ws->reply;
        result = 0;
    } else {
        result = -1;
    }

    actor_stop(rt, waiter_id);
    runtime_step(rt); /* cleanup waiter */
    free(ws);
    return result;
}
