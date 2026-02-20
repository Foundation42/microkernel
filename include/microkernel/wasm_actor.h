#ifndef MICROKERNEL_WASM_ACTOR_H
#define MICROKERNEL_WASM_ACTOR_H

#include "types.h"

#ifdef HAVE_WASM

#define WASM_DEFAULT_STACK_SIZE  8192
#define WASM_DEFAULT_HEAP_SIZE   65536

/* Initialize/cleanup WAMR (call once per process) */
bool wasm_actors_init(void);
void wasm_actors_cleanup(void);

/* Spawn a WASM actor from a bytecode/AoT buffer (buffer is copied internally) */
actor_id_t actor_spawn_wasm(runtime_t *rt, const uint8_t *wasm_buf,
                             size_t wasm_size, size_t mailbox_size,
                             uint32_t stack_size, uint32_t heap_size);

/* Spawn a WASM actor from a file path */
actor_id_t actor_spawn_wasm_file(runtime_t *rt, const char *path,
                                  size_t mailbox_size,
                                  uint32_t stack_size, uint32_t heap_size);

/* Supervision integration -- use as child_spec_t fields */
bool  wasm_actor_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state);
void *wasm_actor_factory(void *arg);   /* arg = wasm_factory_arg_t* */
void  wasm_actor_free(void *state);

/* Factory arg: create once, reuse for supervised restarts.
   Copies wasm_buf and loads module once; instances are cheap to create. */
typedef struct wasm_factory_arg wasm_factory_arg_t;

wasm_factory_arg_t *wasm_factory_arg_create(const uint8_t *wasm_buf,
                                             size_t wasm_size,
                                             uint32_t stack_size,
                                             uint32_t heap_size);
void wasm_factory_arg_destroy(wasm_factory_arg_t *arg);

#endif /* HAVE_WASM */
#endif /* MICROKERNEL_WASM_ACTOR_H */
