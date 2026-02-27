#ifndef MICROKERNEL_SEQUENCER_H
#define MICROKERNEL_SEQUENCER_H

#include "types.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Time base ───────────────────────────────────────────────────── */

typedef uint32_t tick_t;

#define SEQ_PPQN           480
#define SEQ_TICKS_PER_BEAT SEQ_PPQN
#define SEQ_TICKS_PER_BAR  (SEQ_PPQN * 4)  /* 4/4 time */

/* ── Pitch (microtonal) ──────────────────────────────────────────── */

typedef uint16_t pitch_t;

/* Upper byte = MIDI note (0–127), lower byte = cents (0–255 maps to 0–99) */

static inline pitch_t pitch_make(uint8_t note, uint8_t cents_0_99) {
    uint8_t cents_byte = (uint8_t)((cents_0_99 * 255) / 99);
    return (uint16_t)((note << 8) | cents_byte);
}

static inline uint8_t pitch_note(pitch_t p) {
    return (uint8_t)(p >> 8);
}

static inline uint8_t pitch_cents(pitch_t p) {
    return (uint8_t)(((p & 0xFF) * 99) / 255);
}

static inline pitch_t pitch_transpose(pitch_t p, int8_t semitones, int8_t cents_0_99) {
    int total = (pitch_note(p) * 100) + pitch_cents(p) +
                (semitones * 100) + cents_0_99;
    int new_note = total / 100;
    int new_cents = total % 100;
    if (new_note < 0) new_note = 0;
    if (new_note > 127) new_note = 127;
    if (new_cents < 0) new_cents = 0;
    return pitch_make((uint8_t)new_note, (uint8_t)new_cents);
}

static inline uint8_t pitch_to_midi_note(pitch_t p) {
    return pitch_note(p);
}

/* ── Event types ─────────────────────────────────────────────────── */

#define SEQ_EVENT_NOTE         0
#define SEQ_EVENT_NOTE_OFF     1
#define SEQ_EVENT_CONTROL      2
#define SEQ_EVENT_PITCH_BEND   3
#define SEQ_EVENT_PROGRAM      4
#define SEQ_EVENT_AFTERTOUCH   5
#define SEQ_EVENT_TEMPO        6

/* ── Event flags ─────────────────────────────────────────────────── */

#define SEQ_FLAG_LEGATO    0x01
#define SEQ_FLAG_ACCENT    0x02
#define SEQ_FLAG_SLIDE     0x04
#define SEQ_FLAG_GHOST     0x08
#define SEQ_FLAG_BYPASS_FX 0x10
#define SEQ_FLAG_GENERATED 0x20
#define SEQ_FLAG_MUTED     0x40
#define SEQ_FLAG_SELECTED  0x80

/* ── Event struct (16 bytes packed) ──────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  type;       /* SEQ_EVENT_* */
    uint8_t  flags;      /* SEQ_FLAG_* bitmask */
    uint8_t  _pad[2];
    tick_t   tick;       /* absolute position in ticks */

    union {
        struct {
            pitch_t  pitch;       /* 2 bytes */
            uint8_t  velocity;    /* 1 byte */
            uint8_t  channel;     /* 1 byte */
            tick_t   duration;    /* 4 bytes */
        } note;

        struct {
            pitch_t  pitch;       /* 2 bytes (for note-off matching) */
            uint8_t  velocity;    /* 1 byte (release velocity) */
            uint8_t  channel;     /* 1 byte */
            uint32_t _pad;
        } note_off;

        struct {
            uint16_t value;       /* 0–65535 high-res */
            uint8_t  cc_number;
            uint8_t  channel;
            uint32_t _pad;
        } control;

        struct {
            int16_t  value;       /* 14-bit signed */
            uint8_t  channel;
            uint8_t  _pad[5];
        } pitch_bend;

        struct {
            uint8_t  program;
            uint8_t  channel;
            uint8_t  _pad[6];
        } program;

        struct {
            uint8_t  value;       /* 0–127 */
            uint8_t  channel;
            uint8_t  _pad[6];
        } aftertouch;

        struct {
            uint32_t bpm_x100;    /* BPM × 100 (integer, no float) */
            uint32_t _pad;
        } tempo;

        uint8_t raw[8];
    } data;
} seq_event_t;

/* ── Limits ──────────────────────────────────────────────────────── */

#define SEQ_MAX_EVENTS   512    /* max source events per pattern load */
#define SEQ_MAX_EXPANDED 1024   /* after note-off expansion */
#define SEQ_MAX_TRACKS   8

/* ── Message payloads ────────────────────────────────────────────── */

