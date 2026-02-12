#define _POSIX_C_SOURCE 199309L
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <time.h>

#define WARMUP_ROUNDS 100
#define ROUNDS 1000
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

static bool ping_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    ping_state_t *s = (ping_state_t *)state;
    if (msg->type == MSG_PONG) {
        s->count++;
        if (s->count < s->limit) {
            actor_send(rt, s->peer, MSG_PING, NULL, 0);
        }
    }
    return true;
}

static bool pong_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    pong_state_t *s = (pong_state_t *)state;
    if (msg->type == MSG_PING) {
        s->count++;
        actor_send(rt, s->peer, MSG_PONG, NULL, 0);
    }
    return true;
}

static int test_pingpong(void) {
    runtime_t *rt = runtime_init(0, 64);

    ping_state_t ping_state = {0, 0, WARMUP_ROUNDS};
    pong_state_t pong_state = {0, 0};

    actor_id_t ping_id = actor_spawn(rt, ping_behavior, &ping_state, NULL, 16);
    actor_id_t pong_id = actor_spawn(rt, pong_behavior, &pong_state, NULL, 16);
    ASSERT_NE(ping_id, ACTOR_ID_INVALID);
    ASSERT_NE(pong_id, ACTOR_ID_INVALID);

    ping_state.peer = pong_id;
    pong_state.peer = ping_id;

    /* Warmup: prime caches and branch predictors */
    actor_send(rt, pong_id, MSG_PING, NULL, 0);
    runtime_run(rt);
    ASSERT_EQ(ping_state.count, WARMUP_ROUNDS);
    ASSERT_EQ(pong_state.count, WARMUP_ROUNDS);

    /* Reset for measured run */
    ping_state.count = 0;
    ping_state.limit = ROUNDS;
    pong_state.count = 0;

    actor_send(rt, pong_id, MSG_PING, NULL, 0);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    runtime_run(rt);

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ns = (double)(end.tv_sec - start.tv_sec) * 1e9
                      + (double)(end.tv_nsec - start.tv_nsec);
    int total_messages = ping_state.count + pong_state.count;
    double ns_per_msg = elapsed_ns / total_messages;

    printf("\n  Ping-pong: %d rounds (%d messages), %d warmup rounds\n",
           ROUNDS, total_messages, WARMUP_ROUNDS);
    printf("  Total time: %.2f us\n", elapsed_ns / 1000.0);
    printf("  Per message: %.0f ns (%.2f us)\n", ns_per_msg, ns_per_msg / 1000.0);

    ASSERT_EQ(ping_state.count, ROUNDS);
    ASSERT_EQ(pong_state.count, ROUNDS);

    runtime_destroy(rt);
    return 0;
}

int main(void) {
    printf("test_pingpong:\n");
    RUN_TEST(test_pingpong);
    TEST_REPORT();
}
