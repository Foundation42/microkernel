#include "microkernel/arpeggiator.h"
#include "microkernel/midi.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define ARP_MAX_HELD  16
#define ARP_BOOTSTRAP 1

/* ── State ────────────────────────────────────────────────────────── */

typedef struct {
    actor_id_t midi_id;

    /* Held notes (sorted by pitch) */
    uint8_t  held_notes[ARP_MAX_HELD];
    uint8_t  held_vels[ARP_MAX_HELD];
    uint8_t  held_channels[ARP_MAX_HELD];
    int      num_held;

    /* Arpeggio playback state */
    int      current_idx;       /* index into virtual note array */
    int      direction;         /* +1 or -1 for up-down */
    uint8_t  last_note;         /* currently sounding note (for Note Off) */
    uint8_t  last_channel;
    bool     note_playing;

    /* Settings */
    uint8_t  pattern;           /* ARP_UP / DOWN / UPDOWN / RANDOM */
    uint16_t bpm;               /* 30–300 */
    uint8_t  octaves;           /* 1–4 */
    bool     enabled;

    /* Timer */
    timer_id_t step_timer;
    bool       timer_running;
} arp_state_t;

/* ── Timer interval from BPM ──────────────────────────────────────── */

/* 16th notes: step_ms = 60000 / bpm / 4 */
static uint64_t bpm_to_ms(uint16_t bpm) {
    if (bpm < 30) bpm = 30;
    if (bpm > 300) bpm = 300;
    return 60000 / ((uint64_t)bpm * 4);
}

/* ── Note management (sorted insert/remove) ───────────────────────── */

static void add_held_note(arp_state_t *s, uint8_t note, uint8_t vel,
                          uint8_t channel) {
    /* Check for duplicate */
    for (int i = 0; i < s->num_held; i++) {
        if (s->held_notes[i] == note && s->held_channels[i] == channel) {
            s->held_vels[i] = vel; /* update velocity */
            return;
        }
    }

    if (s->num_held >= ARP_MAX_HELD) return;

    /* Sorted insert by pitch */
    int pos = s->num_held;
    for (int i = 0; i < s->num_held; i++) {
        if (note < s->held_notes[i]) {
            pos = i;
            break;
        }
    }

    /* Shift elements up */
    for (int i = s->num_held; i > pos; i--) {
        s->held_notes[i] = s->held_notes[i - 1];
        s->held_vels[i] = s->held_vels[i - 1];
        s->held_channels[i] = s->held_channels[i - 1];
    }

    s->held_notes[pos] = note;
    s->held_vels[pos] = vel;
    s->held_channels[pos] = channel;
    s->num_held++;

    /* Adjust current_idx if insertion was before it */
    if (pos <= s->current_idx && s->num_held > 1)
        s->current_idx++;
}

static void remove_held_note(arp_state_t *s, uint8_t note, uint8_t channel) {
    int pos = -1;
    for (int i = 0; i < s->num_held; i++) {
        if (s->held_notes[i] == note && s->held_channels[i] == channel) {
            pos = i;
            break;
        }
    }
    if (pos < 0) return;

    /* Shift elements down */
    for (int i = pos; i < s->num_held - 1; i++) {
        s->held_notes[i] = s->held_notes[i + 1];
        s->held_vels[i] = s->held_vels[i + 1];
        s->held_channels[i] = s->held_channels[i + 1];
    }
    s->num_held--;

    /* Adjust current_idx */
    if (s->num_held == 0) {
        s->current_idx = 0;
    } else if (pos < s->current_idx) {
        s->current_idx--;
    } else if (s->current_idx >= s->num_held * s->octaves) {
        s->current_idx = 0;
    }
}

/* ── Send a MIDI note via the MIDI actor ──────────────────────────── */

static void send_note(arp_state_t *s, runtime_t *rt,
                      uint8_t status, uint8_t note, uint8_t vel) {
    midi_send_payload_t p = {
        .status = status,
        .data1  = note,
        .data2  = vel,
    };
    actor_send(rt, s->midi_id, MSG_MIDI_SEND, &p, sizeof(p));
}

