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
#include <fcntl.h>

#define TEST_PORT 19884

/* ── Client helper: connect with retries ───────────────────────────── */

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

/* Read full HTTP response (headers + body) into buffer */
static size_t read_response(int fd, char *buf, size_t cap) {
    size_t pos = 0;
    while (pos < cap - 1) {
        ssize_t n = recv(fd, buf + pos, cap - 1 - pos, 0);
        if (n <= 0) break;
        pos += (size_t)n;
        buf[pos] = '\0';

        /* Check if we've received full response */
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            /* Check for Content-Length */
            char *cl = strstr(buf, "Content-Length: ");
            if (cl) {
                size_t body_start = (size_t)(hdr_end + 4 - buf);
                int content_len = atoi(cl + 16);
                if (pos >= body_start + (size_t)content_len)
                    break;
            } else {
                /* No Content-Length — check for Connection: close */
                break;
            }
        }
    }
    return pos;
}

/* ── Server actor behavior ─────────────────────────────────────────── */

typedef struct {
    uint16_t port;
    int scenario;
    int request_count;
    http_conn_id_t last_conn_id;
    char last_method[16];
    char last_path[128];
    char last_body[256];
    size_t last_body_size;
    char last_headers[512];
    size_t last_headers_size;
} server_state_t;

enum {
    SCENARIO_GET_200,
    SCENARIO_POST_ECHO,
    SCENARIO_404,
    SCENARIO_HEADERS,
    SCENARIO_MULTIPLE
};

