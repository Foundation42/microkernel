#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include "microkernel/runtime.h"
#include "microkernel/services.h"

/* Internal types shared between runtime.c and service modules */

typedef struct {
    timer_id_t  id;       /* 0 = unused */
    actor_id_t  owner;
    int         fd;       /* timerfd (Linux) */
    bool        periodic;
} timer_entry_t;

typedef struct {
    char       name[64];
    actor_id_t actor_id;
    bool       occupied;
} name_entry_t;

/* Accessors for runtime internals (defined in runtime.c) */
timer_entry_t *runtime_get_timers(runtime_t *rt);
size_t         runtime_get_max_timers(void);
uint32_t       runtime_alloc_timer_id(runtime_t *rt);
actor_id_t     runtime_current_actor_id(runtime_t *rt);

actor_id_t     runtime_get_log_actor(runtime_t *rt);
void           runtime_set_log_actor(runtime_t *rt, actor_id_t id);
int            runtime_get_min_log_level(runtime_t *rt);

name_entry_t  *runtime_get_name_registry(runtime_t *rt);
size_t         runtime_get_name_registry_size(void);

#endif /* RUNTIME_INTERNAL_H */
