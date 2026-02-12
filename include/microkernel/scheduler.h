#ifndef MICROKERNEL_SCHEDULER_H
#define MICROKERNEL_SCHEDULER_H

#include "types.h"

struct scheduler {
    actor_t *ready_queue_head;
    actor_t *ready_queue_tail;
    size_t   ready_count;
};

void     scheduler_init(scheduler_t *sched);
void     scheduler_enqueue(scheduler_t *sched, actor_t *actor);
actor_t *scheduler_dequeue(scheduler_t *sched);
bool     scheduler_is_empty(const scheduler_t *sched);

#endif /* MICROKERNEL_SCHEDULER_H */
