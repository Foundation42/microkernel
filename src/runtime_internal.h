#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include "microkernel/runtime.h"
#include "microkernel/services.h"
#include "microkernel/mk_socket.h"

/* Internal types shared between runtime.c and service modules */

typedef struct {
    timer_id_t  id;       /* 0 = unused */
    actor_id_t  owner;
    int         fd;       /* timerfd (Linux) */
    bool        periodic;
} timer_entry_t;

typedef struct {
    char       name[64];
    actor_id_t actor_id;
    bool       occupied;
} name_entry_t;

/* ── HTTP connection state machine ─────────────────────────────────── */

typedef enum {
    HTTP_STATE_IDLE,
    HTTP_STATE_SENDING,
    HTTP_STATE_RECV_STATUS,
    HTTP_STATE_RECV_HEADERS,
    HTTP_STATE_BODY_CONTENT,
    HTTP_STATE_BODY_CHUNKED,
    HTTP_STATE_BODY_STREAM,     /* SSE */
    HTTP_STATE_WS_ACTIVE,
    /* Server-side states */
    HTTP_STATE_SRV_RECV_REQUEST,
    HTTP_STATE_SRV_RECV_HEADERS,
    HTTP_STATE_SRV_RECV_BODY,
    HTTP_STATE_SRV_SENDING,
    HTTP_STATE_SRV_SSE_ACTIVE,
    HTTP_STATE_DONE,
    HTTP_STATE_ERROR
} http_state_t;

typedef enum {
    HTTP_CONN_HTTP,
    HTTP_CONN_SSE,
    HTTP_CONN_WS,
    HTTP_CONN_SERVER,
    HTTP_CONN_SERVER_SSE,
    HTTP_CONN_SERVER_WS
} http_conn_type_t;

#define HTTP_READ_BUF_SIZE 8192
#define MAX_HTTP_CONNS 32

typedef struct {
    http_conn_id_t   id;          /* 0 = unused slot */
    http_state_t     state;
    http_conn_type_t conn_type;
    actor_id_t       owner;
    mk_socket_t     *sock;

    /* Request buffer (SENDING state) */
    uint8_t         *send_buf;
    size_t           send_size;
    size_t           send_pos;

    /* Read buffer (sliding window) */
    uint8_t          read_buf[HTTP_READ_BUF_SIZE];
    size_t           read_len;    /* bytes of valid data from index 0 */

    /* Response state */
    int              status_code;
    int64_t          content_length; /* -1 = unknown */
    bool             chunked;
    bool             upgrade_ws;

    /* Headers accumulator: "Key: Value\0Key: Value\0" */
    char            *headers_buf;
    size_t           headers_size;
    size_t           headers_cap;

    /* Body accumulator */
    uint8_t         *body_buf;
    size_t           body_size;
    size_t           body_cap;

    /* Chunked transfer state */
    size_t           chunk_remaining;
    bool             in_chunk_data;

    /* SSE state */
    char             sse_event[256];
    char            *sse_data;
    size_t           sse_data_size;
    size_t           sse_data_cap;

    /* WebSocket state */
    char             ws_accept_key[29]; /* expected Sec-WebSocket-Accept */

    /* Server-side fields */
    bool             is_server;
    char            *request_method;
    char            *request_path;
} http_conn_t;

/* ── HTTP listener (server-side) ──────────────────────────────────── */

#define MAX_HTTP_LISTENERS 8

typedef struct {
    int         listen_fd;   /* -1 = unused */
    uint16_t    port;
    actor_id_t  owner;
} http_listener_t;

/* ── Accessors for runtime internals (defined in runtime.c) ────────── */

timer_entry_t *runtime_get_timers(runtime_t *rt);
size_t         runtime_get_max_timers(void);
uint32_t       runtime_alloc_timer_id(runtime_t *rt);
actor_id_t     runtime_current_actor_id(runtime_t *rt);

actor_id_t     runtime_get_log_actor(runtime_t *rt);
void           runtime_set_log_actor(runtime_t *rt, actor_id_t id);
int            runtime_get_min_log_level(runtime_t *rt);

name_entry_t  *runtime_get_name_registry(runtime_t *rt);
size_t         runtime_get_name_registry_size(void);

/* Phase 3.5: HTTP connection accessors */
http_conn_t   *runtime_get_http_conns(runtime_t *rt);
size_t         runtime_get_max_http_conns(void);
uint32_t       runtime_alloc_http_conn_id(runtime_t *rt);

/* Phase 5: HTTP listener accessors */
http_listener_t *runtime_get_http_listeners(runtime_t *rt);

/* Deliver a message to a local actor (used by http_conn.c) */
bool runtime_deliver_msg(runtime_t *rt, actor_id_t dest, msg_type_t type,
                         const void *payload, size_t payload_size);

/* Platform-specific timer fd cleanup (Linux: close; ESP32: stop esp_timer + close eventfd) */
void timer_platform_close(size_t slot, int fd);

/* Drive an HTTP connection (called from runtime.c poll loop) */
void http_conn_drive(http_conn_t *conn, short revents, runtime_t *rt);

#endif /* RUNTIME_INTERNAL_H */
