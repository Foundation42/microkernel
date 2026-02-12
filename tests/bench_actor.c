#define _POSIX_C_SOURCE 199309L
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <time.h>
#include <stdio.h>

#define MSG_PING 1
#define MSG_PONG 2

typedef struct {
    actor_id_t peer;
    int        count;
    int        limit;
} ping_state_t;

typedef struct {
    actor_id_t peer;
    int        count;
} pong_state_t;

static bool ping_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                          message_t *msg, void *state) {
    ping_state_t *s = state;
    if (msg->type == MSG_PONG) {
        s->count++;
        if (s->count < s->limit) {
            actor_send(rt, s->peer, MSG_PING, NULL, 0);
        }
    }
    return true;
}

static bool pong_behavior(runtime_t *rt, actor_t *self __attribute__((unused)),
                          message_t *msg, void *state) {
    pong_state_t *s = state;
    if (msg->type == MSG_PING) {
        s->count++;
        actor_send(rt, s->peer, MSG_PONG, NULL, 0);
    }
    return true;
}

static void run_round(const char *label, int warmup, int rounds) {
    runtime_t *rt = runtime_init(0, 1024);

    ping_state_t ping_state = {0, 0, warmup};
    pong_state_t pong_state = {0, 0};

    actor_id_t ping_id = actor_spawn(rt, ping_behavior, &ping_state, NULL, 64);
    actor_id_t pong_id = actor_spawn(rt, pong_behavior, &pong_state, NULL, 64);

    ping_state.peer = pong_id;
    pong_state.peer = ping_id;

    /* Warmup */
    actor_send(rt, pong_id, MSG_PING, NULL, 0);
    runtime_run(rt);

    /* Reset for measured run */
    ping_state.count = 0;
    ping_state.limit = rounds;
    pong_state.count = 0;

    actor_send(rt, pong_id, MSG_PING, NULL, 0);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    runtime_run(rt);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9
                      + (double)(end.tv_nsec - start.tv_nsec);
    int total_messages = ping_state.count + pong_state.count;
    double msgs_per_sec = (double)total_messages / (elapsed_ns / 1e9);
    double ns_per_msg = elapsed_ns / total_messages;

    printf("  %-12s %d msgs, %.2f ms, %.0f msg/s, %.0f ns/msg\n",
           label, total_messages, elapsed_ns / 1e6, msgs_per_sec, ns_per_msg);

    runtime_destroy(rt);
}

int main(void) {
    printf("bench_actor:\n");

    run_round("10K:",  1000,   10000);
    run_round("100K:", 1000,  100000);
    run_round("1M:",   1000, 1000000);

    printf("\nbench_actor: done\n");
    return 0;
}
