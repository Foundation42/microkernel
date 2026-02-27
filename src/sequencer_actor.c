/* sequencer_actor.c — Pattern-based MIDI sequencer with timer-driven playback.
 *
 * Single-track playback engine (Phase 1): 480 PPQN, wall-clock tick
 * calculation, note-off pre-expansion, loop support.
 */

#include "microkernel/sequencer.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/midi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <esp_timer.h>
#else
#include <time.h>
#endif

/* ── Constants ───────────────────────────────────────────────────── */

#define SEQ_BOOTSTRAP   1
#define SEQ_TICK_MS     5     /* timer interval */

/* ── Portable wall clock ─────────────────────────────────────────── */

static uint64_t now_us(void) {
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

/* ── Pattern storage ─────────────────────────────────────────────── */

typedef struct {
    seq_event_t *events;       /* heap-allocated, sorted */
    uint16_t     event_count;
    tick_t       length;       /* pattern length in ticks */
    char         name[32];
} seq_pattern_t;

/* ── Track ───────────────────────────────────────────────────────── */

typedef struct {
    seq_pattern_t slots[2];
    uint8_t       active_slot;
    uint16_t      event_index;   /* current playback position */
    bool          muted;
} seq_track_t;

/* ── Actor state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t   midi_id;       /* MIDI actor for output */
    seq_track_t  tracks[SEQ_MAX_TRACKS];

    bool         playing;
    bool         paused;

    /* Tempo and timing */
    uint32_t     bpm_x100;      /* BPM × 100 (e.g. 12000 = 120 BPM) */
    tick_t       current_tick;
    uint64_t     start_time_us;  /* wall clock at play start */
    uint64_t     pause_time_us;  /* wall clock when paused */

    /* Loop */
    bool         loop_enabled;
    tick_t       loop_start;
    tick_t       loop_end;       /* 0 = pattern length */

    timer_id_t   timer;
} seq_state_t;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void reply_ok(runtime_t *rt, actor_id_t dest) {
    actor_send(rt, dest, MSG_SEQ_OK, NULL, 0);
}

static void reply_error(runtime_t *rt, actor_id_t dest, const char *err) {
    actor_send(rt, dest, MSG_SEQ_ERROR, err, strlen(err));
}

/* ── Tick calculation (integer math) ─────────────────────────────── */

/*
 * ticks = elapsed_us * bpm_x100 * PPQN / (60 * 1000000 * 100)
 *       = elapsed_us * bpm_x100 * PPQN / 6000000000
 *
 * To avoid overflow with uint64_t: split into stages.
 * Max elapsed_us for 24h = 86400e6, bpm_x100 max ~30000, PPQN=480
 * => 86400e6 * 30000 * 480 = 1.24e18 < UINT64_MAX (1.8e19)
 */
static tick_t calc_tick(uint64_t elapsed_us, uint32_t bpm_x100) {
    uint64_t numer = elapsed_us * (uint64_t)bpm_x100 * (uint64_t)SEQ_PPQN;
    return (tick_t)(numer / 6000000000ULL);
}

/* ── Event sorting ───────────────────────────────────────────────── */

static int event_compare(const void *a, const void *b) {
    const seq_event_t *ea = (const seq_event_t *)a;
    const seq_event_t *eb = (const seq_event_t *)b;

    if (ea->tick != eb->tick)
        return (ea->tick < eb->tick) ? -1 : 1;

    /* At same tick: note-on before note-off */
    if (ea->type == SEQ_EVENT_NOTE && eb->type == SEQ_EVENT_NOTE_OFF)
        return -1;
    if (ea->type == SEQ_EVENT_NOTE_OFF && eb->type == SEQ_EVENT_NOTE)
        return 1;

    return (ea->type < eb->type) ? -1 : (ea->type > eb->type) ? 1 : 0;
}

/* ── Note-off expansion ──────────────────────────────────────────── */

/*
 * Scans source events for NOTE events, generates NOTE_OFF events at
 * tick + duration.  Handles loop wrap: if note_off tick >= pattern_length,
 * wraps to note_off tick % pattern_length.
 *
 * Returns new event count, or -1 if buffer overflow.
 */
