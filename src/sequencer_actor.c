/* sequencer_actor.c — Pattern-based MIDI sequencer with timer-driven playback.
 *
 * Multi-track playback engine (Phase 2): 480 PPQN, wall-clock tick
 * calculation, note-off pre-expansion, loop support, per-track wrapping,
 * mute/solo, double-buffer slot switching.
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

/* ── Active note tracking ────────────────────────────────────────── */

/* 16 channels × 128 notes = 256 bytes per track.
 * Bit layout: bits[channel * 16 + note / 8] & (1 << (note % 8)) */
typedef struct {
    uint8_t bits[256];
} active_notes_t;

static inline void an_set(active_notes_t *an, uint8_t ch, uint8_t note) {
    an->bits[ch * 16 + note / 8] |= (uint8_t)(1 << (note % 8));
}

static inline void an_clear(active_notes_t *an, uint8_t ch, uint8_t note) {
    an->bits[ch * 16 + note / 8] &= (uint8_t)~(1 << (note % 8));
}

static inline void an_clear_all(active_notes_t *an) {
    memset(an->bits, 0, sizeof(an->bits));
}

/* ── Track ───────────────────────────────────────────────────────── */

typedef struct {
    seq_pattern_t  slots[2];
    active_notes_t active_notes; /* notes currently sounding */
    seq_fx_chain_t fx_chain;     /* per-track effect chain */
    uint32_t       humanize_seed; /* xorshift32 PRNG state */
    uint8_t        active_slot;
    uint16_t       event_index;  /* current playback position */
    bool           muted;
    bool           soloed;
    bool           pending_switch;
    uint8_t        pending_slot; /* slot to switch to at boundary */
} seq_track_t;

/* ── Actor state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t   midi_id;       /* MIDI actor for output */
    seq_track_t  tracks[SEQ_MAX_TRACKS];

    bool         playing;
    bool         paused;
    uint8_t      solo_mask;     /* bitmask of soloed tracks */

    /* Tempo and timing */
    uint32_t     bpm_x100;      /* BPM × 100 (e.g. 12000 = 120 BPM) */
    tick_t       current_tick;
    tick_t       prev_tick;     /* tick at last handle_tick call */
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

/* ── Per-track effects ────────────────────────────────────────────── */

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void apply_single_fx(const seq_fx_t *fx, seq_event_t *ev,
                             uint32_t *humanize_seed) {
    switch (fx->type) {
    case SEQ_FX_TRANSPOSE:
        if (ev->type == SEQ_EVENT_NOTE) {
            ev->data.note.pitch = pitch_transpose(
                ev->data.note.pitch,
                fx->params.transpose.semitones,
                fx->params.transpose.cents);
        } else if (ev->type == SEQ_EVENT_NOTE_OFF) {
            ev->data.note_off.pitch = pitch_transpose(
                ev->data.note_off.pitch,
                fx->params.transpose.semitones,
                fx->params.transpose.cents);
        }
        break;

    case SEQ_FX_VELOCITY_SCALE:
        if (ev->type == SEQ_EVENT_NOTE) {
            int vel = (int)ev->data.note.velocity *
                      (int)fx->params.velocity_scale.scale_pct / 100;
            if (vel < 1) vel = 1;
            if (vel > 127) vel = 127;
            ev->data.note.velocity = (uint8_t)vel;
        }
        break;

    case SEQ_FX_HUMANIZE: {
        if (ev->type == SEQ_EVENT_NOTE) {
            int range = (int)fx->params.humanize.velocity_range;
            if (range > 0) {
                uint32_t r = xorshift32(humanize_seed);
                int offset = (int)(r % (uint32_t)(2 * range + 1)) - range;
                int vel = (int)ev->data.note.velocity + offset;
                if (vel < 1) vel = 1;
                if (vel > 127) vel = 127;
                ev->data.note.velocity = (uint8_t)vel;
            }
        }
        break;
    }

    case SEQ_FX_CC_SCALE:
        if (ev->type == SEQ_EVENT_CONTROL &&
            ev->data.control.cc_number == fx->params.cc_scale.cc_number) {
            /* Map 16-bit value (0–65535) to min_val..max_val 7-bit range,
             * then store back as 16-bit (shifted <<9 for 7-bit compat) */
            uint8_t val7 = (uint8_t)(ev->data.control.value >> 9);
            int min_v = (int)fx->params.cc_scale.min_val;
            int max_v = (int)fx->params.cc_scale.max_val;
            int mapped = min_v + (int)val7 * (max_v - min_v) / 127;
            if (mapped < 0) mapped = 0;
            if (mapped > 127) mapped = 127;
            ev->data.control.value = (uint16_t)((uint16_t)mapped << 9);
        }
        break;

    default:
        break;
    }
}

