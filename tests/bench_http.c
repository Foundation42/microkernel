#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L
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
#include <time.h>
#include <stdio.h>

#define BENCH_PORT 19890
#define WARMUP_REQUESTS 50
#define MEASURED_REQUESTS 500
#define TOTAL_REQUESTS (WARMUP_REQUESTS + MEASURED_REQUESTS)

/* ── Local HTTP server ───────────────────────────────────────────── */

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
    listen(fd, 128);
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

static const char *HTTP_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "ok";

static pid_t start_bench_server(void) {
    pid_t pid = fork();
    if (pid != 0) { usleep(50000); return pid; }

    int lfd = listen_tcp(BENCH_PORT);

    for (int i = 0; i < TOTAL_REQUESTS; i++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        char req[1024];
        read_request(cfd, req, sizeof(req));
        send(cfd, HTTP_RESPONSE, strlen(HTTP_RESPONSE), 0);
        close(cfd);
    }

    close(lfd);
    _exit(0);
}

/* ── Benchmark actor ─────────────────────────────────────────────── */

typedef struct {
    int requests_done;
    int target;
    bool measuring;
    struct timespec start;
    struct timespec end;
    char url[128];
} bench_state_t;

static bool bench_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                           message_t *msg, void *state) {
    bench_state_t *s = state;

    if (msg->type == 0) {
        actor_http_get(rt, s->url);
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        actor_http_close(rt, p->conn_id);

        s->requests_done++;

        if (s->requests_done == WARMUP_REQUESTS && !s->measuring) {
            s->measuring = true;
            s->requests_done = 0;
            clock_gettime(CLOCK_MONOTONIC, &s->start);
        }

        if (s->measuring && s->requests_done >= MEASURED_REQUESTS) {
            clock_gettime(CLOCK_MONOTONIC, &s->end);
            runtime_stop(rt);
            return false;
        }

        actor_http_get(rt, s->url);
        return true;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        fprintf(stderr, "  HTTP error at request %d\n", s->requests_done);
        runtime_stop(rt);
        return false;
    }

    return true;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("bench_http:\n");

    pid_t server = start_bench_server();

    runtime_t *rt = runtime_init(1, 64);
    bench_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.url, sizeof(state.url),
             "http://127.0.0.1:%d/bench", BENCH_PORT);

    actor_id_t aid = actor_spawn(rt, bench_behavior, &state, NULL, 64);
    actor_send(rt, aid, 0, NULL, 0);
    runtime_run(rt);

    if (state.measuring) {
        double elapsed_ns = (double)(state.end.tv_sec - state.start.tv_sec) * 1e9
                          + (double)(state.end.tv_nsec - state.start.tv_nsec);
        double elapsed_ms = elapsed_ns / 1e6;
        double rps = (double)MEASURED_REQUESTS / (elapsed_ns / 1e9);
        double us_per_req = (elapsed_ns / 1000.0) / MEASURED_REQUESTS;

        printf("  %d requests (%d warmup)\n", MEASURED_REQUESTS, WARMUP_REQUESTS);
        printf("  Total time: %.2f ms\n", elapsed_ms);
        printf("  Throughput: %.0f req/s\n", rps);
        printf("  Avg latency: %.1f us/req\n", us_per_req);
    } else {
        printf("  FAIL: measurement phase not reached\n");
    }

    runtime_destroy(rt);
    waitpid(server, NULL, 0);

    printf("\nbench_http: done\n");
    return 0;
}