/*
 * Message types (defined in services.h):
 *   MSG_SEQ_LOAD_PATTERN   0xFF000080  load pattern (flex payload)
 *   MSG_SEQ_START          0xFF000081  start playback (empty)
 *   MSG_SEQ_STOP           0xFF000082  stop playback (empty)
 *   MSG_SEQ_PAUSE          0xFF000083  pause/resume toggle (empty)
 *   MSG_SEQ_SET_TEMPO      0xFF000084  set BPM (seq_tempo_payload_t)
 *   MSG_SEQ_SET_POSITION   0xFF000085  seek (seq_position_payload_t)
 *   MSG_SEQ_SET_LOOP       0xFF000086  set loop (seq_loop_payload_t)
 *   MSG_SEQ_OK             0xFF000090  success reply (empty)
 *   MSG_SEQ_ERROR          0xFF000091  error reply (error string)
 *   MSG_SEQ_STATUS         0xFF000092  status reply (seq_status_payload_t)
 *   MSG_SEQ_POSITION       0xFF000093  position update (seq_position_payload_t)
 */

typedef struct {
    uint8_t  track;            /* track index (0–7) */
    uint8_t  slot;             /* pattern slot (0 or 1) */
    uint8_t  _pad[2];
    tick_t   length;           /* pattern length in ticks */
    uint16_t event_count;      /* number of events in events[] */
    uint16_t _pad2;
    char     name[32];         /* pattern name */
    seq_event_t events[];      /* flex array of events */
} seq_load_payload_t;

typedef struct {
    uint32_t bpm_x100;         /* BPM × 100 (e.g. 12000 = 120.00 BPM) */
} seq_tempo_payload_t;

typedef struct {
    tick_t   tick;             /* position in ticks */
} seq_position_payload_t;

typedef struct {
    bool     enabled;
    uint8_t  _pad[3];
    tick_t   start_tick;       /* loop start */
    tick_t   end_tick;         /* loop end (0 = pattern length) */
} seq_loop_payload_t;

typedef struct {
    bool     playing;
    bool     paused;
    bool     looping;
    uint8_t  _pad;
    uint32_t bpm_x100;
    tick_t   current_tick;
    tick_t   pattern_length;
    uint16_t event_count;
    uint16_t _pad2;
} seq_status_payload_t;

/* ── Convenience constructors ────────────────────────────────────── */

static inline seq_event_t seq_note(tick_t tick, uint8_t note, uint8_t vel,
                                   tick_t duration, uint8_t channel) {
    seq_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SEQ_EVENT_NOTE;
    ev.tick = tick;
    ev.data.note.pitch    = pitch_make(note, 0);
    ev.data.note.velocity = vel;
    ev.data.note.channel  = channel;
    ev.data.note.duration = duration;
    return ev;
}

static inline seq_event_t seq_cc(tick_t tick, uint8_t cc, uint8_t val,
                                 uint8_t channel) {
    seq_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SEQ_EVENT_CONTROL;
    ev.tick = tick;
    ev.data.control.cc_number = cc;
    ev.data.control.value     = (uint16_t)val << 9;  /* scale 7-bit to 16-bit */
    ev.data.control.channel   = channel;
    return ev;
}

static inline seq_event_t seq_program(tick_t tick, uint8_t program,
                                      uint8_t channel) {
    seq_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SEQ_EVENT_PROGRAM;
    ev.tick = tick;
    ev.data.program.program = program;
    ev.data.program.channel = channel;
    return ev;
}

/* ── Build load payload helper ───────────────────────────────────── */

/* Allocates a seq_load_payload_t with room for `count` events.
 * Caller must free().  Returns NULL on allocation failure. */
static inline seq_load_payload_t *seq_build_load_payload(
        uint8_t track, uint8_t slot, tick_t length,
        const char *name, const seq_event_t *events, uint16_t count) {
    size_t sz = sizeof(seq_load_payload_t) + (size_t)count * sizeof(seq_event_t);
    seq_load_payload_t *p = (seq_load_payload_t *)calloc(1, sz);
    if (!p) return NULL;
    p->track = track;
    p->slot  = slot;
    p->length = length;
    p->event_count = count;
    if (name) {
        strncpy(p->name, name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
    }
    if (count > 0 && events)
        memcpy(p->events, events, (size_t)count * sizeof(seq_event_t));
    return p;
}

static inline size_t seq_load_payload_size(uint16_t count) {
    return sizeof(seq_load_payload_t) + (size_t)count * sizeof(seq_event_t);
}

/* ── Init ────────────────────────────────────────────────────────── */

/*
 * Spawn the sequencer actor.  Registers as "/sys/sequencer".
 * Requires MIDI actor at "/node/hardware/midi".
 * Returns actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t sequencer_init(runtime_t *rt);

#endif /* MICROKERNEL_SEQUENCER_H */
