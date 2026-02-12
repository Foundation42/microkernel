#include "microkernel/scheduler.h"
#include "microkernel/actor.h"

void scheduler_init(scheduler_t *sched) {
    sched->ready_queue_head = NULL;
    sched->ready_queue_tail = NULL;
    sched->ready_count = 0;
}

void scheduler_enqueue(scheduler_t *sched, actor_t *actor) {
    /* Guard: don't enqueue if already READY (prevents duplicates) */
    if (actor->status == ACTOR_READY) return;

    actor->status = ACTOR_READY;
    actor->next = NULL;

    if (sched->ready_queue_tail) {
        sched->ready_queue_tail->next = actor;
    } else {
        sched->ready_queue_head = actor;
    }
    sched->ready_queue_tail = actor;
    sched->ready_count++;
}

actor_t *scheduler_dequeue(scheduler_t *sched) {
    actor_t *actor = sched->ready_queue_head;
    if (!actor) return NULL;

    sched->ready_queue_head = actor->next;
    if (!sched->ready_queue_head) {
        sched->ready_queue_tail = NULL;
    }
    actor->next = NULL;
    sched->ready_count--;
    return actor;
}

bool scheduler_is_empty(const scheduler_t *sched) {
    return sched->ready_queue_head == NULL;
}
