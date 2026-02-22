#ifndef MICROKERNEL_STATE_PERSIST_H
#define MICROKERNEL_STATE_PERSIST_H

#include "types.h"

/* Initialize state persistence with a base directory.
   Creates the directory if needed. Call once before using save/load. */
void state_persist_init(runtime_t *rt, const char *base_path);

/* Save binary blob. Returns 0 on success, -1 on error.
   actor_name: registered name or NULL (auto-resolve from current actor). */
int state_save(runtime_t *rt, const char *actor_name,
               const char *key, const void *data, size_t size);

/* Load binary blob. Returns bytes read, or -1 if not found. */
int state_load(runtime_t *rt, const char *actor_name,
               const char *key, void *buf, size_t cap);

/* Delete a state key. Returns 0 on success, -1 on error. */
int state_delete(runtime_t *rt, const char *actor_name, const char *key);

#endif /* MICROKERNEL_STATE_PERSIST_H */
