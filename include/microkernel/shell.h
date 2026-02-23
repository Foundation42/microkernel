#ifndef MICROKERNEL_SHELL_H
#define MICROKERNEL_SHELL_H

#include "microkernel/runtime.h"
#include "microkernel/actor.h"

/* Shell protocol messages */
#define MSG_SHELL_INPUT  100
#define MSG_SHELL_INIT   101
#define MSG_SHELL_PROMPT 109   /* shell → console: "ready for input" */

/**
 * Spawn the shell actor.  Returns the shell actor ID.
 * The shell reads MSG_SHELL_INPUT lines and dispatches commands.
 * Output goes to stdout (console VFS on ESP32, terminal on Linux).
 */
actor_id_t shell_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_SHELL_H */
