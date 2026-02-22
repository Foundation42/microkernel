#ifndef MICROKERNEL_LOCAL_KV_H
#define MICROKERNEL_LOCAL_KV_H

#include "types.h"

/* ── Configuration ────────────────────────────────────────────────── */

typedef struct {
    char base_path[64];  /* "/s" on ESP32, "/tmp/mk_kv" on Linux */
} local_kv_config_t;

/* ── API ──────────────────────────────────────────────────────────── */

/*
 * Spawn the local KV actor.  Uses the filesystem at config->base_path
 * for persistent key-value storage.
 *
 * Registers namespace paths:
 *   - /node/local/storage/kv    (always)
 *   - /node/storage/kv          (if unclaimed)
 *
 * Reuses the same message types as cf_proxy (MSG_CF_KV_*).
 * Must be called BEFORE cf_proxy_init() so local KV claims
 * /node/storage/kv first.
 *
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t local_kv_init(runtime_t *rt, const local_kv_config_t *config);

#endif /* MICROKERNEL_LOCAL_KV_H */