static void note_off_current(arp_state_t *s, runtime_t *rt) {
    if (!s->note_playing) return;
    send_note(s, rt, (uint8_t)(0x80 | (s->last_channel & 0x0F)),
              s->last_note, 0);
    s->note_playing = false;
}

/* ── Pattern advance ──────────────────────────────────────────────── */

static int advance_index(arp_state_t *s) {
    int total = s->num_held * s->octaves;
    if (total == 0) return -1;
    if (total == 1) return 0;

    switch (s->pattern) {
    case ARP_UP:
        s->current_idx = (s->current_idx + 1) % total;
        break;

    case ARP_DOWN:
        s->current_idx--;
        if (s->current_idx < 0)
            s->current_idx = total - 1;
        break;

    case ARP_UPDOWN:
        s->current_idx += s->direction;
        if (s->current_idx >= total) {
            s->direction = -1;
            s->current_idx = total - 2;
        }
        if (s->current_idx < 0) {
            s->direction = 1;
            s->current_idx = 1;
        }
        break;

    case ARP_RANDOM:
        {
            int new_idx;
            do {
                new_idx = rand() % total;
            } while (new_idx == s->current_idx && total > 1);
            s->current_idx = new_idx;
        }
        break;
    }

    return s->current_idx;
}

/* Get the actual MIDI note + velocity for a virtual index */
static void get_note_at(arp_state_t *s, int idx,
                        uint8_t *note, uint8_t *vel, uint8_t *channel) {
    int base_idx = idx % s->num_held;
    int octave_shift = idx / s->num_held;
    int n = (int)s->held_notes[base_idx] + 12 * octave_shift;
    *note = (uint8_t)(n > 127 ? 127 : n);
    *vel = s->held_vels[base_idx];
    *channel = s->held_channels[base_idx];
}

/* ── Timer management ─────────────────────────────────────────────── */

static void start_timer(arp_state_t *s, runtime_t *rt) {
    if (s->timer_running) return;
    uint64_t ms = bpm_to_ms(s->bpm);
    s->step_timer = actor_set_timer(rt, ms, true);
    s->timer_running = true;
}

static void stop_timer(arp_state_t *s, runtime_t *rt) {
    if (!s->timer_running) return;
    actor_cancel_timer(rt, s->step_timer);
    s->timer_running = false;
}

static void restart_timer(arp_state_t *s, runtime_t *rt) {
    stop_timer(s, rt);
    if (s->num_held > 0 && s->enabled)
        start_timer(s, rt);
}

/* ── Step: play next note in pattern ──────────────────────────────── */

static void arp_step(arp_state_t *s, runtime_t *rt) {
    if (s->num_held == 0 || !s->enabled) {
        note_off_current(s, rt);
        stop_timer(s, rt);
        return;
    }

    int idx = advance_index(s);
    if (idx < 0) return;

    uint8_t note, vel, channel;
    get_note_at(s, idx, &note, &vel, &channel);

    /* Legato: Note On first, then Note Off for previous */
    send_note(s, rt, (uint8_t)(0x90 | (channel & 0x0F)), note, vel);

    /* Note Off for previous note (after new Note On for legato) */
    if (s->note_playing && (s->last_note != note || s->last_channel != channel))
        note_off_current(s, rt);

    s->last_note = note;
    s->last_channel = channel;
    s->note_playing = true;
}

/* ── Event handlers ───────────────────────────────────────────────── */

static void handle_midi_event(arp_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(midi_event_payload_t)) return;
    const midi_event_payload_t *ev =
        (const midi_event_payload_t *)msg->payload;

    uint8_t type = ev->status & 0xF0;

    if (type == 0x90 && ev->data2 > 0) {
        /* Note On */
        bool was_empty = (s->num_held == 0);
        add_held_note(s, ev->data1, ev->data2, ev->channel);

        if (was_empty && s->enabled) {
            /* First note: play immediately and start timer */
            s->current_idx = 0;
            s->direction = 1;

            uint8_t note, vel, channel;
            get_note_at(s, 0, &note, &vel, &channel);
            send_note(s, rt, (uint8_t)(0x90 | (channel & 0x0F)), note, vel);
            s->last_note = note;
            s->last_channel = channel;
            s->note_playing = true;

            start_timer(s, rt);
        }
    } else if (type == 0x80 || (type == 0x90 && ev->data2 == 0)) {
        /* Note Off */
        remove_held_note(s, ev->data1, ev->channel);

        if (s->num_held == 0) {
            note_off_current(s, rt);
            stop_timer(s, rt);
        }
    }
}

