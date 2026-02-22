#ifndef MICROKERNEL_WASM_ACTOR_H
#define MICROKERNEL_WASM_ACTOR_H

#include "types.h"

#ifdef HAVE_WASM

#define WASM_DEFAULT_STACK_SIZE  8192
#define WASM_DEFAULT_HEAP_SIZE   65536

/* Fiber stack classes for cooperative yielding from WASM host calls.
   FIBER_STACK_NONE = sync-only (Phase 12 behavior, zero overhead). */
typedef enum {
    FIBER_STACK_NONE   = 0,      /* No fiber (sync only) — DEFAULT */
    FIBER_STACK_TINY   = 16384,  /* 16 KB (8 KB usable after guard) */
    FIBER_STACK_SMALL  = 32768,  /* 32 KB */
    FIBER_STACK_MEDIUM = 65536,  /* 64 KB */
    FIBER_STACK_LARGE  = 131072, /* 128 KB */
} fiber_stack_class_t;

#define FIBER_GUARD_SIZE 8192

/* Redirect mk_print output to a file descriptor (-1 = stdout, default) */
void wasm_actor_set_print_fd(int fd);

/* Initialize/cleanup WAMR (call once per process).
   wasm_actors_init() uses the system allocator (malloc).
   wasm_actors_init_pool() uses a pre-allocated buffer — call this on
   memory-constrained targets (ESP32) to avoid fragmentation issues. */
bool wasm_actors_init(void);
bool wasm_actors_init_pool(void *pool_buf, size_t pool_size);
void wasm_actors_cleanup(void);

/* Spawn a WASM actor from a bytecode/AoT buffer (buffer is copied internally) */
actor_id_t actor_spawn_wasm(runtime_t *rt, const uint8_t *wasm_buf,
                             size_t wasm_size, size_t mailbox_size,
                             uint32_t stack_size, uint32_t heap_size,
                             fiber_stack_class_t fiber_stack);

/* Spawn a WASM actor from a file path */
actor_id_t actor_spawn_wasm_file(runtime_t *rt, const char *path,
                                  size_t mailbox_size,
                                  uint32_t stack_size, uint32_t heap_size,
                                  fiber_stack_class_t fiber_stack);

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
                                             uint32_t heap_size,
                                             fiber_stack_class_t fiber_stack);
void wasm_factory_arg_destroy(wasm_factory_arg_t *arg);

/* Hot code reload */
typedef enum {
    RELOAD_OK = 0,
    RELOAD_ERR_NOT_FOUND,
    RELOAD_ERR_NOT_WASM,
    RELOAD_ERR_FIBER_ACTIVE,
    RELOAD_ERR_MODULE_LOAD,
    RELOAD_ERR_INSTANCE,
} reload_result_t;

/* Atomically replace a WASM actor's module in-place.
   Preserves registered names, forwards queued messages, updates supervisor.
   Returns RELOAD_OK on success, error code otherwise.
   *new_id_out receives the new actor's ID. */
reload_result_t actor_reload_wasm(runtime_t *rt, actor_id_t old_id,
                                   const uint8_t *new_buf, size_t new_size,
                                   actor_id_t *new_id_out);

#endif /* HAVE_WASM */
#endif /* MICROKERNEL_WASM_ACTOR_H */
