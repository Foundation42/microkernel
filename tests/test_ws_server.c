#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/http.h"
#include "microkernel/services.h"
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

#define TEST_PORT 19886
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ── Client helpers ────────────────────────────────────────────────── */

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

/* Send WS upgrade and verify 101 */
static bool ws_client_handshake(int fd) {
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET /ws HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "\r\n", key);
    send(fd, req, (size_t)n, 0);

    char resp[1024];
    size_t pos = 0;
    for (int i = 0; i < 100 && pos < sizeof(resp) - 1; i++) {
        ssize_t r = recv(fd, resp + pos, sizeof(resp) - 1 - pos, 0);
        if (r <= 0) break;
        pos += (size_t)r;
        resp[pos] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
        usleep(5000);
    }

    return strstr(resp, "101 Switching Protocols") != NULL;
}

/* Send a masked client WS frame */
static void ws_client_send(int fd, uint8_t opcode, const void *data,
                           size_t len) {
    uint8_t header[14];
    size_t hlen = 0;
    header[hlen++] = 0x80 | opcode;

    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    if (len < 126) {
        header[hlen++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (uint8_t)(len >> 8);
        header[hlen++] = (uint8_t)(len & 0xFF);
    }

    memcpy(header + hlen, mask, 4);
    hlen += 4;
    send(fd, header, hlen, 0);

    if (len > 0) {
        uint8_t *masked = malloc(len);
        memcpy(masked, data, len);
        for (size_t i = 0; i < len; i++)
            masked[i] ^= mask[i & 3];
        send(fd, masked, len, 0);
        free(masked);
    }
}

/* Read an unmasked server WS frame */
static ssize_t ws_client_recv(int fd, uint8_t *opcode_out, uint8_t *buf,
                              size_t cap) {
    uint8_t hdr[2];
    ssize_t n = recv(fd, hdr, 2, 0);
    if (n < 2) return -1;

    *opcode_out = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t plen = hdr[1] & 0x7F;

    if (plen == 126) {
        uint8_t ext[2];
        n = recv(fd, ext, 2, 0);
        if (n < 2) return -1;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        n = recv(fd, ext, 8, 0);
        if (n < 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
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

/* ── Server actor ──────────────────────────────────────────────────── */

typedef struct {
    uint16_t port;
    int scenario;
    http_conn_id_t ws_conn;
    bool opened;
    bool closed;
    uint16_t close_code;
    int msg_count;
    char messages[4][256];
    size_t msg_sizes[4];
} ws_server_state_t;

enum {
    WS_SRV_ECHO,
    WS_SRV_CLOSE,
    WS_SRV_CLIENT_CLOSE
};

static bool ws_server_behavior(runtime_t *rt,
                               actor_t *self __attribute__((unused)),
                               message_t *msg, void *state) {
    ws_server_state_t *s = state;

    if (msg->type == 0) {
        actor_http_listen(rt, s->port);
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->ws_conn = p->conn_id;
        actor_ws_accept(rt, p->conn_id);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->opened = true;
        if (s->scenario == WS_SRV_CLOSE) {
            /* Server initiates: send a message then close */
            actor_ws_send_text(rt, s->ws_conn, "goodbye", 7);
            actor_ws_close(rt, s->ws_conn, 1000, NULL);
            runtime_stop(rt);
            return false;
        }
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

        if (s->scenario == WS_SRV_ECHO) {
            /* Echo it back */
            const void *data = ws_message_data(p);
            actor_ws_send_text(rt, s->ws_conn, data, p->data_size);
            /* Wait for client close or timeout */
            actor_set_timer(rt, 200, false);
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

    if (msg->type == MSG_TIMER) {
        /* Timeout — stop */
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_ws_server_echo(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT, 50);
        if (fd < 0) _exit(1);

        if (!ws_client_handshake(fd)) _exit(2);

        /* Send text */
        ws_client_send(fd, 0x1, "hello ws", 8);

        /* Receive echo */
        uint8_t buf[256];
        uint8_t opcode;
        ssize_t n = ws_client_recv(fd, &opcode, buf, sizeof(buf));
        close(fd);

        if (n != 8) _exit(3);
        if (opcode != 0x1) _exit(4);
        buf[n] = '\0';
        if (strcmp((char *)buf, "hello ws") != 0) _exit(5);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    ws_server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT;
    state.scenario = WS_SRV_ECHO;

    actor_id_t aid = actor_spawn(rt, ws_server_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT_EQ(state.msg_count, 1);
    ASSERT(strcmp(state.messages[0], "hello ws") == 0);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_ws_server_close(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 1, 50);
        if (fd < 0) _exit(1);

        if (!ws_client_handshake(fd)) _exit(2);

        /* Server should send "goodbye" then close */
        uint8_t buf[256];
        uint8_t opcode;

        /* Read text message */
        ssize_t n = ws_client_recv(fd, &opcode, buf, sizeof(buf));
        if (n < 0 || opcode != 0x1) _exit(3);
        buf[n] = '\0';
        if (strcmp((char *)buf, "goodbye") != 0) _exit(4);

        /* Read close frame */
        n = ws_client_recv(fd, &opcode, buf, sizeof(buf));
        if (opcode != 0x8) _exit(5);
        close(fd);

        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    ws_server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 1;
    state.scenario = WS_SRV_CLOSE;

    actor_id_t aid = actor_spawn(rt, ws_server_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_ws_server_client_close(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 2, 50);
        if (fd < 0) _exit(1);

        if (!ws_client_handshake(fd)) _exit(2);

        /* Send a message then close */
        ws_client_send(fd, 0x1, "test msg", 8);
        usleep(50000);

        /* Send close frame: code 1000 */
        uint8_t close_payload[2] = {0x03, 0xE8};
        ws_client_send(fd, 0x8, close_payload, 2);

        /* Read close response */
        uint8_t buf[256];
        uint8_t opcode;
        ws_client_recv(fd, &opcode, buf, sizeof(buf));

        close(fd);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    ws_server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 2;
    state.scenario = WS_SRV_CLIENT_CLOSE;

    actor_id_t aid = actor_spawn(rt, ws_server_behavior, &state, NULL, 32);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.opened);
    ASSERT(state.closed);
    ASSERT_EQ(state.close_code, 1000);
    ASSERT_EQ(state.msg_count, 1);
    ASSERT(strcmp(state.messages[0], "test msg") == 0);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_ws_server:\n");
    RUN_TEST(test_ws_server_echo);
    RUN_TEST(test_ws_server_close);
    RUN_TEST(test_ws_server_client_close);
    TEST_REPORT();
}
