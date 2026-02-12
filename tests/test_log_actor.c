#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"

/* ── Test state ────────────────────────────────────────────────────── */

typedef struct {
    int log_messages_received;
} log_test_state_t;

/* ── Behaviors ─────────────────────────────────────────────────────── */

static bool logging_actor_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    log_test_state_t *s = (log_test_state_t *)state;

    if (msg->type == 1) {
        /* Log some messages */
        actor_log(rt, LOG_INFO, "hello from actor %lu",
                  (unsigned long)actor_self(rt));
        actor_log(rt, LOG_WARN, "warning message");
        s->log_messages_received = 1; /* we sent logs */
        runtime_stop(rt);
        return true;
    }
    return true;
}

static bool level_filter_behavior(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state) {
    (void)self;
    log_test_state_t *s = (log_test_state_t *)state;

    if (msg->type == 1) {
        /* This should be filtered (below WARN) */
        actor_log(rt, LOG_DEBUG, "should be filtered");
        actor_log(rt, LOG_INFO, "also filtered");
        /* This should pass */
        actor_log(rt, LOG_WARN, "should pass");
        actor_log(rt, LOG_ERROR, "should also pass");
        s->log_messages_received = 1;
        runtime_stop(rt);
        return true;
    }
    return true;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int test_basic_log(void) {
    runtime_t *rt = runtime_init(0, 64);
    runtime_enable_logging(rt);

    log_test_state_t s = {0};
    actor_id_t id = actor_spawn(rt, logging_actor_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(s.log_messages_received, 1);

    runtime_destroy(rt);
    return 0;
}

static int test_level_filter(void) {
    runtime_t *rt = runtime_init(0, 64);
    runtime_enable_logging(rt);
    runtime_set_log_level(rt, LOG_WARN);

    log_test_state_t s = {0};
    actor_id_t id = actor_spawn(rt, level_filter_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(s.log_messages_received, 1);

    runtime_destroy(rt);
    return 0;
}

static int test_noop_before_enable(void) {
    runtime_t *rt = runtime_init(0, 64);
    /* Do NOT call runtime_enable_logging */

    /* actor_log should be a no-op when logging not enabled */
    actor_log(rt, LOG_INFO, "this should be ignored %d", 42);

    /* No crash, no messages sent */
    runtime_destroy(rt);
    return 0;
}

int main(void) {
    printf("test_log_actor:\n");
    RUN_TEST(test_basic_log);
    RUN_TEST(test_level_filter);
    RUN_TEST(test_noop_before_enable);
    TEST_REPORT();
}