static void apply_fx_chain(seq_fx_chain_t *chain, seq_event_t *ev,
                            uint32_t *humanize_seed) {
    if (ev->flags & SEQ_FLAG_BYPASS_FX) return;
    for (int i = 0; i < chain->count; i++) {
        seq_fx_t *fx = &chain->effects[i];
        if (!fx->enabled) continue;
        apply_single_fx(fx, ev, humanize_seed);
    }
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
                       const seq_event_t *ev, active_notes_t *an) {
    if (ev->flags & SEQ_FLAG_MUTED) return;

    switch (ev->type) {
    case SEQ_EVENT_NOTE: {
        uint8_t note = pitch_to_midi_note(ev->data.note.pitch);
        uint8_t vel  = ev->data.note.velocity;
        uint8_t ch   = ev->data.note.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0x90 | ch), note, vel);
        an_set(an, ch, note);
        break;
    }
    case SEQ_EVENT_NOTE_OFF: {
        uint8_t note = pitch_to_midi_note(ev->data.note_off.pitch);
        uint8_t vel  = ev->data.note_off.velocity;
        uint8_t ch   = ev->data.note_off.channel & 0x0F;
        send_midi(rt, midi_id, (uint8_t)(0x80 | ch), note, vel);
        an_clear(an, ch, note);
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

/* Send Note Off for every active note in the bitmap, then clear it */
static void kill_active_notes(runtime_t *rt, actor_id_t midi_id,
                              active_notes_t *an) {
    for (int ch = 0; ch < 16; ch++) {
        for (int byte_idx = 0; byte_idx < 16; byte_idx++) {
            uint8_t b = an->bits[ch * 16 + byte_idx];
            if (b == 0) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (b & (1 << bit)) {
                    uint8_t note = (uint8_t)(byte_idx * 8 + bit);
                    send_midi(rt, midi_id, (uint8_t)(0x80 | ch), note, 64);
                }
            }
        }
    }
    an_clear_all(an);
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
            seq_event_t copy = *ev;
            apply_fx_chain(&trk->fx_chain, &copy, &trk->humanize_seed);
            emit_event(rt, s->midi_id, &copy, &trk->active_notes);
        }
        trk->event_index++;
    }
}

/* Check if a track should produce audio given mute/solo state */
static bool track_is_audible(const seq_state_t *s, int track_idx) {
    const seq_track_t *trk = &s->tracks[track_idx];
    if (trk->muted) return false;
    if (s->solo_mask != 0 && !trk->soloed) return false;
    return true;
}

/* Get the max pattern length across all tracks that have patterns */
static tick_t max_pattern_length(const seq_state_t *s) {
    tick_t max_len = 0;
    for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
        const seq_pattern_t *pat = &s->tracks[i].slots[s->tracks[i].active_slot];
        if (pat->events && pat->event_count > 0 && pat->length > max_len)
            max_len = pat->length;
    }
    return max_len;
}

/* Seek a track's event_index to match a local tick position */
static void seek_track(seq_track_t *trk, tick_t local_tick) {
    seq_pattern_t *pat = &trk->slots[trk->active_slot];
    trk->event_index = 0;
    if (pat->events) {
        while (trk->event_index < pat->event_count &&
               pat->events[trk->event_index].tick < local_tick) {
            trk->event_index++;
        }
    }
}

/* Process one track for a tick interval [prev_tick, new_tick).
 * Non-looping: raw ticks clamped to pattern length (no wrapping).
 * Looping: per-track modulo wrapping for polyrhythm support. */
