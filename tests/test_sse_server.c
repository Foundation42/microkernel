#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/http.h"
#include "microkernel/services.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

#define TEST_PORT 19885

/* ── Client helper ─────────────────────────────────────────────────── */

static int connect_retry(uint16_t port, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = inet_addr("127.0.0.1")
        };

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;

        close(fd);
        usleep(20000);
    }
    return -1;
}

/* ── Server actor ──────────────────────────────────────────────────── */

typedef struct {
    uint16_t port;
    int scenario;
    http_conn_id_t sse_conn;
    int events_pushed;
    bool conn_closed;
} sse_server_state_t;

enum {
    SSE_PUSH,
    SSE_NAMED_EVENTS,
    SSE_CLIENT_DISCONNECT
};

static bool sse_server_behavior(runtime_t *rt,
                                actor_t *self __attribute__((unused)),
                                message_t *msg, void *state) {
    sse_server_state_t *s = state;

    if (msg->type == 0) {
        actor_http_listen(rt, s->port);
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->sse_conn = p->conn_id;
        actor_sse_start(rt, p->conn_id);

        /* Push events immediately after SSE start (which is async via SENDING) */
        /* We use a timer to push after the headers are sent */
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_TIMER) {
        switch (s->scenario) {
        case SSE_PUSH:
            actor_sse_push(rt, s->sse_conn, NULL, "event1", 6);
            actor_sse_push(rt, s->sse_conn, NULL, "event2", 6);
            /* Close after a short delay so client can read */
            actor_set_timer(rt, 50, false);
            s->events_pushed = 2;
            return true;

        case SSE_NAMED_EVENTS:
            actor_sse_push(rt, s->sse_conn, "update", "data1", 5);
            actor_sse_push(rt, s->sse_conn, "notify", "data2", 5);
            actor_set_timer(rt, 50, false);
            s->events_pushed = 2;
            return true;

        case SSE_CLIENT_DISCONNECT:
            if (s->events_pushed == 0) {
                actor_sse_push(rt, s->sse_conn, NULL, "waiting", 7);
                s->events_pushed = 1;
                /* Keep waiting for client disconnect */
                return true;
            }
            /* Second timer = timeout, stop */
            runtime_stop(rt);
            return false;
        }

        if (s->events_pushed > 0) {
            actor_http_close(rt, s->sse_conn);
            runtime_stop(rt);
            return false;
        }
        return true;
    }

    if (msg->type == MSG_HTTP_CONN_CLOSED) {
        s->conn_closed = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_sse_push(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT, 50);
        if (fd < 0) _exit(1);

        const char *req = "GET /events HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Accept: text/event-stream\r\n"
                          "\r\n";
        send(fd, req, strlen(req), 0);

        /* Read SSE headers + events */
        char buf[4096];
        size_t pos = 0;
        for (int i = 0; i < 100 && pos < sizeof(buf) - 1; i++) {
            ssize_t n = recv(fd, buf + pos, sizeof(buf) - 1 - pos, 0);
            if (n <= 0) break;
            pos += (size_t)n;
            buf[pos] = '\0';
            /* Look for two events */
            if (strstr(buf, "data: event1") && strstr(buf, "data: event2"))
                break;
            usleep(10000);
        }
        close(fd);

        if (!strstr(buf, "text/event-stream")) _exit(2);
        if (!strstr(buf, "data: event1")) _exit(3);
        if (!strstr(buf, "data: event2")) _exit(4);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    sse_server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT;
    state.scenario = SSE_PUSH;

    actor_id_t aid = actor_spawn(rt, sse_server_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.events_pushed, 2);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_sse_named_events(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 1, 50);
        if (fd < 0) _exit(1);

        const char *req = "GET /events HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        send(fd, req, strlen(req), 0);

        char buf[4096];
        size_t pos = 0;
        for (int i = 0; i < 100 && pos < sizeof(buf) - 1; i++) {
            ssize_t n = recv(fd, buf + pos, sizeof(buf) - 1 - pos, 0);
            if (n <= 0) break;
            pos += (size_t)n;
            buf[pos] = '\0';
            if (strstr(buf, "event: update") && strstr(buf, "event: notify"))
                break;
            usleep(10000);
        }
        close(fd);

        if (!strstr(buf, "event: update\ndata: data1")) _exit(2);
        if (!strstr(buf, "event: notify\ndata: data2")) _exit(3);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    sse_server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 1;
    state.scenario = SSE_NAMED_EVENTS;

    actor_id_t aid = actor_spawn(rt, sse_server_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.events_pushed, 2);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_sse_client_disconnect(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 2, 50);
        if (fd < 0) _exit(1);

        const char *req = "GET /events HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        send(fd, req, strlen(req), 0);

        /* Read headers + first event */
        char buf[4096];
        size_t pos = 0;
        for (int i = 0; i < 100 && pos < sizeof(buf) - 1; i++) {
            ssize_t n = recv(fd, buf + pos, sizeof(buf) - 1 - pos, 0);
            if (n <= 0) break;
            pos += (size_t)n;
            buf[pos] = '\0';
            if (strstr(buf, "data: waiting")) break;
            usleep(10000);
        }

        /* Now disconnect */
        close(fd);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    sse_server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 2;
    state.scenario = SSE_CLIENT_DISCONNECT;

    actor_id_t aid = actor_spawn(rt, sse_server_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.conn_closed);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_sse_server:\n");
    RUN_TEST(test_sse_push);
    RUN_TEST(test_sse_named_events);
    RUN_TEST(test_sse_client_disconnect);
    TEST_REPORT();
}