static void handle_set_bpm(arp_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(arp_bpm_payload_t)) return;
    const arp_bpm_payload_t *p = (const arp_bpm_payload_t *)msg->payload;
    uint16_t bpm = p->bpm;
    if (bpm < 30) bpm = 30;
    if (bpm > 300) bpm = 300;
    s->bpm = bpm;
    if (s->timer_running)
        restart_timer(s, rt);
}

static void handle_set_pattern(arp_state_t *s, message_t *msg) {
    if (msg->payload_size < sizeof(arp_pattern_payload_t)) return;
    const arp_pattern_payload_t *p = (const arp_pattern_payload_t *)msg->payload;
    if (p->pattern > ARP_RANDOM) return;
    s->pattern = p->pattern;
    s->current_idx = 0;
    s->direction = 1;
}

static void handle_set_octaves(arp_state_t *s, message_t *msg) {
    if (msg->payload_size < sizeof(arp_octaves_payload_t)) return;
    const arp_octaves_payload_t *p = (const arp_octaves_payload_t *)msg->payload;
    uint8_t oct = p->octaves;
    if (oct < 1) oct = 1;
    if (oct > 4) oct = 4;
    s->octaves = oct;
    s->current_idx = 0;
}

static void handle_enable(arp_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(arp_enable_payload_t)) return;
    const arp_enable_payload_t *p = (const arp_enable_payload_t *)msg->payload;
    bool was_enabled = s->enabled;
    s->enabled = (p->enable != 0);

    if (s->enabled && !was_enabled && s->num_held > 0) {
        /* Re-enable with notes held — start arpeggio */
        s->current_idx = 0;
        s->direction = 1;
        start_timer(s, rt);
    } else if (!s->enabled && was_enabled) {
        /* Disable — stop and silence */
        note_off_current(s, rt);
        stop_timer(s, rt);
    }
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool arp_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    arp_state_t *s = state;

    if (msg->type == ARP_BOOTSTRAP) {
        /* Subscribe from inside behavior context so msg->source is correct */
        midi_subscribe_payload_t sub = {
            .channel = 0xFF,
            .msg_filter = MIDI_FILTER_NOTE
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    switch (msg->type) {
    case MSG_MIDI_EVENT:
        handle_midi_event(s, rt, msg);
        break;

    case MSG_TIMER:
        arp_step(s, rt);
        break;

    case MSG_ARP_SET_BPM:
        handle_set_bpm(s, rt, msg);
        break;

    case MSG_ARP_SET_PATTERN:
        handle_set_pattern(s, msg);
        break;

    case MSG_ARP_SET_OCTAVES:
        handle_set_octaves(s, msg);
        break;

    case MSG_ARP_ENABLE:
        handle_enable(s, rt, msg);
        break;

    default:
        /* Ignore MSG_MIDI_OK/ERROR and everything else */
        break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void arp_state_free(void *state) {
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t arpeggiator_init(runtime_t *rt) {
    actor_id_t midi_id = actor_lookup(rt, "/node/hardware/midi");
    if (midi_id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    arp_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ACTOR_ID_INVALID;

    s->midi_id = midi_id;
    s->pattern = ARP_UP;
    s->bpm = 120;
    s->octaves = 1;
    s->enabled = true;
    s->direction = 1;

    actor_id_t id = actor_spawn(rt, arp_behavior, s, arp_state_free, 64);
    if (id == ACTOR_ID_INVALID) {
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/sys/arpeggiator", id);

    /* Bootstrap triggers subscription inside actor context */
    actor_send(rt, id, ARP_BOOTSTRAP, NULL, 0);

    return id;
}
