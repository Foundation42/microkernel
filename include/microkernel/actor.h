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

    /* Supervision */
    actor_id_t        parent;       /* receives MSG_CHILD_EXIT on death; 0 = unlinked */
    uint8_t           exit_reason;  /* EXIT_NORMAL or EXIT_KILLED */
};

/* Create an actor with the given id, behavior, state, and mailbox capacity.
   free_state may be NULL if state does not need cleanup. */
actor_t *actor_create(actor_id_t id, node_id_t node_id,
                      actor_behavior_fn behavior, void *state,
                      void (*free_state)(void *), size_t mailbox_capacity);

/* Destroy an actor: destroys mailbox, calls free_state if set, frees struct. */
void actor_destroy(actor_t *a);

#endif /* MICROKERNEL_ACTOR_H */
