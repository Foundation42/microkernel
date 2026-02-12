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
    mk_socket_t *s = mk_socket_tcp_connect("echo.websocket.events", 80);
    if (!s) return false;
    s->close(s);
    return true;
}

/* ── Actor test state ────────────────────────────────────────────── */

typedef struct {
    bool opened;
    bool closed;
    bool error;
    int msg_count;
    char messages[4][256];
    size_t msg_sizes[4];
    char url[256];
    http_conn_id_t conn_id;
} ws_rw_state_t;

static bool ws_rw_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                            message_t *msg, void *state) {
    ws_rw_state_t *s = state;

    if (msg->type == 0) {
        s->conn_id = actor_ws_connect(rt, s->url);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->opened = true;
        actor_ws_send_text(rt, s->conn_id, "ping test", 9);
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
            s->msg_sizes[idx] = p->data_size;
            s->msg_count++;
        }

        /* Check if we got the echo back */
        if (s->msg_count > 0 && strstr(s->messages[s->msg_count - 1], "ping test")) {
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

static int test_ws_echo_realworld(void) {
    runtime_t *rt = runtime_init(1, 32);
    ws_rw_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "ws://echo.websocket.events/");

    actor_id_t aid = actor_spawn(rt, ws_rw_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(!state.error);
    /* echo.websocket.events may send a welcome message first, then echo */
    bool found_echo = false;
    for (int i = 0; i < state.msg_count; i++) {
        if (strstr(state.messages[i], "ping test")) {
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

    if (!network_available()) {
        printf("test_ws_realworld: SKIP (no network)\n");
        return 0;
    }

    printf("test_ws_realworld:\n");
    RUN_TEST(test_ws_echo_realworld);
    TEST_REPORT();
}