static void process_track_tick(runtime_t *rt, seq_state_t *s,
                               int track_idx,
                               tick_t prev_tick, tick_t new_tick) {
    seq_track_t *trk = &s->tracks[track_idx];
    seq_pattern_t *pat = &trk->slots[trk->active_slot];
    if (!pat->events || pat->event_count == 0 || pat->length == 0) return;
    if (!track_is_audible(s, track_idx)) return;

    tick_t len = pat->length;

    if (!s->loop_enabled) {
        /* Non-looping: emit in [prev_tick, min(new_tick, len)) — no wrapping */
        if (prev_tick >= len) return;
        tick_t capped = (new_tick > len) ? len : new_tick;
        emit_events_in_range(rt, s, trk, prev_tick, capped);
        return;
    }

    /* Looping: per-track modulo wrapping */
    tick_t local_from = prev_tick % len;
    tick_t local_to   = new_tick % len;

    if (new_tick > prev_tick && (new_tick - prev_tick) >= len) {
        /* Jumped more than one full pattern — emit everything once */
        trk->event_index = 0;
        emit_events_in_range(rt, s, trk, 0, len);
        /* Handle slot switch at boundary */
        if (trk->pending_switch) {
            kill_active_notes(rt, s->midi_id, &trk->active_notes);
            trk->active_slot = trk->pending_slot;
            trk->pending_switch = false;
            pat = &trk->slots[trk->active_slot];
        }
        seek_track(trk, local_to);
        return;
    }

    if (local_to < local_from || (new_tick > prev_tick && local_to == local_from
                                   && new_tick != prev_tick)) {
        /* Boundary crossing: pattern wrapped */
        emit_events_in_range(rt, s, trk, local_from, len);

        /* Slot switch at boundary */
        if (trk->pending_switch) {
            kill_active_notes(rt, s->midi_id, &trk->active_notes);
            trk->active_slot = trk->pending_slot;
            trk->pending_switch = false;
            pat = &trk->slots[trk->active_slot];
        }

        trk->event_index = 0;
        emit_events_in_range(rt, s, trk, 0, local_to);
    } else {
        emit_events_in_range(rt, s, trk, local_from, local_to);
    }
}

static void handle_tick(runtime_t *rt, seq_state_t *s) {
    if (!s->playing || s->paused) return;

    uint64_t elapsed = now_us() - s->start_time_us;
    tick_t new_tick = calc_tick(elapsed, s->bpm_x100);

    tick_t max_len = max_pattern_length(s);
    if (max_len == 0) return;

    tick_t effective_end = s->loop_end > 0 ? s->loop_end : max_len;

    if (s->loop_enabled && effective_end > 0 && new_tick >= effective_end) {
        /* Process all tracks up to loop end before wrapping */
        for (int i = 0; i < SEQ_MAX_TRACKS; i++)
            process_track_tick(rt, s, i, s->prev_tick, effective_end);

        /* Global wrap */
        tick_t loop_len = effective_end - s->loop_start;
        if (loop_len == 0) loop_len = 1;

        uint64_t ticks_past_end = new_tick - effective_end;
        tick_t wrapped_ticks = ticks_past_end % loop_len;
        new_tick = s->loop_start + wrapped_ticks;

        /* Re-anchor start_time_us */
        uint64_t needed_us = (uint64_t)new_tick * 6000000000ULL /
                             ((uint64_t)s->bpm_x100 * SEQ_PPQN);
        s->start_time_us = now_us() - needed_us;

        /* Re-seek all tracks to loop start and emit up to wrapped position */
        for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
            seq_track_t *trk = &s->tracks[i];
            seq_pattern_t *pat = &trk->slots[trk->active_slot];
            if (pat->events && pat->event_count > 0 && pat->length > 0) {
                tick_t local_start = s->loop_start % pat->length;
                seek_track(trk, local_start);
            }
        }

        /* Emit events from loop start to wrapped position */
        for (int i = 0; i < SEQ_MAX_TRACKS; i++)
            process_track_tick(rt, s, i, s->loop_start, new_tick + 1);
    } else {
        /* Normal tick: process all tracks */
        for (int i = 0; i < SEQ_MAX_TRACKS; i++)
            process_track_tick(rt, s, i, s->prev_tick, new_tick);
    }

    s->prev_tick = new_tick;
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
        s->prev_tick = s->loop_start;
        s->start_time_us = now_us();
        for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
            s->tracks[i].event_index = 0;
            s->tracks[i].pending_switch = false;
        }
        reply_ok(rt, msg->source);
        return;
    }

    s->playing = true;
    s->paused = false;
    s->current_tick = s->loop_start;
    s->prev_tick = s->loop_start;
    s->start_time_us = now_us();

    for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
        s->tracks[i].event_index = 0;
        s->tracks[i].pending_switch = false;
        s->tracks[i].humanize_seed = (uint32_t)(i + 1);
        an_clear_all(&s->tracks[i].active_notes);
    }

    if (!s->timer)
        s->timer = actor_set_timer(rt, SEQ_TICK_MS, true);

    reply_ok(rt, msg->source);
}

