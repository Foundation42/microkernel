#include "microkernel/actor.h"
#include "microkernel/mailbox.h"
#include <stdlib.h>

actor_t *actor_create(actor_id_t id, node_id_t node_id,
                      actor_behavior_fn behavior, void *state,
                      void (*free_state)(void *), size_t mailbox_capacity) {
    actor_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;

    a->mailbox = mailbox_create(mailbox_capacity);
    if (!a->mailbox) {
        free(a);
        return NULL;
    }

    a->id = id;
    a->node_id = node_id;
    a->behavior = behavior;
    a->state = state;
    a->free_state = free_state;
    a->status = ACTOR_IDLE;
    return a;
}

void actor_destroy(actor_t *a) {
    if (!a) return;
    mailbox_destroy(a->mailbox);
    if (a->free_state && a->state) {
        a->free_state(a->state);
    }
    free(a);
}
