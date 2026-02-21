#include "microkernel/namespace.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "runtime_internal.h"
#include <stdio.h>
#include <string.h>

/* ── Caps actor: replies to MSG_CAPS_REQUEST with key=value pairs ──── */

static bool caps_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self; (void)state;

    if (msg->type != MSG_CAPS_REQUEST)
        return true;

    char buf[512];
    int off = 0;

#ifdef ESP_PLATFORM
    off += snprintf(buf + off, sizeof(buf) - off, "platform=esp32\n");
#else
    off += snprintf(buf + off, sizeof(buf) - off, "platform=linux\n");
#endif

    off += snprintf(buf + off, sizeof(buf) - off,
                    "identity=%s\n", mk_node_identity());
    off += snprintf(buf + off, sizeof(buf) - off,
                    "node_id=%u\n", (unsigned)runtime_get_node_id(rt));

#ifdef HAVE_WASM
    off += snprintf(buf + off, sizeof(buf) - off, "wasm=true\n");
#else
    off += snprintf(buf + off, sizeof(buf) - off, "wasm=false\n");
#endif

#ifdef HAVE_TLS
    off += snprintf(buf + off, sizeof(buf) - off, "tls=true\n");
#else
    off += snprintf(buf + off, sizeof(buf) - off, "tls=false\n");
#endif

    off += snprintf(buf + off, sizeof(buf) - off, "http=true\n");
    off += snprintf(buf + off, sizeof(buf) - off,
                    "max_actors=%zu\n", runtime_get_max_actors(rt));

    actor_id_t ids[256];
    size_t count = runtime_list_actors(rt, ids, 256);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "actor_count=%zu\n", count);
    off += snprintf(buf + off, sizeof(buf) - off,
                    "transports=%zu\n", runtime_get_transport_count(rt));

    actor_send(rt, msg->source, MSG_CAPS_REPLY, buf, (size_t)off);
    return true;
}

actor_id_t caps_actor_init(runtime_t *rt) {
    actor_id_t id = actor_spawn(rt, caps_behavior, NULL, NULL, 8);
    if (id == ACTOR_ID_INVALID) return ACTOR_ID_INVALID;

    char path[NS_PATH_MAX];
    snprintf(path, sizeof(path), "/node/%s/caps", mk_node_identity());
    actor_register_name(rt, path, id);

    return id;
}
