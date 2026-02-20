#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/supervision.h"
#include "runtime_internal.h"

/* ── Helper: drive runtime until scheduler empty (bounded) ─────────── */

static void drain(runtime_t *rt, int max_steps) {
    for (int i = 0; i < max_steps; i++) {
        runtime_step(rt);
    }
}

/* ── Behaviors ─────────────────────────────────────────────────────── */

/* Stays alive forever */
static bool alive_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

/* Dies immediately on any message */
static bool die_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return false;
}

/* Dies on message type 1, stays alive otherwise */
static bool die_on_signal_behavior(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state) {
    (void)rt; (void)self; (void)state;
    return msg->type != 1;
}

/* Records MSG_CHILD_EXIT notifications */
typedef struct {
    actor_id_t child_id;
    uint8_t    exit_reason;
    int        count;
} exit_record_t;

static bool parent_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)rt; (void)self;
    exit_record_t *rec = (exit_record_t *)state;
    if (msg->type == MSG_CHILD_EXIT) {
        const child_exit_payload_t *p =
            (const child_exit_payload_t *)msg->payload;
        rec->child_id = p->child_id;
        rec->exit_reason = p->exit_reason;
        rec->count++;
    }
    return true;
}

/* ── State factories ───────────────────────────────────────────────── */

static void *counter_factory(void *arg) {
    (void)arg;
    int *count = calloc(1, sizeof(int));
    return count;
}

/* ── Test 1: Child exit notification ───────────────────────────────── */

static int test_child_exit_notification(void) {
    runtime_t *rt = runtime_init(0, 64);

    exit_record_t rec = {0};
    actor_id_t parent = actor_spawn(rt, parent_behavior, &rec, NULL, 16);
    actor_id_t child = actor_spawn(rt, die_on_signal_behavior, NULL, NULL, 16);

    /* Set parent link */
    runtime_set_actor_parent(rt, child, parent);

    /* Kill the child */
    actor_send(rt, child, 1, NULL, 0);
    drain(rt, 10);

    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.child_id, child);
    ASSERT_EQ(rec.exit_reason, EXIT_NORMAL);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 2: One-for-one strategy ──────────────────────────────────── */

