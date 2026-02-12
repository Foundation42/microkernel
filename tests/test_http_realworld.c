#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/mk_socket.h"
#include <signal.h>
#include <string.h>

/* ── Network availability check ──────────────────────────────────── */

static bool network_available(void) {
    mk_socket_t *s = mk_socket_tcp_connect("httpbin.org", 80);
    if (!s) return false;
    s->close(s);
    return true;
}

/* ── Actor test infrastructure ───────────────────────────────────── */

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
} http_rw_state_t;

static bool http_rw_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                              message_t *msg, void *state) {
    http_rw_state_t *s = state;

    if (msg->type == 0) {
        if (s->method && strcmp(s->method, "POST") == 0) {
            const char *hdrs[] = {"Content-Type", "application/x-www-form-urlencoded"};
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

/* ── Tests ────────────────────────────────────────────────────────── */

static int test_http_get_realworld(void) {
    runtime_t *rt = runtime_init(1, 16);
    http_rw_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url), "http://httpbin.org/get");

    actor_id_t aid = actor_spawn(rt, http_rw_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT(strstr(state.body, "httpbin.org") != NULL);

    runtime_destroy(rt);
    return 0;
}

static int test_http_post_realworld(void) {
    runtime_t *rt = runtime_init(1, 16);
    http_rw_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url), "http://httpbin.org/post");
    state.method = "POST";
    state.post_body = "hello";
    state.post_body_size = 5;

    actor_id_t aid = actor_spawn(rt, http_rw_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT(strstr(state.body, "hello") != NULL);

    runtime_destroy(rt);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    if (!network_available()) {
        printf("test_http_realworld: SKIP (no network)\n");
        return 0;
    }

    printf("test_http_realworld:\n");
    RUN_TEST(test_http_get_realworld);
    RUN_TEST(test_http_post_realworld);
    TEST_REPORT();
}