static int expand_note_offs(const seq_event_t *src, uint16_t src_count,
                            tick_t pattern_length,
                            seq_event_t *dst, int dst_max) {
    int out = 0;

    /* Copy all source events first */
    for (uint16_t i = 0; i < src_count; i++) {
        if (out >= dst_max) return -1;
        dst[out++] = src[i];
    }

    /* Generate note-off for each NOTE event */
    for (uint16_t i = 0; i < src_count; i++) {
        if (src[i].type != SEQ_EVENT_NOTE) continue;
        if (src[i].flags & SEQ_FLAG_MUTED) continue;

        if (out >= dst_max) return -1;

        seq_event_t off;
        memset(&off, 0, sizeof(off));
        off.type = SEQ_EVENT_NOTE_OFF;
        off.flags = src[i].flags;

        tick_t off_tick = src[i].tick + src[i].data.note.duration;
        if (pattern_length > 0 && off_tick >= pattern_length)
            off_tick = off_tick % pattern_length;
        off.tick = off_tick;

        off.data.note_off.pitch    = src[i].data.note.pitch;
        off.data.note_off.velocity = 64;  /* default release velocity */
        off.data.note_off.channel  = src[i].data.note.channel;

        dst[out++] = off;
    }

    /* Sort everything */
    qsort(dst, (size_t)out, sizeof(seq_event_t), event_compare);

    return out;
}

/* ── MIDI output ─────────────────────────────────────────────────── */

static void send_midi(runtime_t *rt, actor_id_t midi_id,
                      uint8_t status, uint8_t d1, uint8_t d2) {
    midi_send_payload_t pay;
    memset(&pay, 0, sizeof(pay));
    pay.status = status;
    pay.data1  = d1;
    pay.data2  = d2;
    actor_send(rt, midi_id, MSG_MIDI_SEND, &pay, sizeof(pay));
}

static void emit_event(runtime_t *rt, actor_id_t midi_id,
                       const seq_event_t *ev) {
    if (ev->flags & SEQ_FLAG_MUTED) return;

    switch (ev->type) {
    case SEQ_EVENT_NOTE: {
        uint8_t note = pitch_to_midi_note(ev->data.note.pitch);
        uint8_t vel  = ev->data.note.velocity;
        uint8_t ch   = ev->data.note.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0x90 | ch), note, vel);
        break;
    }
    case SEQ_EVENT_NOTE_OFF: {
        uint8_t note = pitch_to_midi_note(ev->data.note_off.pitch);
        uint8_t vel  = ev->data.note_off.velocity;
        uint8_t ch   = ev->data.note_off.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0x80 | ch), note, vel);
        break;
    }
    case SEQ_EVENT_CONTROL: {
        uint8_t cc  = ev->data.control.cc_number;
        uint8_t val = (uint8_t)(ev->data.control.value >> 9); /* 16-bit → 7-bit */
        uint8_t ch  = ev->data.control.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0xB0 | ch), cc, val);
        break;
    }
    case SEQ_EVENT_PROGRAM: {
        uint8_t pgm = ev->data.program.program;
        uint8_t ch  = ev->data.program.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0xC0 | ch), pgm, 0);
        break;
    }
    case SEQ_EVENT_PITCH_BEND: {
        int16_t val = ev->data.pitch_bend.value;
        uint8_t ch  = ev->data.pitch_bend.channel & 0x0F;
        /* Convert signed 14-bit to MIDI pitch bend (0–16383, center=8192) */
        uint16_t midi_val = (uint16_t)(val + 8192);
        send_midi(rt, midi_id, (uint8_t)(0xE0 | ch),
                  (uint8_t)(midi_val & 0x7F),
                  (uint8_t)((midi_val >> 7) & 0x7F));
        break;
    }
    case SEQ_EVENT_AFTERTOUCH: {
        uint8_t val = ev->data.aftertouch.value;
        uint8_t ch  = ev->data.aftertouch.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0xD0 | ch), val, 0);
        break;
    }
    default:
        break;
    }
}

