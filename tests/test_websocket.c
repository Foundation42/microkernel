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

#define TEST_PORT 19882
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

/* Extract Sec-WebSocket-Key from request */
static bool extract_ws_key(const char *req, char *key, size_t key_cap) {
    const char *p = strstr(req, "Sec-WebSocket-Key: ");
    if (!p) return false;
    p += 19;
    const char *end = strstr(p, "\r\n");
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= key_cap) return false;
    memcpy(key, p, len);
    key[len] = '\0';
    return true;
}

/* Build WS accept response */
static void ws_accept_response(int fd, const char *client_key) {
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);

    uint8_t hash[20];
    sha1((const uint8_t *)concat, strlen(concat), hash);

    char accept_b64[32];
    base64_encode(hash, 20, accept_b64);

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n", accept_b64);
    send(fd, resp, (size_t)n, 0);
}

/* Read a WS frame (simplified for test server — expects small unmasked or masked) */
static ssize_t ws_read_frame(int fd, uint8_t *opcode, uint8_t *buf,
                             size_t cap) {
    uint8_t header[14];
    ssize_t n = recv(fd, header, 2, 0);
    if (n < 2) return -1;

    *opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t plen = header[1] & 0x7F;

    if (plen == 126) {
        n = recv(fd, header + 2, 2, 0);
        if (n < 2) return -1;
        plen = ((uint64_t)header[2] << 8) | header[3];
    } else if (plen == 127) {
        n = recv(fd, header + 2, 8, 0);
        if (n < 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | header[2 + i];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        n = recv(fd, mask, 4, 0);
        if (n < 4) return -1;
    }

    if (plen > cap) return -1;

    size_t pos = 0;
    while (pos < plen) {
        n = recv(fd, buf + pos, plen - pos, 0);
        if (n <= 0) return -1;
        pos += (size_t)n;
    }

    if (masked) {
        for (size_t i = 0; i < plen; i++)
            buf[i] ^= mask[i & 3];
    }

    return (ssize_t)plen;
}

/* Send an unmasked WS frame (server → client) */
static void ws_send_frame(int fd, uint8_t opcode, const void *data,
                          size_t len) {
    uint8_t header[10];
    size_t hlen = 0;
    header[hlen++] = 0x80 | opcode; /* FIN + opcode */

    if (len < 126) {
        header[hlen++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[hlen++] = 126;
        header[hlen++] = (uint8_t)(len >> 8);
        header[hlen++] = (uint8_t)(len & 0xFF);
    }

    send(fd, header, hlen, 0);
    if (len > 0) send(fd, data, len, 0);
}

typedef enum {
    WS_ECHO,
    WS_PING_PONG,
    WS_SERVER_CLOSE
} ws_scenario_t;

static pid_t start_ws_server(ws_scenario_t scenario) {
    pid_t pid = fork();
    if (pid != 0) { usleep(50000); return pid; }

    int lfd = listen_tcp(TEST_PORT);
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) _exit(1);

    char req[4096];
    read_request(cfd, req, sizeof(req));

    char key[64];
    if (!extract_ws_key(req, key, sizeof(key))) _exit(1);
    ws_accept_response(cfd, key);
    usleep(10000);

    switch (scenario) {
    case WS_ECHO: {
        /* Read one frame, echo it back, then read close */
        uint8_t buf[1024];
        uint8_t opcode;
        ssize_t n = ws_read_frame(cfd, &opcode, buf, sizeof(buf));
        if (n > 0) {
            ws_send_frame(cfd, opcode, buf, (size_t)n);
        }
        /* Wait for client close or timeout */
        usleep(100000);
        break;
    }
    case WS_PING_PONG: {
        /* Send a ping */
        ws_send_frame(cfd, 0x9, "ping!", 5);
        /* Wait for pong */
        uint8_t buf[64];
        uint8_t opcode;
        ssize_t n = ws_read_frame(cfd, &opcode, buf, sizeof(buf));
        (void)n;
        /* Then send a text message */
        ws_send_frame(cfd, 0x1, "after-ping", 10);
        usleep(100000);
        break;
    }
    case WS_SERVER_CLOSE: {
        /* Send a text message, then close */
        ws_send_frame(cfd, 0x1, "goodbye", 7);
        usleep(10000);
        /* Send close frame: 1000 */
        uint8_t close_payload[2] = {0x03, 0xE8}; /* 1000 */
        ws_send_frame(cfd, 0x8, close_payload, 2);
        usleep(50000);
        break;
    }
    }

    close(cfd);
    close(lfd);
    _exit(0);
}

/* ── Actor test state ──────────────────────────────────────────────── */

#define MAX_WS_MSGS 8

typedef struct {
    bool opened;
    bool closed;
    bool error;
    uint16_t close_code;
    int msg_count;
    char messages[MAX_WS_MSGS][256];
    size_t msg_sizes[MAX_WS_MSGS];
    bool msg_binary[MAX_WS_MSGS];
    char url[128];
    ws_scenario_t scenario;
    http_conn_id_t conn_id;
} ws_test_state_t;

static bool ws_test_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                             message_t *msg, void *state) {
    ws_test_state_t *s = state;

    if (msg->type == 0) {
        s->conn_id = actor_ws_connect(rt, s->url);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->opened = true;
        if (s->scenario == WS_ECHO) {
            actor_ws_send_text(rt, s->conn_id, "hello ws", 8);
        }
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        int idx = s->msg_count;
        if (idx < MAX_WS_MSGS) {
            const void *data = ws_message_data(p);
            size_t len = p->data_size < sizeof(s->messages[idx]) - 1 ?
                         p->data_size : sizeof(s->messages[idx]) - 1;
            memcpy(s->messages[idx], data, len);
            s->messages[idx][len] = '\0';
            s->msg_sizes[idx] = p->data_size;
            s->msg_binary[idx] = p->is_binary;
            s->msg_count++;
        }

        /* For echo test: close after receiving echo */
        if (s->scenario == WS_ECHO) {
            actor_ws_close(rt, s->conn_id, 1000, NULL);
            runtime_stop(rt);
            return false;
        }
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        const ws_status_payload_t *p = msg->payload;
        s->closed = true;
        s->close_code = p->close_code;
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

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_ws_echo(void) {
    pid_t server = start_ws_server(WS_ECHO);

    runtime_t *rt = runtime_init(1, 16);
    ws_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "ws://127.0.0.1:%d/echo", TEST_PORT);
    state.scenario = WS_ECHO;

    actor_id_t aid = actor_spawn(rt, ws_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(state.msg_count >= 1);
    ASSERT(strcmp(state.messages[0], "hello ws") == 0);
    ASSERT(!state.msg_binary[0]);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_ws_ping_pong(void) {
    pid_t server = start_ws_server(WS_PING_PONG);

    runtime_t *rt = runtime_init(1, 16);
    ws_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "ws://127.0.0.1:%d/ping", TEST_PORT);
    state.scenario = WS_PING_PONG;

    actor_id_t aid = actor_spawn(rt, ws_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    /* Should get "after-ping" message (pong auto-handled) */
    ASSERT(state.msg_count >= 1);
    ASSERT(strcmp(state.messages[0], "after-ping") == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_ws_server_close(void) {
    pid_t server = start_ws_server(WS_SERVER_CLOSE);

    runtime_t *rt = runtime_init(1, 16);
    ws_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "ws://127.0.0.1:%d/close", TEST_PORT);
    state.scenario = WS_SERVER_CLOSE;

    actor_id_t aid = actor_spawn(rt, ws_test_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(state.msg_count >= 1);
    ASSERT(strcmp(state.messages[0], "goodbye") == 0);
    ASSERT(state.closed);
    ASSERT_EQ(state.close_code, 1000);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_websocket:\n");
    RUN_TEST(test_ws_echo);
    RUN_TEST(test_ws_ping_pong);
    RUN_TEST(test_ws_server_close);
    TEST_REPORT();
}
