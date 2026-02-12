#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"

/* ── Test state ────────────────────────────────────────────────────── */

typedef struct {
    int       fire_count;
    timer_id_t last_timer_id;
    timer_id_t periodic_id;
    int        cancel_after;
} timer_test_state_t;

/* ── Behaviors ─────────────────────────────────────────────────────── */

static bool oneshot_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    timer_test_state_t *s = (timer_test_state_t *)state;

    if (msg->type == 1) {
        /* Setup message: arm a one-shot timer */
        s->last_timer_id = actor_set_timer(rt, 10, false);
        return true;
    }
    if (msg->type == MSG_TIMER) {
        const timer_payload_t *tp = (const timer_payload_t *)msg->payload;
        s->last_timer_id = tp->id;
        s->fire_count++;
        runtime_stop(rt);
        return true;
    }
    return true;
}

static bool periodic_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    timer_test_state_t *s = (timer_test_state_t *)state;

    if (msg->type == 1) {
        /* Setup: arm a periodic timer */
        s->periodic_id = actor_set_timer(rt, 10, true);
        return true;
    }
    if (msg->type == MSG_TIMER) {
        s->fire_count++;
        if (s->fire_count >= s->cancel_after) {
            actor_cancel_timer(rt, s->periodic_id);
            runtime_stop(rt);
        }
        return true;
    }
    return true;
}

static bool cancel_before_fire_behavior(runtime_t *rt, actor_t *self,
                                        message_t *msg, void *state) {
    (void)self;
    timer_test_state_t *s = (timer_test_state_t *)state;

    if (msg->type == 1) {
        timer_id_t tid = actor_set_timer(rt, 1000, false);
        actor_cancel_timer(rt, tid);
        /* Arm a short timer to prove the cancelled one doesn't fire */
        actor_set_timer(rt, 20, false);
        return true;
    }
    if (msg->type == MSG_TIMER) {
        s->fire_count++;
        runtime_stop(rt);
        return true;
    }
    return true;
}

static bool timer_stop_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    timer_test_state_t *s = (timer_test_state_t *)state;

    if (msg->type == 1) {
        actor_set_timer(rt, 1000, true); /* won't fire, cleaned on stop */
        s->fire_count = 0;
        return false; /* stop the actor */
    }
    return true;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int test_oneshot_timer(void) {
    runtime_t *rt = runtime_init(0, 64);
    timer_test_state_t s = {0};
    actor_id_t id = actor_spawn(rt, oneshot_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);
    ASSERT_EQ(s.fire_count, 1);
    ASSERT_NE(s.last_timer_id, TIMER_ID_INVALID);
    runtime_destroy(rt);
    return 0;
}

static int test_periodic_timer(void) {
    runtime_t *rt = runtime_init(0, 64);
    timer_test_state_t s = {.cancel_after = 3};
    actor_id_t id = actor_spawn(rt, periodic_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);
    ASSERT(s.fire_count >= 3);
    runtime_destroy(rt);
    return 0;
}

static int test_cancel_before_fire(void) {
    runtime_t *rt = runtime_init(0, 64);
    timer_test_state_t s = {0};
    actor_id_t id = actor_spawn(rt, cancel_before_fire_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);
    /* Only the short 20ms timer should have fired, not the cancelled 1000ms */
    ASSERT_EQ(s.fire_count, 1);
    runtime_destroy(rt);
    return 0;
}

static int test_cleanup_on_actor_stop(void) {
    runtime_t *rt = runtime_init(0, 64);
    timer_test_state_t s = {0};
    actor_id_t id = actor_spawn(rt, timer_stop_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);
    /* Actor stopped → timer should be cleaned up (no leak, no fire) */
    ASSERT_EQ(s.fire_count, 0);
    runtime_destroy(rt);
    return 0;
}

int main(void) {
    printf("test_timer:\n");
    RUN_TEST(test_oneshot_timer);
    RUN_TEST(test_periodic_timer);
    RUN_TEST(test_cancel_before_fire);
    RUN_TEST(test_cleanup_on_actor_stop);
    TEST_REPORT();
}
