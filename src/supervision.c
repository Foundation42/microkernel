#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/supervision.h"
#include "runtime_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Internal constants ────────────────────────────────────────────── */

#ifndef MAX_SUPERVISOR_CHILDREN
#define MAX_SUPERVISOR_CHILDREN 16
#endif
#ifndef MAX_RESTART_HISTORY
#define MAX_RESTART_HISTORY 32
#endif

#define MSG_SUP_START ((msg_type_t)0xFF000011)

/* ── Supervisor state ──────────────────────────────────────────────── */

typedef struct {
    restart_strategy_t strategy;
    int                max_restarts;
    uint64_t           window_ms;
    child_spec_t       specs[MAX_SUPERVISOR_CHILDREN];
    size_t             n_specs;
    actor_id_t         children[MAX_SUPERVISOR_CHILDREN];
    uint64_t           restart_times[MAX_RESTART_HISTORY];
    size_t             restart_head;
    size_t             restart_count;
    bool               shutting_down;
} supervisor_state_t;

/* ── Helpers ────────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static actor_id_t spawn_child(runtime_t *rt, actor_id_t sup_id,
                               const child_spec_t *spec) {
    void *state = spec->factory ? spec->factory(spec->factory_arg) : NULL;
    actor_id_t child = actor_spawn(rt, spec->behavior, state,
                                    spec->free_state, spec->mailbox_size);
    if (child != ACTOR_ID_INVALID) {
        runtime_set_actor_parent(rt, child, sup_id);
    }
    return child;
}

static bool should_restart(restart_type_t type, uint8_t exit_reason) {
    switch (type) {
    case RESTART_PERMANENT:  return true;
    case RESTART_TRANSIENT:  return exit_reason != EXIT_NORMAL;
    case RESTART_TEMPORARY:  return false;
    }
    return false;
}

static bool check_restart_limit(supervisor_state_t *st) {
    if (st->max_restarts <= 0) return false;

    uint64_t t = now_ms();

    /* Record this restart */
    st->restart_times[st->restart_head] = t;
    st->restart_head = (st->restart_head + 1) % MAX_RESTART_HISTORY;
    if (st->restart_count < MAX_RESTART_HISTORY)
        st->restart_count++;

    /* Count restarts within window */
    int recent = 0;
    for (size_t i = 0; i < st->restart_count; i++) {
        if (t - st->restart_times[i] <= st->window_ms)
            recent++;
    }

    return recent <= st->max_restarts;
}

static void stop_child(runtime_t *rt, supervisor_state_t *st, size_t idx) {
    actor_id_t id = st->children[idx];
    if (id == ACTOR_ID_INVALID) return;
    /* Clear parent to suppress cascading MSG_CHILD_EXIT */
    runtime_set_actor_parent(rt, id, ACTOR_ID_INVALID);
    actor_stop(rt, id);
    st->children[idx] = ACTOR_ID_INVALID;
}

static void stop_all_children(runtime_t *rt, supervisor_state_t *st) {
    for (size_t i = 0; i < st->n_specs; i++) {
        stop_child(rt, st, i);
    }
}

static void stop_children_from(runtime_t *rt, supervisor_state_t *st,
                                size_t from) {
    for (size_t i = from; i < st->n_specs; i++) {
        stop_child(rt, st, i);
    }
}

/* ── Supervisor behavior ───────────────────────────────────────────── */

