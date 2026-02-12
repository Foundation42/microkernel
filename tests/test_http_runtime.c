#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "sha1.h"
#include "base64.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

#define TEST_PORT 19883
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

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
    listen(fd, 4);
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

/* ── Multi-protocol server ─────────────────────────────────────────── */
/* Handles 3 sequential connections: HTTP, SSE, WS */

static pid_t start_multi_server(void) {
    pid_t pid = fork();
    if (pid != 0) { usleep(50000); return pid; }

    int lfd = listen_tcp(TEST_PORT);

    /* Connection 1: HTTP GET */
    {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) _exit(1);
        char req[4096];
        read_request(cfd, req, sizeof(req));
        const char *resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 13\r\n"
                           "\r\n"
                           "hello runtime";
        send(cfd, resp, strlen(resp), 0);
        close(cfd);
    }

    /* Connection 2: SSE */
    {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) _exit(1);
        char req[4096];
        read_request(cfd, req, sizeof(req));
        const char *hdr = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "\r\n";
        send(cfd, hdr, strlen(hdr), 0);
        usleep(10000);
        send(cfd, "data: sse-test\n\n", 16, 0);
        usleep(50000);
        close(cfd);
    }

    /* Connection 3: WebSocket */
    {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) _exit(1);
        char req[4096];
        read_request(cfd, req, sizeof(req));

        /* WS handshake */
        const char *key_start = strstr(req, "Sec-WebSocket-Key: ");
        if (key_start) {
            key_start += 19;
            const char *key_end = strstr(key_start, "\r\n");
            if (key_end) {
                char key[64];
                size_t klen = (size_t)(key_end - key_start);
                memcpy(key, key_start, klen);
                key[klen] = '\0';

                char concat[128];
                snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
                uint8_t hash[20];
                sha1((const uint8_t *)concat, strlen(concat), hash);
                char accept[32];
                base64_encode(hash, 20, accept);

                char resp[512];
                int n = snprintf(resp, sizeof(resp),
                                 "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: %s\r\n"
                                 "\r\n", accept);
                send(cfd, resp, (size_t)n, 0);
            }
        }
        usleep(10000);

        /* Send a text message then close */
        uint8_t frame[64];
        const char *msg = "ws-runtime";
        size_t mlen = strlen(msg);
        frame[0] = 0x81; /* FIN + text */
        frame[1] = (uint8_t)mlen;
        memcpy(frame + 2, msg, mlen);
        send(cfd, frame, 2 + mlen, 0);
        usleep(10000);

        /* Send close */
        frame[0] = 0x88; /* FIN + close */
        frame[1] = 2;
        frame[2] = 0x03; frame[3] = 0xE8; /* 1000 */
        send(cfd, frame, 4, 0);
        usleep(100000);
        close(cfd);
    }

    close(lfd);
    _exit(0);
}

/* ── Multi-protocol actor ──────────────────────────────────────────── */

typedef struct {
    int phase; /* 0=init, 1=waiting http, 2=waiting sse, 3=waiting ws */
    /* HTTP results */
    bool http_ok;
    int http_status;
    char http_body[64];
    /* SSE results */
    bool sse_opened;
    bool sse_closed;
    char sse_data[64];
    /* WS results */
    bool ws_opened;
    bool ws_closed;
    char ws_message[64];
    uint16_t ws_close_code;

    char base_url[64];
} multi_test_state_t;

static bool multi_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                           message_t *msg, void *state) {
    multi_test_state_t *s = state;

    if (msg->type == 0) {
        /* Start HTTP request */
        s->phase = 1;
        char url[128];
        snprintf(url, sizeof(url), "http://%s/test", s->base_url);
        actor_http_get(rt, url);
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE && s->phase == 1) {
        const http_response_payload_t *p = msg->payload;
        s->http_ok = true;
        s->http_status = p->status_code;
        size_t copy = p->body_size < sizeof(s->http_body) - 1 ?
                      p->body_size : sizeof(s->http_body) - 1;
        memcpy(s->http_body, http_response_body(p), copy);
        s->http_body[copy] = '\0';

        /* Start SSE */
        s->phase = 2;
        char url[128];
        snprintf(url, sizeof(url), "http://%s/events", s->base_url);
        actor_sse_connect(rt, url);
        return true;
    }

    if (msg->type == MSG_SSE_OPEN) {
        s->sse_opened = true;
        return true;
    }

    if (msg->type == MSG_SSE_EVENT) {
        const sse_event_payload_t *p = msg->payload;
        size_t copy = p->data_size < sizeof(s->sse_data) - 1 ?
                      p->data_size : sizeof(s->sse_data) - 1;
        memcpy(s->sse_data, sse_event_data(p), copy);
        s->sse_data[copy] = '\0';
        return true;
    }

    if (msg->type == MSG_SSE_CLOSED) {
        s->sse_closed = true;

        /* Start WebSocket */
        s->phase = 3;
        char url[128];
        snprintf(url, sizeof(url), "ws://%s/ws", s->base_url);
        actor_ws_connect(rt, url);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->ws_opened = true;
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        size_t copy = p->data_size < sizeof(s->ws_message) - 1 ?
                      p->data_size : sizeof(s->ws_message) - 1;
        memcpy(s->ws_message, ws_message_data(p), copy);
        s->ws_message[copy] = '\0';
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        const ws_status_payload_t *p = msg->payload;
        s->ws_closed = true;
        s->ws_close_code = p->close_code;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR || msg->type == MSG_WS_ERROR) {
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_full_integration(void) {
    pid_t server = start_multi_server();

    runtime_t *rt = runtime_init(1, 16);
    multi_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.base_url, sizeof(state.base_url),
             "127.0.0.1:%d", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, multi_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    /* Verify HTTP */
    ASSERT(state.http_ok);
    ASSERT_EQ(state.http_status, 200);
    ASSERT(strcmp(state.http_body, "hello runtime") == 0);

    /* Verify SSE */
    ASSERT(state.sse_opened);
    ASSERT(state.sse_closed);
    ASSERT(strcmp(state.sse_data, "sse-test") == 0);

    /* Verify WebSocket */
    ASSERT(state.ws_opened);
    ASSERT(state.ws_closed);
    ASSERT(strcmp(state.ws_message, "ws-runtime") == 0);
    ASSERT_EQ(state.ws_close_code, 1000);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_http_runtime:\n");
    RUN_TEST(test_full_integration);
    TEST_REPORT();
}
