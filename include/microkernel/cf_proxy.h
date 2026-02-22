#ifndef MICROKERNEL_CF_PROXY_H
#define MICROKERNEL_CF_PROXY_H

#include "types.h"

/* ── Cloudflare proxy message types ───────────────────────────────── */

/* Requests (to cf_proxy) */
#define MSG_CF_KV_PUT     ((msg_type_t)300)
#define MSG_CF_KV_GET     ((msg_type_t)301)
#define MSG_CF_KV_DELETE  ((msg_type_t)302)
#define MSG_CF_KV_LIST    ((msg_type_t)303)
/* 304-308 reserved for DB/Queue/AI */

/* Replies (from cf_proxy) */
#define MSG_CF_OK         ((msg_type_t)310)
#define MSG_CF_VALUE      ((msg_type_t)311)
#define MSG_CF_KEYS       ((msg_type_t)313)
#define MSG_CF_NOT_FOUND  ((msg_type_t)315)
#define MSG_CF_ERROR      ((msg_type_t)316)
/* 312/314 reserved for DB rows/embeddings */

/* ── Configuration ────────────────────────────────────────────────── */

typedef struct {
    char url[256];       /* "wss://mk-proxy.example.dev/ws" */
    char token[128];     /* pre-shared auth token */
} cf_proxy_config_t;

/* ── API ──────────────────────────────────────────────────────────── */

/*
 * Spawn the cf_proxy actor.  Connects via WebSocket to the Cloudflare
 * Worker at config->url, registers namespace paths:
 *   - /node/cloudflare/storage/kv  (always)
 *   - /node/storage/kv             (if unclaimed)
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 *
 * Payload formats (actor -> cf_proxy):
 *   MSG_CF_KV_PUT:    "key=mykey\nvalue=hello\nttl=0"
 *   MSG_CF_KV_GET:    "key=mykey"
 *   MSG_CF_KV_DELETE: "key=mykey"
 *   MSG_CF_KV_LIST:   "prefix=history/\nlimit=50"
 *
 * Reply payloads (cf_proxy -> requester):
 *   MSG_CF_VALUE:     (raw value bytes)
 *   MSG_CF_KEYS:      "key1\nkey2\nkey3"
 *   MSG_CF_OK:        (empty)
 *   MSG_CF_NOT_FOUND: (empty)
 *   MSG_CF_ERROR:     "error message text"
 */
actor_id_t cf_proxy_init(runtime_t *rt, const cf_proxy_config_t *config);

#endif /* MICROKERNEL_CF_PROXY_H */
