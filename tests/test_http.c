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
#include <errno.h>

#define TEST_PORT 19880

/* ── Test HTTP server ──────────────────────────────────────────────── */

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

typedef enum {
    SCENARIO_200_CONTENT_LENGTH,
    SCENARIO_200_CHUNKED,
    SCENARIO_POST_ECHO,
    SCENARIO_404,
    SCENARIO_MULTI_HEADER
} http_scenario_t;

static pid_t start_http_server(http_scenario_t scenario) {
    pid_t pid = fork();
    if (pid != 0) { usleep(50000); return pid; }

    int lfd = listen_tcp(TEST_PORT);
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) _exit(1);

    char req[4096];
    read_request(cfd, req, sizeof(req));

    switch (scenario) {
    case SCENARIO_200_CONTENT_LENGTH: {
        const char *resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 5\r\n"
                           "Content-Type: text/plain\r\n"
                           "\r\n"
                           "hello";
        send(cfd, resp, strlen(resp), 0);
        break;
    }
    case SCENARIO_200_CHUNKED: {
        const char *resp = "HTTP/1.1 200 OK\r\n"
                           "Transfer-Encoding: chunked\r\n"
                           "\r\n"
                           "5\r\nhello\r\n"
                           "6\r\n world\r\n"
                           "0\r\n\r\n";
        send(cfd, resp, strlen(resp), 0);
        break;
    }
    case SCENARIO_POST_ECHO: {
        /* Find body after \r\n\r\n */
        char *body_start = strstr(req, "\r\n\r\n");
        if (body_start) body_start += 4;
        size_t body_len = body_start ? strlen(body_start) : 0;

        char resp[512];
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Length: %zu\r\n"
                         "\r\n", body_len);
        send(cfd, resp, (size_t)n, 0);
        if (body_len > 0) send(cfd, body_start, body_len, 0);
        break;
    }
    case SCENARIO_404: {
        const char *resp = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Length: 9\r\n"
                           "\r\n"
                           "not found";
        send(cfd, resp, strlen(resp), 0);
        break;
    }
    case SCENARIO_MULTI_HEADER: {
        const char *resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Length: 2\r\n"
                           "X-Custom: foo\r\n"
                           "X-Another: bar\r\n"
                           "\r\n"
                           "ok";
        send(cfd, resp, strlen(resp), 0);
        break;
    }
    }

    close(cfd);
    close(lfd);
    _exit(0);
}

/* ── Actor test infrastructure ─────────────────────────────────────── */

typedef struct {
    int status_code;
    char body[256];
    size_t body_size;
    char headers[512];
    size_t headers_size;
    bool got_response;
    bool got_error;
    char url[128];
    const char *method;
    const char *post_body;
    size_t post_body_size;
} http_test_state_t;

static bool http_test_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                               message_t *msg, void *state) {
    http_test_state_t *s = state;

    if (msg->type == 0) {
        /* Init message: start the HTTP request */
        if (s->method && strcmp(s->method, "POST") == 0) {
            actor_http_fetch(rt, "POST", s->url, NULL, 0,
                             s->post_body, s->post_body_size);
        } else {
            actor_http_get(rt, s->url);
        }
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->status_code = p->status_code;
        s->headers_size = p->headers_size < sizeof(s->headers) ?
                          p->headers_size : sizeof(s->headers) - 1;
        memcpy(s->headers, http_response_headers(p), s->headers_size);

        s->body_size = p->body_size < sizeof(s->body) ?
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

/* Search packed null-separated headers for a substring */
static bool find_in_headers(const char *headers, size_t size, const char *needle) {
    size_t pos = 0;
    while (pos < size) {
        const char *entry = headers + pos;
        if (strstr(entry, needle)) return true;
        size_t len = strlen(entry);
        pos += len + 1;
    }
    return false;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_http_get_200(void) {
    pid_t server = start_http_server(SCENARIO_200_CONTENT_LENGTH);

    runtime_t *rt = runtime_init(1, 16);
    http_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/test", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, http_test_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0); /* trigger init */
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT_EQ(state.body_size, 5);
    ASSERT(memcmp(state.body, "hello", 5) == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_http_chunked(void) {
    pid_t server = start_http_server(SCENARIO_200_CHUNKED);

    runtime_t *rt = runtime_init(1, 16);
    http_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/chunked", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, http_test_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT_EQ(state.body_size, 11);
    ASSERT(memcmp(state.body, "hello world", 11) == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_http_post(void) {
    pid_t server = start_http_server(SCENARIO_POST_ECHO);

    runtime_t *rt = runtime_init(1, 16);
    http_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/echo", TEST_PORT);
    state.method = "POST";
    state.post_body = "test data";
    state.post_body_size = 9;

    actor_id_t aid = actor_spawn(rt, http_test_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT_EQ(state.body_size, 9);
    ASSERT(memcmp(state.body, "test data", 9) == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_http_404(void) {
    pid_t server = start_http_server(SCENARIO_404);

    runtime_t *rt = runtime_init(1, 16);
    http_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/missing", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, http_test_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 404);
    ASSERT_EQ(state.body_size, 9);
    ASSERT(memcmp(state.body, "not found", 9) == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_http_headers(void) {
    pid_t server = start_http_server(SCENARIO_MULTI_HEADER);

    runtime_t *rt = runtime_init(1, 16);
    http_test_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/headers", TEST_PORT);

    actor_id_t aid = actor_spawn(rt, http_test_behavior, &state, NULL, 16);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    ASSERT(state.got_response);
    ASSERT_EQ(state.status_code, 200);
    ASSERT(state.headers_size > 0);
    /* Headers are packed as "Key: Value\0Key: Value\0..." */
    ASSERT(find_in_headers(state.headers, state.headers_size, "X-Custom: foo"));

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_http:\n");
    RUN_TEST(test_http_get_200);
    RUN_TEST(test_http_chunked);
    RUN_TEST(test_http_post);
    RUN_TEST(test_http_404);
    RUN_TEST(test_http_headers);
    TEST_REPORT();
}
