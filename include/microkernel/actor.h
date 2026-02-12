#ifndef MICROKERNEL_ACTOR_H
#define MICROKERNEL_ACTOR_H

#include "types.h"

struct actor {
    actor_id_t        id;
    node_id_t         node_id;
    actor_status_t    status;

    mailbox_t        *mailbox;
    actor_behavior_fn behavior;
    void             *state;
    void            (*free_state)(void *);

    /* Scheduling: intrusive linked list */
    struct actor     *next;
    uint32_t          priority;
};

/* Create an actor with the given id, behavior, state, and mailbox capacity.
   free_state may be NULL if state does not need cleanup. */
actor_t *actor_create(actor_id_t id, node_id_t node_id,
                      actor_behavior_fn behavior, void *state,
                      void (*free_state)(void *), size_t mailbox_capacity);

/* Destroy an actor: destroys mailbox, calls free_state if set, frees struct. */
void actor_destroy(actor_t *a);

#endif /* MICROKERNEL_ACTOR_H */