static void handle_stop(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (s->playing) {
        for (int i = 0; i < SEQ_MAX_TRACKS; i++)
            kill_active_notes(rt, s->midi_id, &s->tracks[i].active_notes);
    }

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

    /* Kill any sounding notes before jumping */
    if (s->playing) {
        for (int i = 0; i < SEQ_MAX_TRACKS; i++)
            kill_active_notes(rt, s->midi_id, &s->tracks[i].active_notes);
    }

    s->current_tick = req->tick;
    s->prev_tick = req->tick;

    /* Re-anchor start time */
    uint64_t needed_us = (uint64_t)req->tick * 6000000000ULL /
                         ((uint64_t)s->bpm_x100 * SEQ_PPQN);
    s->start_time_us = now_us() - needed_us;

    /* Reset event indices to match new position (per-track local tick) */
    for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
        seq_track_t *trk = &s->tracks[i];
        seq_pattern_t *pat = &trk->slots[trk->active_slot];
        if (pat->events && pat->event_count > 0 && pat->length > 0) {
            tick_t local = req->tick % pat->length;
            seek_track(trk, local);
        } else {
            trk->event_index = 0;
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
    status.solo_mask = s->solo_mask;

    /* Track 0 for backward compat */
    seq_pattern_t *pat0 = &s->tracks[0].slots[s->tracks[0].active_slot];
    status.pattern_length = pat0->length;
    status.event_count = pat0->event_count;

    /* Per-track status */
    uint8_t track_count = 0;
    for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
        seq_track_t *trk = &s->tracks[i];
        seq_pattern_t *pat = &trk->slots[trk->active_slot];
        status.tracks[i].pattern_length = pat->length;
        status.tracks[i].event_count = pat->event_count;
        status.tracks[i].active_slot = trk->active_slot;
        status.tracks[i].muted = trk->muted;
        status.tracks[i].soloed = trk->soloed;
        status.tracks[i].pending_switch = trk->pending_switch;
        status.tracks[i].fx_count = trk->fx_chain.count;
        if (pat->events && pat->event_count > 0)
            track_count++;
    }
    status.track_count = track_count;

    actor_send(rt, msg->source, MSG_SEQ_STATUS, &status, sizeof(status));
}

/* ── Mute / Solo / Slot switch handlers ──────────────────────────── */

static void rebuild_solo_mask(seq_state_t *s) {
    s->solo_mask = 0;
    for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
        if (s->tracks[i].soloed)
            s->solo_mask |= (uint8_t)(1 << i);
    }
}

static void handle_mute_track(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_mute_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }
    const seq_mute_payload_t *req = (const seq_mute_payload_t *)msg->payload;
    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    s->tracks[req->track].muted = req->muted;
    if (req->muted && s->playing)
        kill_active_notes(rt, s->midi_id, &s->tracks[req->track].active_notes);
    reply_ok(rt, msg->source);
}

static void handle_solo_track(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_solo_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }
    const seq_solo_payload_t *req = (const seq_solo_payload_t *)msg->payload;
    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    s->tracks[req->track].soloed = req->soloed;
    rebuild_solo_mask(s);

    /* Kill active notes on tracks that just became inaudible */
    if (s->playing) {
        for (int i = 0; i < SEQ_MAX_TRACKS; i++) {
            if (!track_is_audible(s, i))
                kill_active_notes(rt, s->midi_id, &s->tracks[i].active_notes);
        }
    }

    reply_ok(rt, msg->source);
}

