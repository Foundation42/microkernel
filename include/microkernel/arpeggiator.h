#ifndef MICROKERNEL_ARPEGGIATOR_H
#define MICROKERNEL_ARPEGGIATOR_H

#include "types.h"
#include <stdint.h>

/* Arpeggiator message types (defined in services.h):
 *   MSG_ARP_SET_BPM      0xFF000075  → arp  arp_bpm_payload_t
 *   MSG_ARP_SET_PATTERN   0xFF000076  → arp  arp_pattern_payload_t
 *   MSG_ARP_SET_OCTAVES   0xFF000077  → arp  arp_octaves_payload_t
 *   MSG_ARP_ENABLE        0xFF000078  → arp  arp_enable_payload_t
 */

/* ── Pattern types ────────────────────────────────────────────────── */

#define ARP_UP      0
#define ARP_DOWN    1
#define ARP_UPDOWN  2
#define ARP_RANDOM  3

/* ── Config payloads ──────────────────────────────────────────────── */

typedef struct {
    uint16_t bpm;        /* 30–300 */
    uint8_t  _pad[2];
} arp_bpm_payload_t;

typedef struct {
    uint8_t  pattern;    /* ARP_UP / ARP_DOWN / ARP_UPDOWN / ARP_RANDOM */
    uint8_t  _pad[3];
} arp_pattern_payload_t;

typedef struct {
    uint8_t  octaves;    /* 1–4 */
    uint8_t  _pad[3];
} arp_octaves_payload_t;

typedef struct {
    uint8_t  enable;     /* 0=bypass, 1=active */
    uint8_t  _pad[3];
} arp_enable_payload_t;

/*
 * Spawn the arpeggiator actor.  Registers as "/sys/arpeggiator".
 * Requires MIDI actor at "/node/hardware/midi".
 * Returns the actor ID, or ACTOR_ID_INVALID if MIDI actor not found.
 *
 * Defaults: BPM=120, pattern=UP, octaves=1, enabled.
 */
actor_id_t arpeggiator_init(runtime_t *rt);

#endif /* MICROKERNEL_ARPEGGIATOR_H */
