#ifndef MICROKERNEL_RUNTIME_H
#define MICROKERNEL_RUNTIME_H

#include "types.h"

/* Initialization / teardown */
runtime_t *runtime_init(node_id_t node_id, size_t max_actors);
void       runtime_destroy(runtime_t *rt);

/* Actor lifecycle */
actor_id_t actor_spawn(runtime_t *rt, actor_behavior_fn behavior,
                       void *initial_state, void (*free_state)(void *),
                       size_t mailbox_size);
void       actor_stop(runtime_t *rt, actor_id_t id);

/* Messaging */
bool actor_send(runtime_t *rt, actor_id_t dest, msg_type_t type,
                const void *payload, size_t payload_size);

/* Helpers for use inside behavior functions */
actor_id_t actor_self(runtime_t *rt);
void      *actor_state(runtime_t *rt);

/* Transport */
bool runtime_add_transport(runtime_t *rt, transport_t *transport);

/* Execution */
void runtime_run(runtime_t *rt);   /* Blocking event loop */
void runtime_step(runtime_t *rt);  /* Single scheduling iteration */
void runtime_stop(runtime_t *rt);  /* Signal shutdown */

#endif /* MICROKERNEL_RUNTIME_H */