static bool server_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                            message_t *msg, void *state) {
    server_state_t *s = state;

    if (msg->type == 0) {
        actor_http_listen(rt, s->port);
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->request_count++;
        s->last_conn_id = p->conn_id;

        const char *method = http_request_method(p);
        const char *path = http_request_path(p);
        const char *body = http_request_body(p);

        snprintf(s->last_method, sizeof(s->last_method), "%s", method);
        snprintf(s->last_path, sizeof(s->last_path), "%s", path);
        if (p->body_size > 0 && p->body_size < sizeof(s->last_body)) {
            memcpy(s->last_body, body, p->body_size);
            s->last_body[p->body_size] = '\0';
            s->last_body_size = p->body_size;
        }
        if (p->headers_size > 0 && p->headers_size < sizeof(s->last_headers)) {
            memcpy(s->last_headers, http_request_headers(p), p->headers_size);
            s->last_headers_size = p->headers_size;
        }

        switch (s->scenario) {
        case SCENARIO_GET_200: {
            const char *resp_body = "hello";
            actor_http_respond(rt, p->conn_id, 200, NULL, 0,
                               resp_body, 5);
            runtime_stop(rt);
            return false;
        }
        case SCENARIO_POST_ECHO: {
            actor_http_respond(rt, p->conn_id, 200, NULL, 0,
                               body, p->body_size);
            runtime_stop(rt);
            return false;
        }
        case SCENARIO_404: {
            const char *resp_body = "not found";
            actor_http_respond(rt, p->conn_id, 404, NULL, 0,
                               resp_body, 9);
            runtime_stop(rt);
            return false;
        }
        case SCENARIO_HEADERS: {
            const char *resp_headers[] = {
                "X-Custom: hello-from-server",
                "X-Another: value"
            };
            actor_http_respond(rt, p->conn_id, 200, resp_headers, 2,
                               "ok", 2);
            runtime_stop(rt);
            return false;
        }
        case SCENARIO_MULTIPLE: {
            const char *resp_body = "response";
            actor_http_respond(rt, p->conn_id, 200, NULL, 0,
                               resp_body, 8);
            if (s->request_count >= 2) {
                runtime_stop(rt);
                return false;
            }
            return true;
        }
        }
        return true;
    }

    return true;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_server_get_200(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: client */
        int fd = connect_retry(TEST_PORT, 50);
        if (fd < 0) _exit(1);

        const char *req = "GET /hello HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        send(fd, req, strlen(req), 0);

        char resp[4096];
        size_t n = read_response(fd, resp, sizeof(resp));
        close(fd);

        if (n == 0) _exit(1);
        if (!strstr(resp, "200 OK")) _exit(2);
        if (!strstr(resp, "hello")) _exit(3);
        _exit(0);
    }

    /* Parent: server */
    runtime_t *rt = runtime_init(1, 16);
    server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT;
    state.scenario = SCENARIO_GET_200;

    actor_id_t aid = actor_spawn(rt, server_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.request_count, 1);
    ASSERT(strcmp(state.last_method, "GET") == 0);
    ASSERT(strcmp(state.last_path, "/hello") == 0);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_server_post_echo(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 1, 50);
        if (fd < 0) _exit(1);

        const char *req = "POST /echo HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: 10\r\n"
                          "\r\n"
                          "hello body";
        send(fd, req, strlen(req), 0);

        char resp[4096];
        size_t n = read_response(fd, resp, sizeof(resp));
        close(fd);

        if (n == 0) _exit(1);
        if (!strstr(resp, "200 OK")) _exit(2);
        if (!strstr(resp, "hello body")) _exit(3);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 1;
    state.scenario = SCENARIO_POST_ECHO;

    actor_id_t aid = actor_spawn(rt, server_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.request_count, 1);
    ASSERT(strcmp(state.last_method, "POST") == 0);
    ASSERT(strcmp(state.last_body, "hello body") == 0);
    ASSERT_EQ(state.last_body_size, 10);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_server_404(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 2, 50);
        if (fd < 0) _exit(1);

        const char *req = "GET /missing HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "\r\n";
        send(fd, req, strlen(req), 0);

        char resp[4096];
        size_t n = read_response(fd, resp, sizeof(resp));
        close(fd);

        if (n == 0) _exit(1);
        if (!strstr(resp, "404 Not Found")) _exit(2);
        if (!strstr(resp, "not found")) _exit(3);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 2;
    state.scenario = SCENARIO_404;

    actor_id_t aid = actor_spawn(rt, server_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.request_count, 1);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_server_headers(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = connect_retry(TEST_PORT + 3, 50);
        if (fd < 0) _exit(1);

        const char *req = "GET /headers HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "X-Client-Custom: test-val\r\n"
                          "\r\n";
        send(fd, req, strlen(req), 0);

        char resp[4096];
        size_t n = read_response(fd, resp, sizeof(resp));
        close(fd);

        if (n == 0) _exit(1);
        if (!strstr(resp, "200 OK")) _exit(2);
        if (!strstr(resp, "X-Custom: hello-from-server")) _exit(3);
        if (!strstr(resp, "X-Another: value")) _exit(4);
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 3;
    state.scenario = SCENARIO_HEADERS;

    actor_id_t aid = actor_spawn(rt, server_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.request_count, 1);
    /* Verify we received the client's custom header */
    ASSERT(state.last_headers_size > 0);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

static int test_server_multiple(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Two sequential clients */
        for (int i = 0; i < 2; i++) {
            int fd = connect_retry(TEST_PORT + 4, 50);
            if (fd < 0) _exit(1);

            const char *req = "GET /multi HTTP/1.1\r\n"
                              "Host: localhost\r\n"
                              "\r\n";
            send(fd, req, strlen(req), 0);

            char resp[4096];
            size_t n = read_response(fd, resp, sizeof(resp));
            close(fd);

            if (n == 0) _exit(2 + i);
            if (!strstr(resp, "200 OK")) _exit(4 + i);
            if (!strstr(resp, "response")) _exit(6 + i);
        }
        _exit(0);
    }

    runtime_t *rt = runtime_init(1, 16);
    server_state_t state;
    memset(&state, 0, sizeof(state));
    state.port = TEST_PORT + 4;
    state.scenario = SCENARIO_MULTIPLE;

    actor_id_t aid = actor_spawn(rt, server_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(state.request_count, 2);

    runtime_destroy(rt);

    int status;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_http_server:\n");
    RUN_TEST(test_server_get_200);
    RUN_TEST(test_server_post_echo);
    RUN_TEST(test_server_404);
    RUN_TEST(test_server_headers);
    RUN_TEST(test_server_multiple);
    TEST_REPORT();
}
