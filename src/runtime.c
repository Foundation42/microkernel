#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/mailbox.h"
#include "microkernel/message.h"
#include "microkernel/scheduler.h"
#include <stdlib.h>
#include <string.h>

struct runtime {
    node_id_t    node_id;
    actor_t    **actors;         /* flat array indexed by local sequence */
    size_t       max_actors;
    uint32_t     next_actor_seq; /* monotonic, starts at 1 (0 = invalid) */
    size_t       actor_count;
    scheduler_t  scheduler;      /* embedded by value */
    actor_t     *current_actor;  /* set during behavior dispatch */
    bool         running;
};

/* ── Initialization / teardown ──────────────────────────────────────── */

runtime_t *runtime_init(node_id_t node_id, size_t max_actors) {
    runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return NULL;

    rt->actors = calloc(max_actors, sizeof(actor_t *));
    if (!rt->actors) {
        free(rt);
        return NULL;
    }

    rt->node_id = node_id;
    rt->max_actors = max_actors;
    rt->next_actor_seq = 1;
    scheduler_init(&rt->scheduler);
    return rt;
}

void runtime_destroy(runtime_t *rt) {
    if (!rt) return;
    for (size_t i = 0; i < rt->max_actors; i++) {
        if (rt->actors[i]) {
            actor_destroy(rt->actors[i]);
        }
    }
    free(rt->actors);
    free(rt);
}

/* ── Actor lifecycle ────────────────────────────────────────────────── */

actor_id_t actor_spawn(runtime_t *rt, actor_behavior_fn behavior,
                       void *initial_state, void (*free_state)(void *),
                       size_t mailbox_size) {
    if (rt->actor_count >= rt->max_actors) return ACTOR_ID_INVALID;

    uint32_t seq = rt->next_actor_seq++;
    actor_id_t id = actor_id_make(rt->node_id, seq);

    actor_t *a = actor_create(id, rt->node_id, behavior,
                              initial_state, free_state, mailbox_size);
    if (!a) return ACTOR_ID_INVALID;

    /* Store at index = seq (slot 0 is unused since seq starts at 1) */
    if (seq >= rt->max_actors) {
        actor_destroy(a);
        return ACTOR_ID_INVALID;
    }
    rt->actors[seq] = a;
    rt->actor_count++;
    return id;
}

void actor_stop(runtime_t *rt, actor_id_t id) {
    uint32_t seq = actor_id_seq(id);
    if (seq == 0 || seq >= rt->max_actors) return;
    actor_t *a = rt->actors[seq];
    if (a) a->status = ACTOR_STOPPED;
}

/* ── Internal: look up an actor by id ───────────────────────────────── */

static actor_t *lookup(runtime_t *rt, actor_id_t id) {
    uint32_t seq = actor_id_seq(id);
    if (seq == 0 || seq >= rt->max_actors) return NULL;
    actor_t *a = rt->actors[seq];
    if (!a || a->status == ACTOR_STOPPED) return NULL;
    return a;
}

/* ── Messaging ──────────────────────────────────────────────────────── */

bool actor_send(runtime_t *rt, actor_id_t dest, msg_type_t type,
                const void *payload, size_t payload_size) {
    actor_t *target = lookup(rt, dest);
    if (!target) return false;

    actor_id_t source = rt->current_actor ? rt->current_actor->id
                                          : ACTOR_ID_INVALID;

    message_t *msg = message_create(source, dest, type,
                                    payload, payload_size);
    if (!msg) return false;

    if (!mailbox_enqueue(target->mailbox, msg)) {
        message_destroy(msg);
        return false;
    }

    /* Schedule target if not already ready/running */
    if (target->status == ACTOR_IDLE) {
        scheduler_enqueue(&rt->scheduler, target);
    }
    return true;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

actor_id_t actor_self(runtime_t *rt) {
    return rt->current_actor ? rt->current_actor->id : ACTOR_ID_INVALID;
}

void *actor_state(runtime_t *rt) {
    return rt->current_actor ? rt->current_actor->state : NULL;
}

/* ── Execution ──────────────────────────────────────────────────────── */

static void cleanup_stopped(runtime_t *rt) {
    for (size_t i = 1; i < rt->max_actors; i++) {
        actor_t *a = rt->actors[i];
        if (a && a->status == ACTOR_STOPPED) {
            actor_destroy(a);
            rt->actors[i] = NULL;
            rt->actor_count--;
        }
    }
}

void runtime_step(runtime_t *rt) {
    actor_t *actor = scheduler_dequeue(&rt->scheduler);
    if (!actor) return;

    if (actor->status == ACTOR_STOPPED) return;

    actor->status = ACTOR_RUNNING;
    rt->current_actor = actor;

    /* Process one message per turn for fairness */
    message_t *msg = mailbox_dequeue(actor->mailbox);
    if (msg) {
        bool keep = actor->behavior(rt, actor, msg, actor->state);
        message_destroy(msg);
        if (!keep) {
            actor->status = ACTOR_STOPPED;
        }
    }

    rt->current_actor = NULL;

    /* Re-enqueue if still alive and has more messages */
    if (actor->status == ACTOR_RUNNING) {
        if (!mailbox_is_empty(actor->mailbox)) {
            actor->status = ACTOR_IDLE; /* reset before enqueue sets READY */
            scheduler_enqueue(&rt->scheduler, actor);
        } else {
            actor->status = ACTOR_IDLE;
        }
    }

    cleanup_stopped(rt);
}

void runtime_run(runtime_t *rt) {
    rt->running = true;
    while (rt->running && (rt->actor_count > 0)) {
        if (scheduler_is_empty(&rt->scheduler)) {
            /* No actors ready — nothing to do, stop. */
            break;
        }
        runtime_step(rt);
    }
    rt->running = false;
}

void runtime_stop(runtime_t *rt) {
    rt->running = false;
}
