#ifndef MICROKERNEL_HTTP_H
#define MICROKERNEL_HTTP_H

#include "types.h"

/* ── Connection ID ──────────────────────────────────────────────────── */

typedef uint32_t http_conn_id_t;
#define HTTP_CONN_ID_INVALID ((http_conn_id_t)0)

/* ── HTTP response payload (MSG_HTTP_RESPONSE) ─────────────────────── */

typedef struct {
    http_conn_id_t conn_id;
    int            status_code;
    size_t         headers_size;  /* packed "Key: Value\0" pairs after struct */
    size_t         body_size;     /* body bytes after headers */
} http_response_payload_t;

static inline const char *http_response_headers(const http_response_payload_t *p) {
    return (const char *)(p + 1);
}

static inline const void *http_response_body(const http_response_payload_t *p) {
    return (const uint8_t *)(p + 1) + p->headers_size;
}

/* ── HTTP error payload (MSG_HTTP_ERROR) ───────────────────────────── */

typedef struct {
    http_conn_id_t conn_id;
    int            error_code;
    char           message[128];
} http_error_payload_t;

/* ── SSE payloads ──────────────────────────────────────────────────── */

typedef struct {
    http_conn_id_t conn_id;
    int            status_code;
} sse_status_payload_t;

typedef struct {
    http_conn_id_t conn_id;
    size_t         event_size;  /* event name bytes after struct */
    size_t         data_size;   /* data bytes after event name */
} sse_event_payload_t;

static inline const char *sse_event_name(const sse_event_payload_t *p) {
    return (const char *)(p + 1);
}

static inline const char *sse_event_data(const sse_event_payload_t *p) {
    return (const char *)(p + 1) + p->event_size;
}

/* ── WebSocket payloads ───────────────────────────────────────────── */

typedef struct {
    http_conn_id_t conn_id;
    uint16_t       close_code;
} ws_status_payload_t;

typedef struct {
    http_conn_id_t conn_id;
    bool           is_binary;
    size_t         data_size;  /* data bytes after struct */
} ws_message_payload_t;

static inline const void *ws_message_data(const ws_message_payload_t *p) {
    return (const void *)(p + 1);
}

/* ── HTTP request payload (MSG_HTTP_REQUEST) — server-side ────────── */

typedef struct {
    http_conn_id_t conn_id;
    size_t method_size;    /* includes \0 */
    size_t path_size;      /* includes \0 */
    size_t headers_size;   /* packed "Key: Value\0" pairs */
    size_t body_size;
    /* followed by: [method\0][path\0][packed headers][body] */
} http_request_payload_t;

static inline const char *http_request_method(const http_request_payload_t *p) {
    return (const char *)(p + 1);
}

static inline const char *http_request_path(const http_request_payload_t *p) {
    return (const char *)(p + 1) + p->method_size;
}

static inline const char *http_request_headers(const http_request_payload_t *p) {
    return (const char *)(p + 1) + p->method_size + p->path_size;
}

static inline const void *http_request_body(const http_request_payload_t *p) {
    return (const uint8_t *)(p + 1) + p->method_size + p->path_size +
           p->headers_size;
}

/* ── Actor-facing APIs ────────────────────────────────────────────── */

http_conn_id_t actor_http_fetch(runtime_t *rt, const char *method,
                                const char *url, const char *const *headers,
                                size_t n_headers, const void *body,
                                size_t body_size);
http_conn_id_t actor_http_get(runtime_t *rt, const char *url);

http_conn_id_t actor_sse_connect(runtime_t *rt, const char *url);

http_conn_id_t actor_ws_connect(runtime_t *rt, const char *url);
bool actor_ws_send_text(runtime_t *rt, http_conn_id_t id,
                        const char *text, size_t len);
bool actor_ws_send_binary(runtime_t *rt, http_conn_id_t id,
                          const void *data, size_t len);
bool actor_ws_close(runtime_t *rt, http_conn_id_t id,
                    uint16_t code, const char *reason);

void actor_http_close(runtime_t *rt, http_conn_id_t id);

/* ── Server-side APIs (Phase 5) ──────────────────────────────────── */

bool actor_http_listen(runtime_t *rt, uint16_t port);
bool actor_http_unlisten(runtime_t *rt, uint16_t port);

bool actor_http_respond(runtime_t *rt, http_conn_id_t conn_id,
                        int status_code,
                        const char *const *headers, size_t n_headers,
                        const void *body, size_t body_size);

bool actor_sse_start(runtime_t *rt, http_conn_id_t conn_id);
bool actor_sse_push(runtime_t *rt, http_conn_id_t conn_id,
                    const char *event, const char *data, size_t data_size);

bool actor_ws_accept(runtime_t *rt, http_conn_id_t conn_id);

#endif /* MICROKERNEL_HTTP_H */
