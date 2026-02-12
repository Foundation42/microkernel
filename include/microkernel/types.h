#ifndef MICROKERNEL_TYPES_H
#define MICROKERNEL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef uint64_t actor_id_t;
typedef uint32_t node_id_t;
typedef uint32_t msg_type_t;

/* Actor ID encoding: upper 32 bits = node_id, lower 32 bits = local sequence */
static inline actor_id_t actor_id_make(node_id_t node, uint32_t seq) {
    return ((uint64_t)node << 32) | (uint64_t)seq;
}

static inline node_id_t actor_id_node(actor_id_t id) {
    return (node_id_t)(id >> 32);
}

static inline uint32_t actor_id_seq(actor_id_t id) {
    return (uint32_t)(id & 0xFFFFFFFF);
}

#define ACTOR_ID_INVALID ((actor_id_t)0)

/* Actor lifecycle states */
typedef enum {
    ACTOR_IDLE,
    ACTOR_READY,
    ACTOR_RUNNING,
    ACTOR_STOPPED
} actor_status_t;

typedef uint32_t timer_id_t;
#define TIMER_ID_INVALID ((timer_id_t)0)

/* Forward declarations */
typedef struct message message_t;
typedef struct mailbox mailbox_t;
typedef struct actor actor_t;
typedef struct scheduler scheduler_t;
typedef struct runtime runtime_t;
typedef struct transport transport_t;

/* Behavior function: returns true to continue, false to stop */
typedef bool (*actor_behavior_fn)(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state);

#endif /* MICROKERNEL_TYPES_H */