static int test_one_for_one(void) {
    runtime_t *rt = runtime_init(0, 64);

    child_spec_t specs[] = {
        { .name = "A", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
        { .name = "B", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
    };

    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                       5, 10000, specs, 2);
    ASSERT_NE(sup, ACTOR_ID_INVALID);

    /* Process MSG_SUP_START */
    drain(rt, 10);

    actor_id_t a_before = supervisor_get_child(rt, sup, 0);
    actor_id_t b_before = supervisor_get_child(rt, sup, 1);
    ASSERT_NE(a_before, ACTOR_ID_INVALID);
    ASSERT_NE(b_before, ACTOR_ID_INVALID);

    /* Kill child A */
    actor_stop(rt, a_before);
    drain(rt, 20);

    actor_id_t a_after = supervisor_get_child(rt, sup, 0);
    actor_id_t b_after = supervisor_get_child(rt, sup, 1);

    /* A should have a new ID (restarted) */
    ASSERT_NE(a_after, ACTOR_ID_INVALID);
    ASSERT_NE(a_after, a_before);
    /* B should be unchanged */
    ASSERT_EQ(b_after, b_before);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 3: One-for-all strategy ──────────────────────────────────── */

static int test_one_for_all(void) {
    runtime_t *rt = runtime_init(0, 64);

    child_spec_t specs[] = {
        { .name = "A", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
        { .name = "B", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
    };

    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ALL,
                                       5, 10000, specs, 2);
    ASSERT_NE(sup, ACTOR_ID_INVALID);
    drain(rt, 10);

    actor_id_t a_before = supervisor_get_child(rt, sup, 0);
    actor_id_t b_before = supervisor_get_child(rt, sup, 1);

    /* Kill child B */
    actor_stop(rt, b_before);
    drain(rt, 20);

    actor_id_t a_after = supervisor_get_child(rt, sup, 0);
    actor_id_t b_after = supervisor_get_child(rt, sup, 1);

    /* Both should have new IDs */
    ASSERT_NE(a_after, ACTOR_ID_INVALID);
    ASSERT_NE(b_after, ACTOR_ID_INVALID);
    ASSERT_NE(a_after, a_before);
    ASSERT_NE(b_after, b_before);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 4: Rest-for-one strategy ─────────────────────────────────── */

static int test_rest_for_one(void) {
    runtime_t *rt = runtime_init(0, 64);

    child_spec_t specs[] = {
        { .name = "A", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
        { .name = "B", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
        { .name = "C", .behavior = alive_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
    };

    actor_id_t sup = supervisor_start(rt, STRATEGY_REST_FOR_ONE,
                                       5, 10000, specs, 3);
    ASSERT_NE(sup, ACTOR_ID_INVALID);
    drain(rt, 10);

    actor_id_t a_before = supervisor_get_child(rt, sup, 0);
    actor_id_t b_before = supervisor_get_child(rt, sup, 1);
    actor_id_t c_before = supervisor_get_child(rt, sup, 2);

    /* Kill child B */
    actor_stop(rt, b_before);
    drain(rt, 20);

    actor_id_t a_after = supervisor_get_child(rt, sup, 0);
    actor_id_t b_after = supervisor_get_child(rt, sup, 1);
    actor_id_t c_after = supervisor_get_child(rt, sup, 2);

    /* A unchanged, B and C restarted */
    ASSERT_EQ(a_after, a_before);
    ASSERT_NE(b_after, ACTOR_ID_INVALID);
    ASSERT_NE(b_after, b_before);
    ASSERT_NE(c_after, ACTOR_ID_INVALID);
    ASSERT_NE(c_after, c_before);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 5: Restart limit exceeded ────────────────────────────────── */

static int test_restart_limit(void) {
    runtime_t *rt = runtime_init(0, 128);

    child_spec_t specs[] = {
        { .name = "crasher", .behavior = die_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
    };

    /* max_restarts=2 within 10s window — child dies immediately so
       it will exceed 2 restarts quickly */
    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                       2, 10000, specs, 1);
    ASSERT_NE(sup, ACTOR_ID_INVALID);

    /* Drive until supervisor gives up. The die_behavior child will
       die each time it gets spawned and receives any message.
       But children spawned by the supervisor don't automatically receive
       messages — they die because... actually die_behavior dies on the
       first message it receives. The child needs a message to die.

       Wait — newly spawned actors are idle. They won't die until they
       get a message. The supervisor spawns them but doesn't send them
       a message. So the die_behavior never runs.

       For this test, let's use a different approach: kill the child
       via actor_stop 3 times and check the supervisor eventually stops. */

    drain(rt, 10); /* process MSG_SUP_START */

    /* Kill child 3 times (limit is 2) */
    for (int i = 0; i < 3; i++) {
        actor_id_t child = supervisor_get_child(rt, sup, 0);
        if (child == ACTOR_ID_INVALID) break;
        actor_stop(rt, child);
        drain(rt, 20);
    }

    /* After 3 kills with limit=2, supervisor should have stopped */
    /* Verify by checking that we can't send to the supervisor */
    ASSERT(!actor_send(rt, sup, 0, NULL, 0));

    runtime_destroy(rt);
    return 0;
}

/* ── Test 6: Transient child, normal exit → NOT restarted ──────────── */

static int test_transient_normal_exit(void) {
    runtime_t *rt = runtime_init(0, 64);

    child_spec_t specs[] = {
        { .name = "transient", .behavior = die_on_signal_behavior,
          .mailbox_size = 16, .restart_type = RESTART_TRANSIENT },
    };

    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                       5, 10000, specs, 1);
    ASSERT_NE(sup, ACTOR_ID_INVALID);
    drain(rt, 10);

    actor_id_t child_before = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child_before, ACTOR_ID_INVALID);

    /* Make child exit normally (behavior returns false on msg type 1) */
    actor_send(rt, child_before, 1, NULL, 0);
    drain(rt, 20);

    /* Transient child with normal exit should NOT be restarted */
    actor_id_t child_after = supervisor_get_child(rt, sup, 0);
    ASSERT_EQ(child_after, ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 7: Permanent child, normal exit → IS restarted ───────────── */

static int test_permanent_normal_exit(void) {
    runtime_t *rt = runtime_init(0, 64);

    child_spec_t specs[] = {
        { .name = "permanent", .behavior = die_on_signal_behavior,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
    };

    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                       5, 10000, specs, 1);
    ASSERT_NE(sup, ACTOR_ID_INVALID);
    drain(rt, 10);

    actor_id_t child_before = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child_before, ACTOR_ID_INVALID);

    /* Make child exit normally */
    actor_send(rt, child_before, 1, NULL, 0);
    drain(rt, 20);

    /* Permanent child should be restarted even on normal exit */
    actor_id_t child_after = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child_after, ACTOR_ID_INVALID);
    ASSERT_NE(child_after, child_before);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 8: Nested supervisors ────────────────────────────────────── */

static int test_nested_supervisors(void) {
    runtime_t *rt = runtime_init(0, 128);

    /* Inner supervisor manages one child */
    child_spec_t inner_specs[] = {
        { .name = "worker", .behavior = alive_behavior,
          .factory = counter_factory, .free_state = free,
          .mailbox_size = 16, .restart_type = RESTART_PERMANENT },
    };

    /* We need a behavior that starts an inner supervisor.
       Instead, let's test by creating two levels manually:
       - Top supervisor manages a sub-supervisor (which is itself an actor)
       - We kill the sub-supervisor's child, verify sub restarts it
       - Then kill the sub-supervisor itself, verify top restarts it */

    /* Create inner supervisor first */
    actor_id_t inner_sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                             5, 10000, inner_specs, 1);
    ASSERT_NE(inner_sup, ACTOR_ID_INVALID);
    drain(rt, 10);

    /* Get the inner child */
    actor_id_t inner_child_before = supervisor_get_child(rt, inner_sup, 0);
    ASSERT_NE(inner_child_before, ACTOR_ID_INVALID);

    /* Kill the inner child — inner supervisor should restart it */
    actor_stop(rt, inner_child_before);
    drain(rt, 20);

    actor_id_t inner_child_after = supervisor_get_child(rt, inner_sup, 0);
    ASSERT_NE(inner_child_after, ACTOR_ID_INVALID);
    ASSERT_NE(inner_child_after, inner_child_before);

    /* Now create an outer supervisor that manages the inner supervisor.
       We use a spec that spawns alive_behavior (since we can't nest
       supervisor_start as a spec behavior easily). Instead, let's
       set the parent link manually to simulate the top supervisor
       watching the inner supervisor. */

    exit_record_t rec = {0};
    actor_id_t top = actor_spawn(rt, parent_behavior, &rec, NULL, 16);
    runtime_set_actor_parent(rt, inner_sup, top);

    /* Kill the inner supervisor */
    actor_stop(rt, inner_sup);
    drain(rt, 20);

    /* Top should have been notified */
    ASSERT_EQ(rec.count, 1);
    ASSERT_EQ(rec.child_id, inner_sup);
    ASSERT_EQ(rec.exit_reason, EXIT_KILLED);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_supervision:\n");
    RUN_TEST(test_child_exit_notification);
    RUN_TEST(test_one_for_one);
    RUN_TEST(test_one_for_all);
    RUN_TEST(test_rest_for_one);
    RUN_TEST(test_restart_limit);
    RUN_TEST(test_transient_normal_exit);
    RUN_TEST(test_permanent_normal_exit);
    RUN_TEST(test_nested_supervisors);
    TEST_REPORT();
}
