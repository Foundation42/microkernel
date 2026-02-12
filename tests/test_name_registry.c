#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"

/* ── Behaviors ─────────────────────────────────────────────────────── */

static bool noop_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

static bool stop_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return false;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int test_register_and_lookup(void) {
    runtime_t *rt = runtime_init(0, 64);
    actor_id_t id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    ASSERT(actor_register_name(rt, "my_actor", id));
    ASSERT_EQ(actor_lookup(rt, "my_actor"), id);

    runtime_destroy(rt);
    return 0;
}

static int test_missing_returns_invalid(void) {
    runtime_t *rt = runtime_init(0, 64);

    ASSERT_EQ(actor_lookup(rt, "nonexistent"), ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

static int test_duplicate_name_fails(void) {
    runtime_t *rt = runtime_init(0, 64);
    actor_id_t id1 = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    actor_id_t id2 = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    ASSERT(actor_register_name(rt, "service", id1));
    ASSERT(!actor_register_name(rt, "service", id2)); /* duplicate */
    ASSERT_EQ(actor_lookup(rt, "service"), id1); /* original still there */

    runtime_destroy(rt);
    return 0;
}

static int test_deregister_on_actor_stop(void) {
    runtime_t *rt = runtime_init(0, 64);
    actor_id_t id = actor_spawn(rt, stop_behavior, NULL, NULL, 16);

    ASSERT(actor_register_name(rt, "ephemeral", id));
    ASSERT_EQ(actor_lookup(rt, "ephemeral"), id);

    /* Send a message to trigger stop */
    actor_send(rt, id, 0, NULL, 0);
    runtime_step(rt); /* processes message, actor stops, cleanup runs */

    ASSERT_EQ(actor_lookup(rt, "ephemeral"), ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

int main(void) {
    printf("test_name_registry:\n");
    RUN_TEST(test_register_and_lookup);
    RUN_TEST(test_missing_returns_invalid);
    RUN_TEST(test_duplicate_name_fails);
    RUN_TEST(test_deregister_on_actor_stop);
    TEST_REPORT();
}
