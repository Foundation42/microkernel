#ifndef MICROKERNEL_NAMESPACE_H
#define MICROKERNEL_NAMESPACE_H

#include "types.h"

/* ── Namespace message types (system range) ────────────────────────── */

#define MSG_NS_REGISTER   ((msg_type_t)0xFF000014)
#define MSG_NS_LOOKUP     ((msg_type_t)0xFF000015)
#define MSG_NS_LIST       ((msg_type_t)0xFF000016)
#define MSG_NS_MOUNT      ((msg_type_t)0xFF000017)
#define MSG_NS_UMOUNT     ((msg_type_t)0xFF000018)
#define MSG_NS_REPLY      ((msg_type_t)0xFF000019)
#define MSG_NS_NOTIFY     ((msg_type_t)0xFF00001A)

#define NS_PATH_MAX 128

/* ── Request payloads ──────────────────────────────────────────────── */

typedef struct {
    char       path[NS_PATH_MAX];
    actor_id_t actor_id;
} ns_register_t;

typedef struct {
    char path[NS_PATH_MAX];
} ns_lookup_t;

typedef struct {
    char prefix[NS_PATH_MAX];
} ns_list_req_t;

typedef struct {
    char       mount_point[NS_PATH_MAX];
    actor_id_t target;     /* actor that handles this subtree */
} ns_mount_t;

typedef struct {
    char mount_point[NS_PATH_MAX];
} ns_umount_t;

/* ── Reply payload ─────────────────────────────────────────────────── */

#define NS_OK       0
#define NS_ENOENT  -1
#define NS_EEXIST  -2
#define NS_EFULL   -3
#define NS_EINVAL  -4

#define NS_REPLY_PAYLOAD_MAX 1024

typedef struct {
    int32_t    status;     /* NS_OK, NS_ENOENT, NS_EEXIST, ... */
    actor_id_t actor_id;   /* for lookup replies */
    uint32_t   data_len;   /* bytes used in data[] */
    char       data[NS_REPLY_PAYLOAD_MAX];
} ns_reply_t;

/* ── Notify payload (future: change notifications) ─────────────────── */

typedef struct {
    char       path[NS_PATH_MAX];
    actor_id_t actor_id;
    int32_t    event;      /* register=1, unregister=2 */
} ns_notify_t;

/* ── API ───────────────────────────────────────────────────────────── */

/* Spawn the namespace actor. Returns its ID. Registers itself as "ns". */
actor_id_t ns_actor_init(runtime_t *rt);

/* Direct-access path operations (bypass message queue, safe in single-threaded runtime).
   Used by name_registry.c for transparent /-prefixed path support. */
int        ns_register_path(runtime_t *rt, const char *path, actor_id_t id);
actor_id_t ns_lookup_path(runtime_t *rt, const char *path);
void       ns_deregister_actor_paths(runtime_t *rt, actor_id_t id);

/* Synchronous call to namespace actor (waiter actor pattern).
   Caller provides MSG_NS_* type + payload, gets ns_reply_t back.
   Only safe from outside the scheduler (init code, tests). */
int ns_call(runtime_t *rt, msg_type_t type,
            const void *payload, size_t payload_size,
            ns_reply_t *reply);

#endif /* MICROKERNEL_NAMESPACE_H */