static void send_all_notes_off(runtime_t *rt, actor_id_t midi_id) {
    for (uint8_t ch = 0; ch < 16; ch++)
        send_midi(rt, midi_id, (uint8_t)(0xB0 | ch), 123, 0);
}

/* ── Playback engine ─────────────────────────────────────────────── */

static void emit_events_in_range(runtime_t *rt, seq_state_t *s,
                                 seq_track_t *trk,
                                 tick_t from, tick_t to) {
    seq_pattern_t *pat = &trk->slots[trk->active_slot];
    if (!pat->events || pat->event_count == 0) return;

    while (trk->event_index < pat->event_count) {
        const seq_event_t *ev = &pat->events[trk->event_index];
        if (ev->tick >= to) break;
        if (ev->tick >= from) {
            emit_event(rt, s->midi_id, ev);
        }
        trk->event_index++;
    }
}

static void handle_tick(runtime_t *rt, seq_state_t *s) {
    if (!s->playing || s->paused) return;

    uint64_t elapsed = now_us() - s->start_time_us;
    tick_t new_tick = calc_tick(elapsed, s->bpm_x100);

    /* Process track 0 (single-track for Phase 1) */
    seq_track_t *trk = &s->tracks[0];
    seq_pattern_t *pat = &trk->slots[trk->active_slot];
    if (!pat->events || pat->event_count == 0) return;

    tick_t effective_end = s->loop_end > 0 ? s->loop_end : pat->length;

    if (s->loop_enabled && effective_end > 0 && new_tick >= effective_end) {
        /* Emit remaining events up to loop end */
        emit_events_in_range(rt, s, trk, s->current_tick, effective_end);

        /* Wrap */
        tick_t loop_len = effective_end - s->loop_start;
        if (loop_len == 0) loop_len = 1; /* prevent infinite loop */

        /* Re-anchor start time to the loop point */
        uint64_t ticks_past_end = new_tick - effective_end;
        tick_t wrapped_ticks = ticks_past_end % loop_len;
        new_tick = s->loop_start + wrapped_ticks;

        /* Recalculate start_time_us so calc_tick returns new_tick now */
        /* new_tick = elapsed' * bpm_x100 * PPQN / 6000000000 */
        /* elapsed' = new_tick * 6000000000 / (bpm_x100 * PPQN) */
        uint64_t needed_us = (uint64_t)new_tick * 6000000000ULL /
                             ((uint64_t)s->bpm_x100 * SEQ_PPQN);
        s->start_time_us = now_us() - needed_us;

        trk->event_index = 0;
        /* Advance event_index past events before new_tick */
        while (trk->event_index < pat->event_count &&
               pat->events[trk->event_index].tick < new_tick) {
            trk->event_index++;
        }

        /* Emit events at the wrap point */
        emit_events_in_range(rt, s, trk, new_tick, new_tick + 1);
    } else {
        emit_events_in_range(rt, s, trk, s->current_tick, new_tick);
    }

    s->current_tick = new_tick;

    /* Auto-stop if not looping and past end */
    if (!s->loop_enabled && effective_end > 0 && new_tick >= effective_end) {
        s->playing = false;
        if (s->timer) {
            actor_cancel_timer(rt, s->timer);
            s->timer = 0;
        }
    }
}

/* ── Message handlers ────────────────────────────────────────────── */

