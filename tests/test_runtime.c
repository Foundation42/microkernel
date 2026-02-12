#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"

/* ── Behaviors ──────────────────────────────────────────────────────── */

static bool echo_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self; (void)state;
    actor_send(rt, msg->source, msg->type, msg->payload, msg->payload_size);
    return true;
}

static bool stop_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return false;
}

static bool counter_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg;
    int *count = (int *)state;
    (*count)++;
    return true;
}

/* For test_actor_self_and_state */
static actor_id_t captured_self;
static void *captured_state;

static bool capture_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self; (void)msg; (void)state;
    captured_self = actor_self(rt);
    captured_state = actor_state(rt);
    return true;
}

/* For test_free_state_called */
static int freed_flag = 0;

static void my_free(void *p) {
    free(p);
    freed_flag = 1;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int test_init_destroy(void) {
    runtime_t *rt = runtime_init(1, 64);
    ASSERT_NOT_NULL(rt);
    runtime_destroy(rt);
    return 0;
}

static int test_spawn(void) {
    runtime_t *rt = runtime_init(1, 64);
    actor_id_t id = actor_spawn(rt, echo_behavior, NULL, NULL, 16);
    ASSERT_NE(id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_id_node(id), (node_id_t)1);
    ASSERT_EQ(actor_id_seq(id), (uint32_t)1);
    runtime_destroy(rt);
    return 0;
}

static int test_send_and_step(void) {
    runtime_t *rt = runtime_init(0, 64);
    int count = 0;
    actor_id_t id = actor_spawn(rt, counter_behavior, &count, NULL, 16);

    ASSERT(actor_send(rt, id, 1, NULL, 0));
    ASSERT(actor_send(rt, id, 1, NULL, 0));

    runtime_step(rt);
    ASSERT_EQ(count, 1); /* one message per step */
    runtime_step(rt);
    ASSERT_EQ(count, 2);

    runtime_destroy(rt);
    return 0;
}

static int test_behavior_returns_false_stops(void) {
    runtime_t *rt = runtime_init(0, 64);
    actor_id_t id = actor_spawn(rt, stop_behavior, NULL, NULL, 16);

    actor_send(rt, id, 0, NULL, 0);
    runtime_step(rt);

    ASSERT(!actor_send(rt, id, 0, NULL, 0));

    runtime_destroy(rt);
    return 0;
}

static int test_actor_self_and_state(void) {
    int my_state = 42;
    runtime_t *rt = runtime_init(0, 64);
    actor_id_t id = actor_spawn(rt, capture_behavior, &my_state, NULL, 16);

    actor_send(rt, id, 0, NULL, 0);
    runtime_step(rt);

    ASSERT_EQ(captured_self, id);
    ASSERT_EQ(captured_state, &my_state);

    runtime_destroy(rt);
    return 0;
}

static int test_send_to_invalid(void) {
    runtime_t *rt = runtime_init(0, 64);
    ASSERT(!actor_send(rt, ACTOR_ID_INVALID, 0, NULL, 0));
    ASSERT(!actor_send(rt, actor_id_make(0, 999), 0, NULL, 0));
    runtime_destroy(rt);
    return 0;
}

static int test_clean_shutdown_with_live_actors(void) {
    runtime_t *rt = runtime_init(0, 64);
    actor_spawn(rt, echo_behavior, NULL, NULL, 16);
    actor_spawn(rt, echo_behavior, NULL, NULL, 16);
    runtime_destroy(rt);
    return 0;
}

static int test_runtime_run(void) {
    runtime_t *rt = runtime_init(0, 64);
    int count = 0;
    actor_id_t id = actor_spawn(rt, counter_behavior, &count, NULL, 16);
    actor_send(rt, id, 0, NULL, 0);
    actor_send(rt, id, 0, NULL, 0);
    actor_send(rt, id, 0, NULL, 0);
    runtime_run(rt);
    ASSERT_EQ(count, 3);
    runtime_destroy(rt);
    return 0;
}

static int test_free_state_called(void) {
    freed_flag = 0;
    runtime_t *rt = runtime_init(0, 64);
    int *state = malloc(sizeof(int));
    *state = 0;
    actor_id_t id = actor_spawn(rt, stop_behavior, state, my_free, 16);
    actor_send(rt, id, 0, NULL, 0);
    runtime_step(rt);
    ASSERT_EQ(freed_flag, 1);
    runtime_destroy(rt);
    return 0;
}

int main(void) {
    printf("test_runtime:\n");
    RUN_TEST(test_init_destroy);
    RUN_TEST(test_spawn);
    RUN_TEST(test_send_and_step);
    RUN_TEST(test_behavior_returns_false_stops);
    RUN_TEST(test_actor_self_and_state);
    RUN_TEST(test_send_to_invalid);
    RUN_TEST(test_clean_shutdown_with_live_actors);
    RUN_TEST(test_runtime_run);
    RUN_TEST(test_free_state_called);
    TEST_REPORT();
}
