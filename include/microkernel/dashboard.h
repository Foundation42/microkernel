#ifndef MICROKERNEL_DASHBOARD_H
#define MICROKERNEL_DASHBOARD_H

#include "types.h"

/*
 * Spawn the dashboard actor.  Registers as "/sys/dashboard".
 * Requires the display actor to be running at "/node/hardware/display".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t dashboard_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_DASHBOARD_H */