static void handle_switch_slot(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_switch_slot_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }
    const seq_switch_slot_payload_t *req =
        (const seq_switch_slot_payload_t *)msg->payload;
    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    if (req->slot > 1) {
        reply_error(rt, msg->source, "slot must be 0 or 1");
        return;
    }
    seq_track_t *trk = &s->tracks[req->track];
    /* Validate target slot has a pattern */
    seq_pattern_t *pat = &trk->slots[req->slot];
    if (!pat->events || pat->event_count == 0) {
        reply_error(rt, msg->source, "target slot has no pattern");
        return;
    }
    if (trk->active_slot == req->slot) {
        /* Already on this slot, nothing to do */
        reply_ok(rt, msg->source);
        return;
    }
    trk->pending_switch = true;
    trk->pending_slot = req->slot;
    reply_ok(rt, msg->source);
}

/* ── FX handlers ─────────────────────────────────────────────────── */

static void handle_set_fx(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_set_fx_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }
    const seq_set_fx_payload_t *req = (const seq_set_fx_payload_t *)msg->payload;
    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    if (req->slot >= SEQ_MAX_FX_PER_TRACK) {
        reply_error(rt, msg->source, "slot index out of range");
        return;
    }
    seq_track_t *trk = &s->tracks[req->track];
    trk->fx_chain.effects[req->slot] = req->effect;
    /* Update count to cover this slot */
    if (req->slot >= trk->fx_chain.count)
        trk->fx_chain.count = req->slot + 1;
    reply_ok(rt, msg->source);
}

static void handle_clear_fx(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_clear_fx_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }
    const seq_clear_fx_payload_t *req =
        (const seq_clear_fx_payload_t *)msg->payload;
    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    seq_track_t *trk = &s->tracks[req->track];
    if (req->slot == 0xFF) {
        /* Clear all */
        memset(&trk->fx_chain, 0, sizeof(trk->fx_chain));
    } else {
        if (req->slot >= SEQ_MAX_FX_PER_TRACK) {
            reply_error(rt, msg->source, "slot index out of range");
            return;
        }
        memset(&trk->fx_chain.effects[req->slot], 0, sizeof(seq_fx_t));
        /* Shrink count if we cleared the last slot */
        while (trk->fx_chain.count > 0 &&
               trk->fx_chain.effects[trk->fx_chain.count - 1].type == SEQ_FX_NONE)
            trk->fx_chain.count--;
    }
    reply_ok(rt, msg->source);
}

static void handle_enable_fx(seq_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(seq_enable_fx_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }
    const seq_enable_fx_payload_t *req =
        (const seq_enable_fx_payload_t *)msg->payload;
    if (req->track >= SEQ_MAX_TRACKS) {
        reply_error(rt, msg->source, "track index out of range");
        return;
    }
    if (req->slot >= SEQ_MAX_FX_PER_TRACK) {
        reply_error(rt, msg->source, "slot index out of range");
        return;
    }
    seq_track_t *trk = &s->tracks[req->track];
    if (req->slot >= trk->fx_chain.count) {
        reply_error(rt, msg->source, "slot not populated");
        return;
    }
    trk->fx_chain.effects[req->slot].enabled = req->enabled;
    reply_ok(rt, msg->source);
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
    case MSG_SEQ_MUTE_TRACK:   handle_mute_track(s, rt, msg);    break;
    case MSG_SEQ_SOLO_TRACK:   handle_solo_track(s, rt, msg);    break;
    case MSG_SEQ_SWITCH_SLOT:  handle_switch_slot(s, rt, msg);   break;
    case MSG_SEQ_SET_FX:       handle_set_fx(s, rt, msg);        break;
    case MSG_SEQ_CLEAR_FX:     handle_clear_fx(s, rt, msg);      break;
    case MSG_SEQ_ENABLE_FX:    handle_enable_fx(s, rt, msg);     break;
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
