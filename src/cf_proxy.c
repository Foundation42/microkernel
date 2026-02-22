#include "microkernel/cf_proxy.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/http.h"
#include "microkernel/namespace.h"
#include "json_util.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define CF_MAX_PENDING   16
#define CF_BACKOFF_INIT  1000    /* 1s */
#define CF_BACKOFF_MAX   30000   /* 30s */
#define CF_JSON_BUF      1024
#define CF_REPLY_BUF     4096

/* Debug logging (compiled out unless CF_PROXY_DEBUG is defined) */
#ifdef CF_PROXY_DEBUG
static void cf_log(const char *fmt, ...) {
    printf("[cf_proxy] ");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}
#define CF_LOG cf_log
#else
static inline void cf_log_noop(const char *fmt, ...) { (void)fmt; }
#define CF_LOG cf_log_noop
#endif

/* ── Pending request tracking ─────────────────────────────────────── */

typedef struct {
    uint32_t   req_id;
    actor_id_t requester;
    msg_type_t req_type;
    bool       occupied;
} cf_pending_t;

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    cf_proxy_config_t config;
    http_conn_id_t    ws_conn;
    bool              connected;
    bool              authed;
    uint32_t          next_req_id;
    cf_pending_t      pending[CF_MAX_PENDING];
    int               backoff_ms;
} cf_proxy_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Find a value for "key=value\n" in payload text */
static size_t payload_get(const char *payload, size_t plen,
                          const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = payload;
    const char *end = payload + plen;

    while (p < end) {
        if ((size_t)(end - p) > klen &&
            memcmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *vstart = p + klen + 1;
            const char *vend = vstart;
            while (vend < end && *vend != '\n') vend++;
            size_t vlen = (size_t)(vend - vstart);
            size_t copy = vlen < cap - 1 ? vlen : cap - 1;
            memcpy(out, vstart, copy);
            out[copy] = '\0';
            return copy;
        }
        /* skip to next line */
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    out[0] = '\0';
    return 0;
}

/* Allocate a pending slot. Returns index or -1 if full. */
static int pending_alloc(cf_proxy_state_t *s, actor_id_t requester,
                          msg_type_t req_type) {
    for (int i = 0; i < CF_MAX_PENDING; i++) {
        if (!s->pending[i].occupied) {
            s->pending[i].occupied = true;
            s->pending[i].req_id = s->next_req_id++;
            s->pending[i].requester = requester;
            s->pending[i].req_type = req_type;
            CF_LOG("pending_alloc: slot=%d req_id=%u type=%u from=%llu",
                   i, s->pending[i].req_id, (unsigned)req_type,
                   (unsigned long long)requester);
            return i;
        }
    }
    CF_LOG("pending_alloc: FULL (16 slots occupied)");
    return -1;
}

/* Find pending slot by req_id. Returns index or -1. */
static int pending_find(cf_proxy_state_t *s, uint32_t req_id) {
    for (int i = 0; i < CF_MAX_PENDING; i++) {
        if (s->pending[i].occupied && s->pending[i].req_id == req_id)
            return i;
    }
    return -1;
}

/* Fail all pending requests with MSG_CF_ERROR. */
static void fail_all_pending(cf_proxy_state_t *s, runtime_t *rt,
                              const char *reason) {
    for (int i = 0; i < CF_MAX_PENDING; i++) {
        if (s->pending[i].occupied) {
            actor_send(rt, s->pending[i].requester, MSG_CF_ERROR,
                       reason, strlen(reason));
            s->pending[i].occupied = false;
        }
    }
}

/* Schedule a reconnect timer. */
static void schedule_reconnect(cf_proxy_state_t *s, runtime_t *rt) {
    actor_set_timer(rt, (uint64_t)s->backoff_ms, false);
    /* Exponential backoff */
    s->backoff_ms *= 2;
    if (s->backoff_ms > CF_BACKOFF_MAX)
        s->backoff_ms = CF_BACKOFF_MAX;
}

/* Send a JSON message over WebSocket. */
static bool ws_send_json(cf_proxy_state_t *s, runtime_t *rt,
                          const char *json, size_t len) {
    if (!s->connected || s->ws_conn == HTTP_CONN_ID_INVALID) {
        CF_LOG("ws_send: not connected");
        return false;
    }
    CF_LOG("ws_send: %.*s", (int)(len > 200 ? 200 : len), json);
    return actor_ws_send_text(rt, s->ws_conn, json, len);
}

/* ── Handle KV request from local actor ───────────────────────────── */

static void handle_kv_request(cf_proxy_state_t *s, runtime_t *rt,
                               message_t *msg) {
    if (!s->connected || !s->authed) {
        const char *err = "not connected";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    int idx = pending_alloc(s, msg->source, msg->type);
    if (idx < 0) {
        const char *err = "pending queue full";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char key[256] = "";
    char value[CF_REPLY_BUF] = "";
    char prefix[256] = "";
    char limit_str[16] = "";
    char ttl_str[16] = "";

    char json[CF_JSON_BUF];
    json_buf_t j;
    json_init(&j, json, sizeof(json));
    json_obj_open(&j);
    json_int(&j, "req_id", (int64_t)s->pending[idx].req_id);

    switch (msg->type) {
    case MSG_CF_KV_PUT:
        json_str(&j, "type", "kv_put");
        payload_get(payload, plen, "key", key, sizeof(key));
        payload_get(payload, plen, "value", value, sizeof(value));
        payload_get(payload, plen, "ttl", ttl_str, sizeof(ttl_str));
        json_str(&j, "key", key);
        json_str(&j, "value", value);
        if (ttl_str[0])
            json_int(&j, "ttl", strtoll(ttl_str, NULL, 10));
        break;

    case MSG_CF_KV_GET:
        json_str(&j, "type", "kv_get");
        payload_get(payload, plen, "key", key, sizeof(key));
        json_str(&j, "key", key);
        break;

    case MSG_CF_KV_DELETE:
        json_str(&j, "type", "kv_delete");
        payload_get(payload, plen, "key", key, sizeof(key));
        json_str(&j, "key", key);
        break;

    case MSG_CF_KV_LIST:
        json_str(&j, "type", "kv_list");
        payload_get(payload, plen, "prefix", prefix, sizeof(prefix));
        payload_get(payload, plen, "limit", limit_str, sizeof(limit_str));
        json_str(&j, "prefix", prefix);
        if (limit_str[0])
            json_int(&j, "limit", strtoll(limit_str, NULL, 10));
        break;

    default:
        s->pending[idx].occupied = false;
        return;
    }

    json_obj_close(&j);

    if (!ws_send_json(s, rt, json, json_len(&j))) {
        s->pending[idx].occupied = false;
        const char *err = "ws send failed";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
    }
}

/* ── Handle DB request from local actor ───────────────────────────── */

static void handle_db_request(cf_proxy_state_t *s, runtime_t *rt,
                               message_t *msg) {
    if (!s->connected || !s->authed) {
        const char *err = "not connected";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    int idx = pending_alloc(s, msg->source, msg->type);
    if (idx < 0) {
        const char *err = "pending queue full";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char sql[512] = "";

    char json[CF_JSON_BUF];
    json_buf_t j;
    json_init(&j, json, sizeof(json));
    json_obj_open(&j);
    json_int(&j, "req_id", (int64_t)s->pending[idx].req_id);

    if (msg->type == MSG_CF_DB_QUERY)
        json_str(&j, "type", "db_query");
    else
        json_str(&j, "type", "db_exec");

    payload_get(payload, plen, "sql", sql, sizeof(sql));
    json_str(&j, "sql", sql);

    /* Collect p= params into JSON array */
    json_array_open(&j, "params");
    {
        const char *p = payload;
        const char *end = payload + plen;
        while (p < end) {
            if ((size_t)(end - p) > 2 && p[0] == 'p' && p[1] == '=') {
                const char *vstart = p + 2;
                const char *vend = vstart;
                while (vend < end && *vend != '\n') vend++;
                char param[256];
                size_t vlen = (size_t)(vend - vstart);
                size_t copy = vlen < sizeof(param) - 1 ? vlen : sizeof(param) - 1;
                memcpy(param, vstart, copy);
                param[copy] = '\0';
                json_array_str_item(&j, param);
                p = vend;
            }
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
        }
    }
    json_array_close(&j);
    json_obj_close(&j);

    if (!ws_send_json(s, rt, json, json_len(&j))) {
        s->pending[idx].occupied = false;
        const char *err = "ws send failed";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
    }
}

/* ── Handle Queue request from local actor ────────────────────────── */

static void handle_queue_request(cf_proxy_state_t *s, runtime_t *rt,
                                  message_t *msg) {
    if (!s->connected || !s->authed) {
        const char *err = "not connected";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    int idx = pending_alloc(s, msg->source, msg->type);
    if (idx < 0) {
        const char *err = "pending queue full";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char body[CF_REPLY_BUF] = "";
    char delay_str[16] = "";

    char json[CF_JSON_BUF];
    json_buf_t j;
    json_init(&j, json, sizeof(json));
    json_obj_open(&j);
    json_int(&j, "req_id", (int64_t)s->pending[idx].req_id);
    json_str(&j, "type", "queue_push");
    payload_get(payload, plen, "body", body, sizeof(body));
    json_str(&j, "body", body);
    payload_get(payload, plen, "delay", delay_str, sizeof(delay_str));
    if (delay_str[0])
        json_int(&j, "delay_seconds", strtoll(delay_str, NULL, 10));
    json_obj_close(&j);

    if (!ws_send_json(s, rt, json, json_len(&j))) {
        s->pending[idx].occupied = false;
        const char *err = "ws send failed";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
    }
}

/* ── Handle AI request from local actor ───────────────────────────── */

static void handle_ai_request(cf_proxy_state_t *s, runtime_t *rt,
                                message_t *msg) {
    if (!s->connected || !s->authed) {
        const char *err = "not connected";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    int idx = pending_alloc(s, msg->source, msg->type);
    if (idx < 0) {
        const char *err = "pending queue full";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char model[128] = "";

    char json[CF_JSON_BUF];
    json_buf_t j;
    json_init(&j, json, sizeof(json));
    json_obj_open(&j);
    json_int(&j, "req_id", (int64_t)s->pending[idx].req_id);

    payload_get(payload, plen, "model", model, sizeof(model));

    if (msg->type == MSG_CF_AI_INFER) {
        char prompt[512] = "";
        json_str(&j, "type", "ai_infer");
        if (model[0]) json_str(&j, "model", model);
        payload_get(payload, plen, "prompt", prompt, sizeof(prompt));
        json_str(&j, "prompt", prompt);
    } else {
        char text[512] = "";
        json_str(&j, "type", "ai_embed");
        if (model[0]) json_str(&j, "model", model);
        payload_get(payload, plen, "text", text, sizeof(text));
        json_str(&j, "text", text);
    }

    json_obj_close(&j);

    if (!ws_send_json(s, rt, json, json_len(&j))) {
        s->pending[idx].occupied = false;
        const char *err = "ws send failed";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
    }
}

/* ── Handle JSON response from Worker ─────────────────────────────── */

static void handle_ws_response(cf_proxy_state_t *s, runtime_t *rt,
                                const char *json, size_t len) {
    CF_LOG("ws_recv: len=%zu type_prefix=%.*s", len,
           (int)(len > 80 ? 80 : len), json);

    /* Check for auth_ok */
    char type[32] = "";
    json_get_str(json, "type", type, sizeof(type));

    if (strcmp(type, "auth_ok") == 0) {
        CF_LOG("auth_ok");
        s->authed = true;
        s->backoff_ms = CF_BACKOFF_INIT;
        return;
    }

    if (strcmp(type, "ping") == 0) {
        char pong[32];
        json_buf_t j;
        json_init(&j, pong, sizeof(pong));
        json_obj_open(&j);
        json_str(&j, "type", "pong");
        json_obj_close(&j);
        ws_send_json(s, rt, pong, json_len(&j));
        return;
    }

    /* All other responses should have req_id */
    int64_t req_id = json_get_int(json, "req_id", -1);
    if (req_id < 0) {
        CF_LOG("ws_recv: no req_id, type=%s", type);
        return;
    }

    int idx = pending_find(s, (uint32_t)req_id);
    if (idx < 0) {
        CF_LOG("ws_recv: req_id=%lld not in pending table (stale?)",
               (long long)req_id);
        return;
    }

    cf_pending_t *p = &s->pending[idx];
    actor_id_t requester = p->requester;
    CF_LOG("ws_recv: req_id=%lld type=%s -> actor %llu",
           (long long)req_id, type, (unsigned long long)requester);
    p->occupied = false;

    if (strcmp(type, "ok") == 0 ||
        strcmp(type, "kv_put_ok") == 0 ||
        strcmp(type, "kv_delete_ok") == 0 ||
        strcmp(type, "queue_push_ok") == 0) {
        actor_send(rt, requester, MSG_CF_OK, NULL, 0);
    } else if (strcmp(type, "db_exec_ok") == 0) {
        /* Send rows_affected as payload */
        int64_t affected = json_get_int(json, "rows_affected", 0);
        char buf[32];
        int blen = snprintf(buf, sizeof(buf), "%lld", (long long)affected);
        actor_send(rt, requester, MSG_CF_OK, buf, (size_t)blen);
    } else if (strcmp(type, "value") == 0 ||
               strcmp(type, "kv_get_ok") == 0) {
        char val[CF_REPLY_BUF] = "";
        size_t vlen = json_get_str(json, "value", val, sizeof(val));
        actor_send(rt, requester, MSG_CF_VALUE, val, vlen);
    } else if (strcmp(type, "ai_infer_ok") == 0) {
        char val[CF_REPLY_BUF] = "";
        size_t vlen = json_get_str(json, "result", val, sizeof(val));
        actor_send(rt, requester, MSG_CF_VALUE, val, vlen);
    } else if (strcmp(type, "keys") == 0 ||
               strcmp(type, "kv_list_ok") == 0) {
        char keys[CF_REPLY_BUF] = "";
        size_t klen = json_get_array_str(json, "keys", keys, sizeof(keys));
        actor_send(rt, requester, MSG_CF_KEYS, keys, klen);
    } else if (strcmp(type, "db_query_ok") == 0) {
        /* Pass raw JSON rows array as payload */
        char rows[CF_REPLY_BUF] = "";
        size_t rlen = json_get_str(json, "rows", rows, sizeof(rows));
        actor_send(rt, requester, MSG_CF_ROWS, rows, rlen);
    } else if (strcmp(type, "ai_embed_ok") == 0) {
        /* Pass raw JSON embedding array as payload (may exceed CF_REPLY_BUF) */
        size_t elen = 0;
        const char *emb = json_find_key(json, "embedding", &elen);
        if (emb && elen > 0)
            actor_send(rt, requester, MSG_CF_EMBEDDING, emb, elen);
        else
            actor_send(rt, requester, MSG_CF_ERROR, "no embedding", 12);
    } else if (strcmp(type, "not_found") == 0 ||
               strcmp(type, "kv_not_found") == 0) {
        actor_send(rt, requester, MSG_CF_NOT_FOUND, NULL, 0);
    } else if (strcmp(type, "error") == 0) {
        char err[256] = "unknown error";
        size_t elen = json_get_str(json, "message", err, sizeof(err));
        if (elen == 0) elen = strlen(err);
        actor_send(rt, requester, MSG_CF_ERROR, err, elen);
    }
}

/* ── Actor behavior ───────────────────────────────────────────────── */

#define CF_MSG_CONNECT 1  /* internal trigger to initiate WS connection */

static bool cf_proxy_behavior(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    cf_proxy_state_t *s = state;

    switch (msg->type) {

    case CF_MSG_CONNECT:
        /* Initiate WS connection from within actor context */
        if (!s->connected && s->config.url[0]) {
            s->ws_conn = actor_ws_connect(rt, s->config.url);
            if (s->ws_conn == HTTP_CONN_ID_INVALID)
                schedule_reconnect(s, rt);
        }
        return true;

    case MSG_WS_OPEN:
        CF_LOG("WS connected");
        s->connected = true;
        /* Send auth */
        {
            char json[512];
            json_buf_t j;
            json_init(&j, json, sizeof(json));
            json_obj_open(&j);
            json_str(&j, "type", "auth");
            json_str(&j, "node_id", mk_node_identity());
            json_str(&j, "token", s->config.token);
            json_obj_close(&j);
            ws_send_json(s, rt, json, json_len(&j));
        }
        return true;

    case MSG_WS_MESSAGE: {
        const ws_message_payload_t *p = msg->payload;
        if (p->is_binary) return true; /* ignore binary */
        const char *data = (const char *)ws_message_data(p);
        handle_ws_response(s, rt, data, p->data_size);
        return true;
    }

    case MSG_WS_CLOSED:
    case MSG_WS_ERROR:
        CF_LOG("WS %s (was connected=%d authed=%d)",
               msg->type == MSG_WS_CLOSED ? "closed" : "error",
               s->connected, s->authed);
        s->connected = false;
        s->authed = false;
        s->ws_conn = HTTP_CONN_ID_INVALID;
        fail_all_pending(s, rt, "connection lost");
        schedule_reconnect(s, rt);
        return true;

    case MSG_TIMER:
        /* Reconnect */
        if (!s->connected && s->config.url[0]) {
            s->ws_conn = actor_ws_connect(rt, s->config.url);
            if (s->ws_conn == HTTP_CONN_ID_INVALID)
                schedule_reconnect(s, rt);
        }
        return true;

    case MSG_CF_KV_PUT:
    case MSG_CF_KV_GET:
    case MSG_CF_KV_DELETE:
    case MSG_CF_KV_LIST:
        handle_kv_request(s, rt, msg);
        return true;

    case MSG_CF_DB_QUERY:
    case MSG_CF_DB_EXEC:
        handle_db_request(s, rt, msg);
        return true;

    case MSG_CF_QUEUE_PUSH:
        handle_queue_request(s, rt, msg);
        return true;

    case MSG_CF_AI_INFER:
    case MSG_CF_AI_EMBED:
        handle_ai_request(s, rt, msg);
        return true;

    default:
        return true;
    }
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t cf_proxy_init(runtime_t *rt, const cf_proxy_config_t *config) {
    cf_proxy_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ACTOR_ID_INVALID;

    memcpy(&s->config, config, sizeof(*config));
    s->ws_conn = HTTP_CONN_ID_INVALID;
    s->backoff_ms = CF_BACKOFF_INIT;
    s->next_req_id = 1;

    actor_id_t id = actor_spawn(rt, cf_proxy_behavior, s, free, 32);
    if (id == ACTOR_ID_INVALID) {
        free(s);
        return ACTOR_ID_INVALID;
    }

    /* Register canonical Cloudflare paths (always) */
    static const char *canonical_paths[] = {
        "/node/cloudflare/storage/kv",
        "/node/cloudflare/storage/db",
        "/node/cloudflare/queue/default",
        "/node/cloudflare/ai/infer",
        "/node/cloudflare/ai/embed",
        NULL
    };
    for (int i = 0; canonical_paths[i]; i++)
        actor_register_name(rt, canonical_paths[i], id);

    /* Register virtual short paths (only if unclaimed) */
    static const char *virtual_paths[] = {
        "/node/storage/kv",
        "/node/storage/db",
        "/node/queue/default",
        "/node/ai/infer",
        "/node/ai/embed",
        NULL
    };
    for (int i = 0; virtual_paths[i]; i++) {
        if (actor_lookup(rt, virtual_paths[i]) == ACTOR_ID_INVALID)
            actor_register_name(rt, virtual_paths[i], id);
    }

    /* Trigger WS connection from within actor context */
    if (config->url[0])
        actor_send(rt, id, CF_MSG_CONNECT, NULL, 0);

    return id;
}