static void handle_load_pattern(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_load_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const seq_load_payload_t *req = (const seq_load_payload_t *)msg->payload;

    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    if (req->slot > 1) {
        reply_error(rt, msg->source, "slot must be 0 or 1");
        return;
    }
    if (req->event_count > SEQ_MAX_EVENTS) {
        reply_error(rt, msg->source, "too many events");
        return;
    }

    size_t expected = sizeof(seq_load_payload_t) +
                      (size_t)req->event_count * sizeof(seq_event_t);
    if (msg->payload_size < expected) {
        reply_error(rt, msg->source, "payload too small for events");
        return;
    }

    seq_track_t *trk = &s->tracks[req->track];
    seq_pattern_t *pat = &trk->slots[req->slot];

    /* Free old pattern */
    free(pat->events);
    pat->events = NULL;
    pat->event_count = 0;

    if (req->event_count == 0) {
        pat->length = req->length;
        memcpy(pat->name, req->name, sizeof(pat->name));
        reply_ok(rt, msg->source);
        return;
    }

    /* Expand note-offs */
    int max_expanded = SEQ_MAX_EXPANDED;
    seq_event_t *expanded = malloc((size_t)max_expanded * sizeof(seq_event_t));
    if (!expanded) {
        reply_error(rt, msg->source, "out of memory");
        return;
    }

    int count = expand_note_offs(req->events, req->event_count,
                                  req->length, expanded, max_expanded);
    if (count < 0) {
        free(expanded);
        reply_error(rt, msg->source, "too many events after expansion");
        return;
    }

    /* Shrink to fit */
    seq_event_t *shrunk = realloc(expanded, (size_t)count * sizeof(seq_event_t));
    pat->events = shrunk ? shrunk : expanded;
    pat->event_count = (uint16_t)count;
    pat->length = req->length;
    memcpy(pat->name, req->name, sizeof(pat->name));

    /* Reset playback index if this is the active slot */
    if (trk->active_slot == req->slot)
        trk->event_index = 0;

    reply_ok(rt, msg->source);
}

static void handle_start(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (s->playing && !s->paused) {
        /* Already playing — restart from beginning */
        s->current_tick = s->loop_start;
        s->start_time_us = now_us();
        for (int i = 0; i < SEQ_MAX_TRACKS; i++)
            s->tracks[i].event_index = 0;
        reply_ok(rt, msg->source);
        return;
    }

    s->playing = true;
    s->paused = false;
    s->current_tick = s->loop_start;
    s->start_time_us = now_us();

    for (int i = 0; i < SEQ_MAX_TRACKS; i++)
        s->tracks[i].event_index = 0;

    if (!s->timer)
        s->timer = actor_set_timer(rt, SEQ_TICK_MS, true);

    reply_ok(rt, msg->source);
}

static void handle_stop(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (s->playing)
        send_all_notes_off(rt, s->midi_id);

    s->playing = false;
    s->paused = false;
    s->current_tick = 0;

    if (s->timer) {
        actor_cancel_timer(rt, s->timer);
        s->timer = 0;
    }

    for (int i = 0; i < SEQ_MAX_TRACKS; i++)
        s->tracks[i].event_index = 0;

    reply_ok(rt, msg->source);
}

static void handle_pause(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->playing) {
        reply_error(rt, msg->source, "not playing");
        return;
    }

    if (s->paused) {
        /* Resume */
        uint64_t pause_duration = now_us() - s->pause_time_us;
        s->start_time_us += pause_duration;
        s->paused = false;
    } else {
        /* Pause */
        s->pause_time_us = now_us();
        s->paused = true;
    }

    reply_ok(rt, msg->source);
}

static void handle_set_tempo(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_tempo_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const seq_tempo_payload_t *req = (const seq_tempo_payload_t *)msg->payload;
    if (req->bpm_x100 == 0) {
        reply_error(rt, msg->source, "BPM must be > 0");
        return;
    }

    if (s->playing && !s->paused) {
        /* Re-anchor: calculate what tick we're at now, then set new tempo */
        uint64_t elapsed = now_us() - s->start_time_us;
        s->current_tick = calc_tick(elapsed, s->bpm_x100);

        /* Recalculate start_time so current_tick stays the same with new BPM */
        uint64_t needed_us = (uint64_t)s->current_tick * 6000000000ULL /
                             ((uint64_t)req->bpm_x100 * SEQ_PPQN);
        s->start_time_us = now_us() - needed_us;
    }

    s->bpm_x100 = req->bpm_x100;
    reply_ok(rt, msg->source);
}

