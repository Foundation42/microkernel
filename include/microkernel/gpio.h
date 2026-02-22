#ifndef MICROKERNEL_GPIO_H
#define MICROKERNEL_GPIO_H

#include "types.h"

/* GPIO message types (defined in services.h):
 *   MSG_GPIO_CONFIGURE   0xFF000020  → gpio  "pin=N\nmode=output"
 *   MSG_GPIO_WRITE       0xFF000021  → gpio  "pin=N\nvalue=1"
 *   MSG_GPIO_READ        0xFF000022  → gpio  "pin=N"
 *   MSG_GPIO_SUBSCRIBE   0xFF000023  → gpio  "pin=N\nedge=rising"
 *   MSG_GPIO_UNSUBSCRIBE 0xFF000024  → gpio  "pin=N"
 *   MSG_GPIO_OK          0xFF000028  ← gpio  (empty)
 *   MSG_GPIO_VALUE       0xFF000029  ← gpio  "pin=N\nvalue=V"
 *   MSG_GPIO_ERROR       0xFF00002A  ← gpio  error string
 *   MSG_GPIO_EVENT       0xFF00002B  ← gpio  "pin=N\nvalue=V\nedge=E"
 *
 * Modes: input, output, input_pullup, input_pulldown
 * Edges: rising, falling, both (default)
 */

/*
 * Spawn the GPIO actor.  Registers as "/node/hardware/gpio".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t gpio_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_GPIO_H */
