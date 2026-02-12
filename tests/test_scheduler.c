#include "test_framework.h"
#include "microkernel/scheduler.h"
#include "microkernel/actor.h"

/* Dummy behavior for creating test actors */
static bool dummy_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

static int test_init_empty(void) {
    scheduler_t sched;
    scheduler_init(&sched);
    ASSERT(scheduler_is_empty(&sched));
    ASSERT_NULL(scheduler_dequeue(&sched));
    return 0;
}

static int test_fifo_order(void) {
    scheduler_t sched;
    scheduler_init(&sched);

    actor_t *a1 = actor_create(1, 0, dummy_behavior, NULL, NULL, 4);
    actor_t *a2 = actor_create(2, 0, dummy_behavior, NULL, NULL, 4);
    actor_t *a3 = actor_create(3, 0, dummy_behavior, NULL, NULL, 4);

    scheduler_enqueue(&sched, a1);
    scheduler_enqueue(&sched, a2);
    scheduler_enqueue(&sched, a3);
    ASSERT_EQ(sched.ready_count, (size_t)3);

    ASSERT_EQ(scheduler_dequeue(&sched), a1);
    ASSERT_EQ(scheduler_dequeue(&sched), a2);
    ASSERT_EQ(scheduler_dequeue(&sched), a3);
    ASSERT_NULL(scheduler_dequeue(&sched));

    actor_destroy(a1);
    actor_destroy(a2);
    actor_destroy(a3);
    return 0;
}

static int test_double_enqueue_prevention(void) {
    scheduler_t sched;
    scheduler_init(&sched);

    actor_t *a = actor_create(1, 0, dummy_behavior, NULL, NULL, 4);
    scheduler_enqueue(&sched, a);
    scheduler_enqueue(&sched, a); /* should be ignored */
    ASSERT_EQ(sched.ready_count, (size_t)1);

    ASSERT_EQ(scheduler_dequeue(&sched), a);
    ASSERT_NULL(scheduler_dequeue(&sched));

    actor_destroy(a);
    return 0;
}

int main(void) {
    printf("test_scheduler:\n");
    RUN_TEST(test_init_empty);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_double_enqueue_prevention);
    TEST_REPORT();
}
