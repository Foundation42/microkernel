#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>

#define TEST_PORT 19881

static int listen_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(fd, 1);
    return fd;
}

static void read_request(int fd, char *buf, size_t cap) {
    size_t pos = 0;
    while (pos < cap - 1) {
        ssize_t n = recv(fd, buf + pos, cap - 1 - pos, 0);
        if (n <= 0) break;
        pos += (size_t)n;
        buf[pos] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
}

typedef enum {
    SSE_SIMPLE_EVENTS,
    SSE_MULTILINE_DATA,
    SSE_WITH_COMMENTS,
    SSE_SERVER_CLOSE
} sse_scenario_t;

static pid_t start_sse_server(sse_scenario_t scenario) {
    pid_t pid = fork();
    if (pid != 0) { usleep(50000); return pid; }

    int lfd = listen_tcp(TEST_PORT);
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) _exit(1);

    char req[4096];
    read_request(cfd, req, sizeof(req));

    /* Send SSE response header */
    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Cache-Control: no-cache\r\n"
                      "\r\n";
    send(cfd, hdr, strlen(hdr), 0);
    usleep(10000);

    switch (scenario) {
    case SSE_SIMPLE_EVENTS:
        send(cfd, "event: greeting\n", 16, 0);
        send(cfd, "data: hello\n\n", 13, 0);
        usleep(10000);
        send(cfd, "data: world\n\n", 13, 0);
        usleep(50000);
        break;

    case SSE_MULTILINE_DATA:
        send(cfd, "data: line1\n", 12, 0);
        send(cfd, "data: line2\n", 12, 0);
        send(cfd, "data: line3\n\n", 13, 0);
        usleep(50000);
        break;

    case SSE_WITH_COMMENTS:
        send(cfd, ": this is a comment\n", 20, 0);
        send(cfd, "data: real data\n\n", 17, 0);
        usleep(50000);
        break;

    case SSE_SERVER_CLOSE:
        send(cfd, "data: bye\n\n", 11, 0);
        usleep(10000);
        /* Close immediately */
        break;
    }

    close(cfd);
    close(lfd);
    _exit(0);
}

/* ── Actor test state ──────────────────────────────────────────────── */

#define MAX_SSE_EVENTS 8

typedef struct {
    bool opened;
    bool closed;
    int event_count;
    char events[MAX_SSE_EVENTS][64];
    char data[MAX_SSE_EVENTS][256];
    char url[128];
} sse_test_state_t;

static bool sse_test_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                              message_t *msg, void *state) {
    sse_test_state_t *s = state;

    if (msg->type == 0) {
        actor_sse_connect(rt, s->url);
        return true;
    }

    if (msg->type == MSG_SSE_OPEN) {
        s->opened = true;
        return true;
    }

    if (msg->type == MSG_SSE_EVENT) {
        const sse_event_payload_t *p = msg->payload;
        int idx = s->event_count;
        if (idx < MAX_SSE_EVENTS) {
            const char *event = sse_event_name(p);
            const char *data = sse_event_data(p);
            size_t elen = p->event_size < sizeof(s->events[idx]) ?
                          p->event_size : sizeof(s->events[idx]) - 1;
            size_t dlen = p->data_size < sizeof(s->data[idx]) ?
                          p->data_size : sizeof(s->data[idx]) - 1;
            memcpy(s->events[idx], event, elen);
            s->events[idx][elen] = '\0';
            memcpy(s->data[idx], data, dlen);
            s->data[idx][dlen] = '\0';
            s->event_count++;
        }
        return true;
    }

    if (msg->type == MSG_SSE_CLOSED) {
        s->closed = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_sse_simple_events(void) {
    pid_t server = start_sse_server(SSE_SIMPLE_EVENTS);

    runtime_t *rt = runtime_init(1, 16);
    sse_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/events", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, sse_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(state.closed);
    ASSERT(state.event_count >= 2);
    /* First event has type "greeting" */
    ASSERT(strcmp(state.events[0], "greeting") == 0);
    ASSERT(strcmp(state.data[0], "hello") == 0);
    /* Second event has default type "message" */
    ASSERT(strcmp(state.events[1], "message") == 0);
    ASSERT(strcmp(state.data[1], "world") == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_sse_multiline_data(void) {
    pid_t server = start_sse_server(SSE_MULTILINE_DATA);

    runtime_t *rt = runtime_init(1, 16);
    sse_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/multi", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, sse_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.event_count >= 1);
    /* Multiline data joined with \n */
    ASSERT(strcmp(state.data[0], "line1\nline2\nline3") == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_sse_comments_ignored(void) {
    pid_t server = start_sse_server(SSE_WITH_COMMENTS);

    runtime_t *rt = runtime_init(1, 16);
    sse_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/comments", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, sse_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.event_count >= 1);
    ASSERT(strcmp(state.data[0], "real data") == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_sse_server_close(void) {
    pid_t server = start_sse_server(SSE_SERVER_CLOSE);

    runtime_t *rt = runtime_init(1, 16);
    sse_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/close", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, sse_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(state.closed);
    ASSERT(state.event_count >= 1);
    ASSERT(strcmp(state.data[0], "bye") == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_sse:\n");
    RUN_TEST(test_sse_simple_events);
    RUN_TEST(test_sse_multiline_data);
    RUN_TEST(test_sse_comments_ignored);
    RUN_TEST(test_sse_server_close);
    TEST_REPORT();
}
