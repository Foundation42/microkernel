#ifndef MICROKERNEL_SUPERVISION_H
#define MICROKERNEL_SUPERVISION_H

#include "types.h"

/* Exit reasons */
#define EXIT_NORMAL  0   /* behavior returned false */
#define EXIT_KILLED  1   /* actor_stop() called */

/* Restart types (per-child policy) */
typedef enum {
    RESTART_PERMANENT,   /* always restart */
    RESTART_TRANSIENT,   /* restart only on abnormal exit */
    RESTART_TEMPORARY    /* never restart */
} restart_type_t;

/* Restart strategies (per-supervisor) */
typedef enum {
    STRATEGY_ONE_FOR_ONE,
    STRATEGY_ONE_FOR_ALL,
    STRATEGY_REST_FOR_ONE
} restart_strategy_t;

/* State factory: produces fresh state for restarts */
typedef void *(*state_factory_fn)(void *factory_arg);

/* Child specification */
typedef struct {
    const char       *name;           /* for logging, may be NULL */
    actor_behavior_fn behavior;
    state_factory_fn  factory;        /* creates initial state; NULL = no state */
    void             *factory_arg;    /* passed to factory on each (re)start */
    void            (*free_state)(void *);
    size_t            mailbox_size;
    restart_type_t    restart_type;
} child_spec_t;

/* MSG_CHILD_EXIT payload */
typedef struct {
    actor_id_t child_id;
    uint8_t    exit_reason;
} child_exit_payload_t;

/* Spawn a supervisor that manages children per the given specs */
actor_id_t supervisor_start(runtime_t *rt,
                            restart_strategy_t strategy,
                            int max_restarts, uint64_t window_ms,
                            const child_spec_t *specs, size_t n_specs);

/* Get the current actor_id of the Nth child (for testing/inspection) */
actor_id_t supervisor_get_child(runtime_t *rt, actor_id_t sup_id,
                                 size_t index);

/* Stop supervisor and all its children */
void supervisor_stop(runtime_t *rt, actor_id_t sup_id);

/* Get the factory_arg for the Nth child spec (for cleanup after reload) */
void *supervisor_get_factory_arg(runtime_t *rt, actor_id_t sup_id,
                                  size_t index);

/* Replace a child in-place (for hot reload).
   Swaps old_child → new_child in children[], replaces factory_arg in spec.
   Returns child index (>=0) on success, -1 if old_child not found.
   *old_factory_arg_out receives the previous factory_arg (caller frees). */
int supervisor_replace_child(runtime_t *rt, actor_id_t sup_id,
                              actor_id_t old_child, actor_id_t new_child,
                              void *new_factory_arg, void **old_factory_arg_out);

#endif /* MICROKERNEL_SUPERVISION_H */
