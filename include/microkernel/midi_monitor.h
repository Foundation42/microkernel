#ifndef MICROKERNEL_MIDI_MONITOR_H
#define MICROKERNEL_MIDI_MONITOR_H

#include "types.h"

/* MIDI Monitor — subscribes to all MIDI events and prints human-readable
 * output to stdout.  Registers as "/sys/midi_monitor".
 *
 * Requires MIDI actor at "/node/hardware/midi".
 * Returns the actor ID, or ACTOR_ID_INVALID if MIDI actor not found.
 */
actor_id_t midi_monitor_init(runtime_t *rt);

#endif /* MICROKERNEL_MIDI_MONITOR_H */
