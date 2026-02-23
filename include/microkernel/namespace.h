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

/* Reverse lookup: find first path registered to actor ID.
   Returns path length (0 if not found or ns_state is NULL). */
size_t     ns_reverse_lookup_path(runtime_t *rt, actor_id_t id,
                                  char *buf, size_t buf_size);

/* Collect ALL paths for an actor, appending ", "-separated entries to buf
   starting at *offset.  Updates *offset.  Returns number of paths found. */
size_t     ns_reverse_lookup_all_paths(runtime_t *rt, actor_id_t id,
                                       char *buf, size_t buf_size,
                                       size_t *offset);

/* Synchronous call to namespace actor (waiter actor pattern).
   Caller provides MSG_NS_* type + payload, gets ns_reply_t back.
   Only safe from outside the scheduler (init code, tests). */
int ns_call(runtime_t *rt, msg_type_t type,
            const void *payload, size_t payload_size,
            ns_reply_t *reply);

/* Node identity — stable human-readable name.
   Linux: MK_NODE_NAME env var or "linux-XXXX" (hostname hash).
   ESP32: "esp32-AABBCC" (efuse MAC). */
const char *mk_node_identity(void);

/* Direct-access path listing. Writes "path=id\n" pairs matching prefix.
   Returns bytes written. */
size_t ns_list_paths(runtime_t *rt, const char *prefix, char *buf, size_t buf_size);

/* Stable node ID derived from identity.
   Linux: MK_NODE_ID env var or hash of identity -> [1,15].
   ESP32: hash of identity -> [1,15]. */
node_id_t mk_node_id(void);

/* Remove a specific path by name (for remote unregister). */
void ns_remove_path(runtime_t *rt, const char *path);

/* Sync all local names + paths to a single transport. */
struct transport;
void ns_sync_to_transport(runtime_t *rt, struct transport *tp);

/* Start a mount listener on the given port. Returns listener actor ID. */
actor_id_t ns_mount_listen(runtime_t *rt, uint16_t port);

/* Connect to a remote mount listener. Blocking (3s timeout).
   On success, returns 0 and fills result. On failure, returns -1. */
typedef struct {
    char      identity[32];
    node_id_t node_id;
} mount_result_t;

int ns_mount_connect(runtime_t *rt, const char *host, uint16_t port,
                     mount_result_t *result);

/* Capability advertisement actor */
actor_id_t caps_actor_init(runtime_t *rt);

#define MK_MOUNT_PORT 4200

/* Mount hello (exchanged over raw TCP before transport creation) */
#define MOUNT_HELLO_MAGIC 0x4D4B3031  /* "MK01" */
typedef struct __attribute__((packed)) {
    uint32_t magic;        /* MOUNT_HELLO_MAGIC, network byte order */
    uint32_t node_id;      /* network byte order */
    char     identity[32]; /* null-terminated */
} mount_hello_t;

#endif /* MICROKERNEL_NAMESPACE_H */
