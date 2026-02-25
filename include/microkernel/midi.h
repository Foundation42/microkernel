#ifndef MICROKERNEL_MIDI_H
#define MICROKERNEL_MIDI_H

#include "types.h"
#include <stdint.h>

/* MIDI message types (defined in services.h):
 *   MSG_MIDI_CONFIGURE     0xFF000070  → midi  midi_config_payload_t
 *   MSG_MIDI_SEND          0xFF000071  → midi  midi_send_payload_t
 *   MSG_MIDI_SEND_SYSEX    0xFF000072  → midi  midi_sysex_payload_t (flex)
 *   MSG_MIDI_SUBSCRIBE     0xFF000073  → midi  midi_subscribe_payload_t
 *   MSG_MIDI_UNSUBSCRIBE   0xFF000074  → midi  (empty)
 *   MSG_MIDI_OK            0xFF00007A  ← midi  (empty)
 *   MSG_MIDI_ERROR         0xFF00007B  ← midi  error string
 *   MSG_MIDI_EVENT         0xFF00007C  ← midi  midi_event_payload_t
 *   MSG_MIDI_SYSEX_EVENT   0xFF00007D  ← midi  midi_sysex_event_payload_t
 *
 * SC16IS752 dual UART-to-I2C bridge:
 *   Channel A = MIDI IN  (RX, interrupt-driven)
 *   Channel B = MIDI OUT (TX)
 */

/* ── Payload structs ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  i2c_port;    /* I2C bus port (0 or 1) */
    uint8_t  i2c_addr;    /* SC16IS752 7-bit address (default 0x48) */
    uint8_t  sda_pin;
    uint8_t  scl_pin;
    uint8_t  irq_pin;     /* GPIO pin for IRQ (active low) */
    uint8_t  _pad[3];
    uint32_t i2c_freq_hz; /* 100000 or 400000 */
} midi_config_payload_t;

typedef struct {
    uint8_t  status;      /* MIDI status byte (0x80–0xEF) */
    uint8_t  data1;       /* first data byte */
    uint8_t  data2;       /* second data byte (0 for 2-byte msgs) */
    uint8_t  _pad;
} midi_send_payload_t;

typedef struct {
    uint16_t length;      /* total SysEx length including F0 and F7 */
    uint8_t  _pad[2];
    uint8_t  data[];      /* raw SysEx bytes: F0 ... F7 */
} midi_sysex_payload_t;

typedef struct {
    uint8_t  channel;     /* MIDI channel 0–15, or 0xFF for all */
    uint8_t  msg_filter;  /* bitmask: see MIDI_FILTER_* below */
    uint8_t  _pad[2];
} midi_subscribe_payload_t;

/* Subscribe filter bitmask */
#define MIDI_FILTER_NOTE       0x01  /* Note On/Off */
#define MIDI_FILTER_CC         0x02  /* Control Change */
#define MIDI_FILTER_PROGRAM    0x04  /* Program Change */
#define MIDI_FILTER_PITCHBEND  0x08  /* Pitch Bend */
#define MIDI_FILTER_AFTERTOUCH 0x10  /* Channel/Poly Aftertouch */
#define MIDI_FILTER_SYSEX      0x20  /* System Exclusive */
#define MIDI_FILTER_REALTIME   0x40  /* Clock, Start, Stop, etc. */
#define MIDI_FILTER_ALL        0xFF  /* everything */

/* ── Event payloads (sent to subscribers) ─────────────────────────── */

typedef struct {
    uint8_t  status;      /* full status byte */
    uint8_t  data1;
    uint8_t  data2;
    uint8_t  channel;     /* extracted channel 0–15 (0xFF for system) */
} midi_event_payload_t;

typedef struct {
    uint16_t length;      /* total length including F0/F7 */
    uint8_t  _pad[2];
    uint8_t  data[];      /* F0 ... F7 */
} midi_sysex_event_payload_t;

/*
 * Spawn the MIDI actor.  Registers as "/node/hardware/midi".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t midi_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_MIDI_H */
