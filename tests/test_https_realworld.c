#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/mk_socket.h"
#include <signal.h>
#include <string.h>

#ifndef HAVE_OPENSSL

int main(void) {
    printf("test_https_realworld: SKIP (no OpenSSL)\n");
    return 0;
}

#else

/* ── Network availability check ──────────────────────────────────── */

static bool tls_host_reachable(const char *host) {
    mk_socket_t *s = mk_socket_tls_connect(host, 443);
    if (!s) return false;
    s->close(s);
    return true;
}

/* ── HTTP actor test infrastructure ──────────────────────────────── */

typedef struct {
    int status_code;
    char body[4096];
    size_t body_size;
    bool got_response;
    bool got_error;
    char url[256];
    const char *method;
    const char *post_body;
    size_t post_body_size;
} https_rw_state_t;

static bool https_rw_behavior(runtime_t *rt,
                               actor_t *self __attribute__((unused)),
                               message_t *msg, void *state) {
    https_rw_state_t *s = state;

    if (msg->type == 0) {
        if (s->method && strcmp(s->method, "POST") == 0) {
            const char *hdrs[] = {
                "Content-Type: application/x-www-form-urlencoded"
            };
            actor_http_fetch(rt, "POST", s->url, hdrs, 1,
                             s->post_body, s->post_body_size);
        } else {
            actor_http_get(rt, s->url);
        }
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->status_code = p->status_code;
        s->body_size = p->body_size < sizeof(s->body) - 1 ?
                       p->body_size : sizeof(s->body) - 1;
        memcpy(s->body, http_response_body(p), s->body_size);
        s->body[s->body_size] = '\0';
        s->got_response = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        s->got_error = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── WebSocket actor test infrastructure ─────────────────────────── */

typedef struct {
    bool opened;
    bool closed;
    bool error;
    int msg_count;
    char messages[4][256];
    char url[256];
    http_conn_id_t conn_id;
} wss_rw_state_t;

static bool wss_rw_behavior(runtime_t *rt,
                             actor_t *self __attribute__((unused)),
                             message_t *msg, void *state) {
    wss_rw_state_t *s = state;

    if (msg->type == 0) {
        s->conn_id = actor_ws_connect(rt, s->url);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->opened = true;
        actor_ws_send_text(rt, s->conn_id, "tls ping", 8);
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        int idx = s->msg_count;
        if (idx < 4) {
            const void *data = ws_message_data(p);
            size_t len = p->data_size < sizeof(s->messages[idx]) - 1 ?
                         p->data_size : sizeof(s->messages[idx]) - 1;
            memcpy(s->messages[idx], data, len);
            s->messages[idx][len] = '\0';
            s->msg_count++;
        }

        if (s->msg_count > 0 &&
            strstr(s->messages[s->msg_count - 1], "tls ping")) {
            actor_ws_close(rt, s->conn_id, 1000, NULL);
            runtime_stop(rt);
            return false;
        }
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        s->closed = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_WS_ERROR) {
        s->error = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── Tests ────────────────────────────────────────────────────────── */

static int test_https_get(void) {
    runtime_t *rt = runtime_init(1, 16);
    https_rw_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url), "https://httpbin.org/get");

    actor_id_t aid = actor_spawn(rt, https_rw_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT(strstr(state.body, "httpbin.org") != NULL);

    runtime_destroy(rt);
    return 0;
}

static int test_https_post(void) {
    runtime_t *rt = runtime_init(1, 16);
    https_rw_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url), "https://httpbin.org/post");
    state.method = "POST";
    state.post_body = "hello_tls";
    state.post_body_size = 9;

    actor_id_t aid = actor_spawn(rt, https_rw_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT(strstr(state.body, "hello_tls") != NULL);

    runtime_destroy(rt);
    return 0;
}

static int test_wss_echo(void) {
    runtime_t *rt = runtime_init(1, 32);
    wss_rw_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url), "wss://ws.postman-echo.com/raw");

    actor_id_t aid = actor_spawn(rt, wss_rw_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(!state.error);

    bool found_echo = false;
    for (int i = 0; i < state.msg_count; i++) {
        if (strstr(state.messages[i], "tls ping")) {
            found_echo = true;
            break;
        }
    }
    ASSERT(found_echo);

    runtime_destroy(rt);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    if (!tls_host_reachable("httpbin.org")) {
        printf("test_https_realworld: SKIP (no network or TLS failed)\n");
        return 0;
    }

    printf("test_https_realworld:\n");
    RUN_TEST(test_https_get);
    RUN_TEST(test_https_post);

    if (tls_host_reachable("ws.postman-echo.com")) {
        RUN_TEST(test_wss_echo);
    } else {
        printf("  SKIP test_wss_echo (ws.postman-echo.com unreachable)\n");
    }

    TEST_REPORT();
}

#endif /* HAVE_OPENSSL */