static bool supervisor_behavior(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    supervisor_state_t *st = (supervisor_state_t *)state;

    if (msg->type == MSG_SUP_START) {
        actor_id_t sup_id = self->id;
        for (size_t i = 0; i < st->n_specs; i++) {
            st->children[i] = spawn_child(rt, sup_id, &st->specs[i]);
        }
        return true;
    }

    if (msg->type == MSG_CHILD_EXIT) {
        if (st->shutting_down) return true;

        const child_exit_payload_t *p =
            (const child_exit_payload_t *)msg->payload;

        /* Find which child died */
        size_t child_idx = st->n_specs; /* sentinel */
        for (size_t i = 0; i < st->n_specs; i++) {
            if (st->children[i] == p->child_id) {
                child_idx = i;
                break;
            }
        }
        if (child_idx >= st->n_specs) return true; /* unknown child */

        st->children[child_idx] = ACTOR_ID_INVALID;

        if (!should_restart(st->specs[child_idx].restart_type,
                            p->exit_reason)) {
            return true;
        }

        if (!check_restart_limit(st)) {
            /* Exceeded restart limit — give up */
            st->shutting_down = true;
            stop_all_children(rt, st);
            return false;
        }

        actor_id_t sup_id = self->id;

        switch (st->strategy) {
        case STRATEGY_ONE_FOR_ONE:
            st->children[child_idx] = spawn_child(rt, sup_id,
                                                    &st->specs[child_idx]);
            break;

        case STRATEGY_ONE_FOR_ALL:
            stop_all_children(rt, st);
            for (size_t i = 0; i < st->n_specs; i++) {
                st->children[i] = spawn_child(rt, sup_id, &st->specs[i]);
            }
            break;

        case STRATEGY_REST_FOR_ONE:
            stop_children_from(rt, st, child_idx);
            for (size_t i = child_idx; i < st->n_specs; i++) {
                st->children[i] = spawn_child(rt, sup_id, &st->specs[i]);
            }
            break;
        }

        return true;
    }

    return true;
}

/* ── Public API ────────────────────────────────────────────────────── */

actor_id_t supervisor_start(runtime_t *rt,
                            restart_strategy_t strategy,
                            int max_restarts, uint64_t window_ms,
                            const child_spec_t *specs, size_t n_specs) {
    if (n_specs == 0 || n_specs > MAX_SUPERVISOR_CHILDREN) return ACTOR_ID_INVALID;

    supervisor_state_t *st = calloc(1, sizeof(*st));
    if (!st) return ACTOR_ID_INVALID;

    st->strategy = strategy;
    st->max_restarts = max_restarts;
    st->window_ms = window_ms;
    st->n_specs = n_specs;
    memcpy(st->specs, specs, n_specs * sizeof(child_spec_t));

    actor_id_t sup_id = actor_spawn(rt, supervisor_behavior, st, free,
                                     n_specs + 4);
    if (sup_id == ACTOR_ID_INVALID) {
        free(st);
        return ACTOR_ID_INVALID;
    }

    /* Send MSG_SUP_START to kick off child spawning */
    actor_send(rt, sup_id, MSG_SUP_START, NULL, 0);

    return sup_id;
}

actor_id_t supervisor_get_child(runtime_t *rt, actor_id_t sup_id,
                                 size_t index) {
    supervisor_state_t *st = (supervisor_state_t *)runtime_get_actor_state(rt, sup_id);
    if (!st || index >= st->n_specs) return ACTOR_ID_INVALID;
    return st->children[index];
}

void *supervisor_get_factory_arg(runtime_t *rt, actor_id_t sup_id,
                                  size_t index) {
    supervisor_state_t *st = (supervisor_state_t *)runtime_get_actor_state(rt, sup_id);
    if (!st || index >= st->n_specs) return NULL;
    return st->specs[index].factory_arg;
}

int supervisor_replace_child(runtime_t *rt, actor_id_t sup_id,
                              actor_id_t old_child, actor_id_t new_child,
                              void *new_factory_arg, void **old_factory_arg_out) {
    supervisor_state_t *st = (supervisor_state_t *)runtime_get_actor_state(rt, sup_id);
    if (!st) return -1;

    for (size_t i = 0; i < st->n_specs; i++) {
        if (st->children[i] == old_child) {
            st->children[i] = new_child;
            if (old_factory_arg_out)
                *old_factory_arg_out = st->specs[i].factory_arg;
            st->specs[i].factory_arg = new_factory_arg;
            return (int)i;
        }
    }
    return -1;
}

void supervisor_stop(runtime_t *rt, actor_id_t sup_id) {
    supervisor_state_t *st = (supervisor_state_t *)runtime_get_actor_state(rt, sup_id);
    if (st) {
        st->shutting_down = true;
        stop_all_children(rt, st);
    }
    actor_stop(rt, sup_id);
}
