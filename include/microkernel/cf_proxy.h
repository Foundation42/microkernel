#ifndef MICROKERNEL_CF_PROXY_H
#define MICROKERNEL_CF_PROXY_H

#include "types.h"

/* ── Cloudflare proxy message types ───────────────────────────────── */

/* Requests (to cf_proxy) — KV */
#define MSG_CF_KV_PUT     ((msg_type_t)300)
#define MSG_CF_KV_GET     ((msg_type_t)301)
#define MSG_CF_KV_DELETE  ((msg_type_t)302)
#define MSG_CF_KV_LIST    ((msg_type_t)303)

/* Requests (to cf_proxy) — D1 SQL */
#define MSG_CF_DB_QUERY   ((msg_type_t)304)
#define MSG_CF_DB_EXEC    ((msg_type_t)305)

/* Requests (to cf_proxy) — Queue */
#define MSG_CF_QUEUE_PUSH ((msg_type_t)306)

/* Requests (to cf_proxy) — AI */
#define MSG_CF_AI_INFER   ((msg_type_t)307)
#define MSG_CF_AI_EMBED   ((msg_type_t)308)

/* Replies (from cf_proxy) */
#define MSG_CF_OK         ((msg_type_t)310)
#define MSG_CF_VALUE      ((msg_type_t)311)  /* string value (KV get, AI infer) */
#define MSG_CF_ROWS       ((msg_type_t)312)  /* JSON rows array (D1 query) */
#define MSG_CF_KEYS       ((msg_type_t)313)  /* newline-delimited keys (KV list) */
#define MSG_CF_EMBEDDING  ((msg_type_t)314)  /* JSON float array (AI embed) */
#define MSG_CF_NOT_FOUND  ((msg_type_t)315)
#define MSG_CF_ERROR      ((msg_type_t)316)

/* ── Configuration ────────────────────────────────────────────────── */

typedef struct {
    char url[256];       /* "wss://mk-proxy.example.dev/ws" */
    char token[128];     /* pre-shared auth token */
} cf_proxy_config_t;

/* ── API ──────────────────────────────────────────────────────────── */

/*
 * Spawn the cf_proxy actor.  Connects via WebSocket to the Cloudflare
 * Worker at config->url, registers namespace paths:
 *   - /node/cloudflare/storage/{kv,db}  (always)
 *   - /node/cloudflare/queue/default    (always)
 *   - /node/cloudflare/ai/{infer,embed} (always)
 *   - /node/storage/{kv,db}             (if unclaimed)
 *   - /node/queue/default               (if unclaimed)
 *   - /node/ai/{infer,embed}            (if unclaimed)
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 *
 * Payload formats (actor -> cf_proxy):
 *   MSG_CF_KV_PUT:      "key=mykey\nvalue=hello\nttl=0"
 *   MSG_CF_KV_GET:      "key=mykey"
 *   MSG_CF_KV_DELETE:   "key=mykey"
 *   MSG_CF_KV_LIST:     "prefix=history/\nlimit=50"
 *   MSG_CF_DB_QUERY:    "sql=SELECT * FROM t WHERE id=?\np=42"
 *   MSG_CF_DB_EXEC:     "sql=INSERT INTO t(x) VALUES(?)\np=hello"
 *   MSG_CF_QUEUE_PUSH:  "body=payload text\ndelay=0"
 *   MSG_CF_AI_INFER:    "model=@cf/meta/llama-3.1-8b-instruct\nprompt=hello"
 *   MSG_CF_AI_EMBED:    "model=@cf/baai/bge-small-en-v1.5\ntext=hello world"
 *
 * Reply payloads (cf_proxy -> requester):
 *   MSG_CF_VALUE:     (raw value bytes — KV get or AI infer result)
 *   MSG_CF_KEYS:      "key1\nkey2\nkey3"
 *   MSG_CF_ROWS:      (JSON rows array string from D1 query)
 *   MSG_CF_EMBEDDING: (JSON float array string from AI embed)
 *   MSG_CF_OK:        (empty, or rows_affected string for DB exec)
 *   MSG_CF_NOT_FOUND: (empty)
 *   MSG_CF_ERROR:     "error message text"
 */
actor_id_t cf_proxy_init(runtime_t *rt, const cf_proxy_config_t *config);

#endif /* MICROKERNEL_CF_PROXY_H */
