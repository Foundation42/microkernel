#include "microkernel/midi_monitor.h"
#include "microkernel/midi.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Note name lookup ─────────────────────────────────────────────── */

static const char *s_note_names[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static void note_str(uint8_t note, char *buf, size_t sz) {
    int octave = (int)(note / 12) - 1;
    snprintf(buf, sz, "%-2s%d", s_note_names[note % 12], octave);
}

/* ── Message type name ────────────────────────────────────────────── */

static const char *msg_name(uint8_t status) {
    switch (status & 0xF0) {
    case 0x80: return "NoteOff ";
    case 0x90: return "NoteOn  ";
    case 0xA0: return "PolyAT  ";
    case 0xB0: return "CC      ";
    case 0xC0: return "PgmChg  ";
    case 0xD0: return "ChanAT  ";
    case 0xE0: return "Pitch   ";
    }
    switch (status) {
    case 0xF8: return "Clock";
    case 0xFA: return "Start";
    case 0xFB: return "Continue";
    case 0xFC: return "Stop";
    case 0xFE: return "ActiveSns";
    case 0xFF: return "Reset";
    default:   return "System";
    }
}

/* ── State ────────────────────────────────────────────────────────── */

typedef struct {
    actor_id_t midi_id;
} monitor_state_t;

/* ── Behavior ─────────────────────────────────────────────────────── */

#define MONITOR_BOOTSTRAP 1

static bool monitor_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    monitor_state_t *s = state;

    if (msg->type == MONITOR_BOOTSTRAP) {
        /* Subscribe from inside behavior context so msg->source is correct */
        midi_subscribe_payload_t sub = {
            .channel = 0xFF,
            .msg_filter = MIDI_FILTER_ALL
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT) {
        if (msg->payload_size < sizeof(midi_event_payload_t)) return true;
        const midi_event_payload_t *ev =
            (const midi_event_payload_t *)msg->payload;

        uint8_t type = ev->status & 0xF0;

        if (type >= 0x80 && type <= 0xE0) {
            /* Channel message */
            char nstr[8];

            switch (type) {
            case 0x80: /* Note Off */
            case 0x90: /* Note On */
                note_str(ev->data1, nstr, sizeof(nstr));
                printf("MIDI %-8s ch=%-2u  %s  vel=%u\n",
                       msg_name(ev->status), ev->channel + 1,
                       nstr, ev->data2);
                break;
            case 0xA0: /* Poly Aftertouch */
                note_str(ev->data1, nstr, sizeof(nstr));
                printf("MIDI %-8s ch=%-2u  %s  prs=%u\n",
                       msg_name(ev->status), ev->channel + 1,
                       nstr, ev->data2);
                break;
            case 0xB0: /* CC */
                printf("MIDI %-8s ch=%-2u  cc=%-3u  val=%u\n",
                       msg_name(ev->status), ev->channel + 1,
                       ev->data1, ev->data2);
                break;
            case 0xC0: /* Program Change */
                printf("MIDI %-8s ch=%-2u  pgm=%u\n",
                       msg_name(ev->status), ev->channel + 1,
                       ev->data1);
                break;
            case 0xD0: /* Channel Aftertouch */
                printf("MIDI %-8s ch=%-2u  prs=%u\n",
                       msg_name(ev->status), ev->channel + 1,
                       ev->data1);
                break;
            case 0xE0: /* Pitch Bend */
                {
                    int bend = (int)(ev->data1 | (ev->data2 << 7)) - 8192;
                    printf("MIDI %-8s ch=%-2u  val=%d\n",
                           msg_name(ev->status), ev->channel + 1, bend);
                }
                break;
            }
        } else {
            /* System real-time */
            printf("MIDI %s\n", msg_name(ev->status));
        }
        return true;
    }

    if (msg->type == MSG_MIDI_SYSEX_EVENT) {
        if (msg->payload_size < sizeof(midi_sysex_event_payload_t)) return true;
        const midi_sysex_event_payload_t *ev =
            (const midi_sysex_event_payload_t *)msg->payload;

        printf("MIDI SysEx    len=%-3u ", ev->length);
        int show = ev->length < 16 ? ev->length : 16;
        for (int i = 0; i < show; i++)
            printf("%02X ", ev->data[i]);
        if (ev->length > 16)
            printf("...");
        printf("\n");
        return true;
    }

    /* Ignore MSG_MIDI_OK and everything else */
    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void monitor_state_free(void *state) {
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t midi_monitor_init(runtime_t *rt) {
    actor_id_t midi_id = actor_lookup(rt, "/node/hardware/midi");
    if (midi_id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    monitor_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ACTOR_ID_INVALID;
    s->midi_id = midi_id;

    actor_id_t id = actor_spawn(rt, monitor_behavior, s, monitor_state_free, 64);
    if (id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    actor_register_name(rt, "/sys/midi_monitor", id);

    /* Bootstrap triggers subscription inside actor context */
    actor_send(rt, id, MONITOR_BOOTSTRAP, NULL, 0);

    return id;
}