static void handle_set_position(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_position_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const seq_position_payload_t *req =
        (const seq_position_payload_t *)msg->payload;

    s->current_tick = req->tick;

    /* Re-anchor start time */
    uint64_t needed_us = (uint64_t)req->tick * 6000000000ULL /
                         ((uint64_t)s->bpm_x100 * SEQ_PPQN);
    s->start_time_us = now_us() - needed_us;

    /* Reset event indices to match new position */
    for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
        seq_track_t *trk = &s->tracks[i];
        seq_pattern_t *pat = &trk->slots[trk->active_slot];
        trk->event_index = 0;
        if (pat->events) {
            while (trk->event_index < pat->event_count &&
                   pat->events[trk->event_index].tick < req->tick) {
                trk->event_index++;
            }
        }
    }

    reply_ok(rt, msg->source);
}

static void handle_set_loop(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_loop_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const seq_loop_payload_t *req = (const seq_loop_payload_t *)msg->payload;
    s->loop_enabled = req->enabled;
    s->loop_start = req->start_tick;
    s->loop_end = req->end_tick;

    reply_ok(rt, msg->source);
}

static void handle_status_request(seq_state_t *s, runtime_t *rt,
                                  message_t *msg) {
    seq_status_payload_t status;
    memset(&status, 0, sizeof(status));
    status.playing = s->playing;
    status.paused = s->paused;
    status.looping = s->loop_enabled;
    status.bpm_x100 = s->bpm_x100;
    status.current_tick = s->current_tick;

    seq_pattern_t *pat = &s->tracks[0].slots[s->tracks[0].active_slot];
    status.pattern_length = pat->length;
    status.event_count = pat->event_count;

    actor_send(rt, msg->source, MSG_SEQ_STATUS, &status, sizeof(status));
}

/* ── Actor behavior ──────────────────────────────────────────────── */

static bool seq_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    seq_state_t *s = state;

    if (msg->type == SEQ_BOOTSTRAP)
        return true;

    if (msg->type == MSG_TIMER) {
        handle_tick(rt, s);
        return true;
    }

    /* Ignore MIDI OK/ERROR replies from our sends */
    if (msg->type == MSG_MIDI_OK || msg->type == MSG_MIDI_ERROR)
        return true;

    switch (msg->type) {
    case MSG_SEQ_LOAD_PATTERN: handle_load_pattern(s, rt, msg);   break;
    case MSG_SEQ_START:        handle_start(s, rt, msg);          break;
    case MSG_SEQ_STOP:         handle_stop(s, rt, msg);           break;
    case MSG_SEQ_PAUSE:        handle_pause(s, rt, msg);          break;
    case MSG_SEQ_SET_TEMPO:    handle_set_tempo(s, rt, msg);      break;
    case MSG_SEQ_SET_POSITION: handle_set_position(s, rt, msg);   break;
    case MSG_SEQ_SET_LOOP:     handle_set_loop(s, rt, msg);       break;
    case MSG_SEQ_STATUS:       handle_status_request(s, rt, msg); break;
    default: break;
    }

    return true;
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

static void seq_state_free(void *state) {
    seq_state_t *s = state;
    for (int t = 0; t < SEQ_MAX_TRACKS; t++) {
        for (int sl = 0; sl < 2; sl++)
            free(s->tracks[t].slots[sl].events);
    }
    free(s);
}

/* ── Init ────────────────────────────────────────────────────────── */

actor_id_t sequencer_init(runtime_t *rt) {
    actor_id_t midi_id = actor_lookup(rt, "/node/hardware/midi");
    if (midi_id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    seq_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ACTOR_ID_INVALID;

    s->midi_id = midi_id;
    s->bpm_x100 = 12000;  /* 120 BPM default */
    s->loop_enabled = true;

    actor_id_t id = actor_spawn(rt, seq_behavior, s, seq_state_free, 64);
    if (id == ACTOR_ID_INVALID) {
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/sys/sequencer", id);
    actor_send(rt, id, SEQ_BOOTSTRAP, NULL, 0);

    return id;
}
