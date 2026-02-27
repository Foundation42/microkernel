#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/midi.h"
#include "microkernel/sequencer.h"
#include "midi_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state ────────────────────────────────────────────────── */

typedef struct {
    actor_id_t seq_id;
    actor_id_t midi_id;
    int        step;
    bool       done;
    bool       timeout;

    msg_type_t last_type;
    uint8_t    last_payload[512];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_error;
    bool       got_status;
    int        ok_count;
} seq_tester_t;

static void save_reply(seq_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload)
                          ? msg->payload_size : sizeof(s->last_payload);
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);

    if (msg->type == MSG_SEQ_OK)     { s->got_ok = true; s->ok_count++; }
    if (msg->type == MSG_SEQ_ERROR)  s->got_error = true;
    if (msg->type == MSG_SEQ_STATUS) s->got_status = true;
}

static void send_midi_config(runtime_t *rt, actor_id_t midi_id) {
    midi_config_payload_t cfg = {
        .i2c_port = 0, .i2c_addr = 0x48,
        .sda_pin = 8, .scl_pin = 9, .irq_pin = 7,
        .i2c_freq_hz = 400000
    };
    actor_send(rt, midi_id, MSG_MIDI_CONFIGURE, &cfg, sizeof(cfg));
}

/* ── test_init ───────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t midi_id = midi_actor_init(rt);
    ASSERT_NE(midi_id, ACTOR_ID_INVALID);

    actor_id_t seq_id = sequencer_init(rt);
    ASSERT_NE(seq_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/sys/sequencer"), seq_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_load_pattern ───────────────────────────────────────────── */

static bool load_pattern_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;

        /* Load 4-note C major pattern */
        seq_event_t events[4];
        events[0] = seq_note(0,            60, 100, SEQ_PPQN / 2, 0);
        events[1] = seq_note(SEQ_PPQN,     62, 100, SEQ_PPQN / 2, 0);
        events[2] = seq_note(SEQ_PPQN * 2, 64, 100, SEQ_PPQN / 2, 0);
        events[3] = seq_note(SEQ_PPQN * 3, 65, 100, SEQ_PPQN / 2, 0);

        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_TICKS_PER_BAR, "test", events, 4);
        ASSERT_NOT_NULL(p);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(4));
        free(p);
        return true;
    }

    if (s->step == 2) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_load_pattern(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, load_pattern_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_start_stop ─────────────────────────────────────────────── */

static bool start_stop_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Load minimal pattern */
        seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_TICKS_PER_BAR, "test", &ev, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* Brief play, then stop */
        actor_set_timer(rt, 20, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_STOP, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->got_ok = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_start_stop(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, start_stop_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_note_output ────────────────────────────────────────────── */

static bool note_output_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* 1 note at tick 0, short pattern so it finishes quickly */
        seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN, "note_test", &ev, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Disable looping so it stops */
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        /* Set fast tempo for quick test */
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };  /* 600 BPM */
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        /* Wait enough for the pattern to play through */
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_note_output(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, note_output_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* Check MIDI TX: should have Note On (0x90 60 100) + Note Off (0x80 60 64) */
    uint8_t txbuf[64];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* At minimum we need Note On (3 bytes) + Note Off (3 bytes) */
    ASSERT(txn >= 6);

    /* Find Note On for note 60 */
    bool found_on = false, found_off = false;
    for (int i = 0; i + 2 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0x90 && txbuf[i + 1] == 60 && txbuf[i + 2] == 100) {
            found_on = true;
            i += 3;
        } else if ((status & 0xF0) == 0x80 && txbuf[i + 1] == 60) {
            found_off = true;
            i += 3;
        } else if ((status & 0xF0) == 0xB0) {
            /* CC (all notes off on stop) — skip */
            i += 3;
        } else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
            i += 2;
        } else {
            i += 3;
        }
    }

    ASSERT(found_on);
    ASSERT(found_off);

    runtime_destroy(rt);
    return 0;
}

/* ── test_tempo ──────────────────────────────────────────────────── */

static bool tempo_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Load 2 notes at beat 0 and beat 1 */
        seq_event_t events[2];
        events[0] = seq_note(0,        60, 100, SEQ_PPQN / 4, 0);
        events[1] = seq_note(SEQ_PPQN, 62, 100, SEQ_PPQN / 4, 0);

        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 2, "tempo_test", events, 2);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(2));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        /* Very fast: 1200 BPM = 20 beats/sec, so 2 beats in 100ms */
        seq_tempo_payload_t tp = { .bpm_x100 = 120000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_tempo(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, tempo_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* At 1200 BPM with 200ms wait, both notes should have played */
    uint8_t txbuf[128];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* Should have at least: Note On 60 + Note Off 60 + Note On 62 + Note Off 62 */
    int note_on_count = 0;
    for (int i = 0; i + 2 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0x90 && txbuf[i + 2] > 0)
            note_on_count++;
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }

    ASSERT(note_on_count >= 2);

    runtime_destroy(rt);
    return 0;
}

/* ── test_loop ───────────────────────────────────────────────────── */

static bool loop_tester(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* 1 note, short pattern */
        seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 8, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN / 2, "loop_test", &ev, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Loop enabled (default, but be explicit) */
        seq_loop_payload_t lp = { .enabled = true, .start_tick = 0, .end_tick = 0 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        /* 1200 BPM: half-beat = 25ms, so 200ms = ~8 loops */
        seq_tempo_payload_t tp = { .bpm_x100 = 120000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->step = 7;
        actor_send(rt, s->seq_id, MSG_SEQ_STOP, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_loop(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, loop_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* Should have multiple Note On events due to looping */
    uint8_t txbuf[512];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    int note_on_count = 0;
    for (int i = 0; i + 2 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0x90 && txbuf[i + 1] == 60 && txbuf[i + 2] > 0)
            note_on_count++;
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }

    /* At 1200 BPM with 200ms and half-beat pattern, expect at least 2 loops */
    ASSERT(note_on_count >= 2);

    runtime_destroy(rt);
    return 0;
}

/* ── test_pause_resume ───────────────────────────────────────────── */

static bool pause_resume_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* 2 notes at beat 0 and beat 2 */
        seq_event_t events[2];
        events[0] = seq_note(0,            60, 100, SEQ_PPQN / 4, 0);
        events[1] = seq_note(SEQ_PPQN * 2, 62, 100, SEQ_PPQN / 4, 0);

        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 4, "pause_test", events, 2);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(2));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        /* 600 BPM: beat = 100ms */
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        /* After 50ms: first note should have played, pause before second */
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->step = 7;
        actor_send(rt, s->seq_id, MSG_SEQ_PAUSE, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->step = 8;
        /* Wait 200ms paused — second note should NOT play */
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 8) {
        s->step = 9;
        /* Resume */
        actor_send(rt, s->seq_id, MSG_SEQ_PAUSE, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 9) {
        s->step = 10;
        /* Wait for second note to play */
        actor_set_timer(rt, 300, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 10) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_pause_resume(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, pause_resume_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* Both notes should have played (first before pause, second after resume) */
    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    int note_on_60 = 0, note_on_62 = 0;
    for (int i = 0; i + 2 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0x90 && txbuf[i + 2] > 0) {
            if (txbuf[i + 1] == 60) note_on_60++;
            if (txbuf[i + 1] == 62) note_on_62++;
        }
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }

    ASSERT(note_on_60 >= 1);
    ASSERT(note_on_62 >= 1);

    runtime_destroy(rt);
    return 0;
}

/* ── test_seek ───────────────────────────────────────────────────── */

static bool seek_tester(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* 3 notes at beat 0, 1, 2 */
        seq_event_t events[3];
        events[0] = seq_note(0,            60, 100, SEQ_PPQN / 4, 0);
        events[1] = seq_note(SEQ_PPQN,     62, 100, SEQ_PPQN / 4, 0);
        events[2] = seq_note(SEQ_PPQN * 2, 64, 100, SEQ_PPQN / 4, 0);

        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 3, "seek_test", events, 3);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(3));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* 1200 BPM: beat = 50ms */
        seq_tempo_payload_t tp = { .bpm_x100 = 120000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        /* Start first, then seek (start resets position) */
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        midi_mock_clear_tx();
        /* Now seek past the first 2 notes (to beat 2) */
        seq_position_payload_t pos = { .tick = SEQ_PPQN * 2 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_POSITION, &pos, sizeof(pos));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 7) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_seek(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, seek_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* Only note 64 (beat 2) should have played; 60 and 62 skipped */
    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    bool found_60 = false, found_62 = false, found_64 = false;
    for (int i = 0; i + 2 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0x90 && txbuf[i + 2] > 0) {
            if (txbuf[i + 1] == 60) found_60 = true;
            if (txbuf[i + 1] == 62) found_62 = true;
            if (txbuf[i + 1] == 64) found_64 = true;
        }
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }

    ASSERT(!found_60);
    ASSERT(!found_62);
    ASSERT(found_64);

    runtime_destroy(rt);
    return 0;
}

/* ── test_note_expansion ─────────────────────────────────────────── */

static bool expansion_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* 1 note at tick 0, duration 480 ticks → should generate Note Off */
        seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 2, "expansion_test", &ev, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        /* 1200 BPM: beat = 50ms */
        seq_tempo_payload_t tp = { .bpm_x100 = 120000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        /* Wait enough for 2 beats at 1200 BPM (100ms + margin) */
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_note_expansion(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, expansion_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    bool found_on = false, found_off = false;
    for (int i = 0; i + 2 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0x90 && txbuf[i + 1] == 60 && txbuf[i + 2] > 0)
            found_on = true;
        if ((status & 0xF0) == 0x80 && txbuf[i + 1] == 60)
            found_off = true;
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }

    ASSERT(found_on);
    ASSERT(found_off);

    runtime_destroy(rt);
    return 0;
}

/* ── test_cc_and_program ─────────────────────────────────────────── */

static bool cc_pgm_tester(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)self;
    seq_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* CC + Program Change events at tick 0 */
        seq_event_t events[2];
        events[0] = seq_cc(0, 7, 100, 0);      /* CC#7 (volume) = 100 */
        events[1] = seq_program(0, 42, 0);      /* Program Change 42 */

        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN, "cc_pgm_test", events, 2);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(2));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_cc_and_program(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, cc_pgm_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[128];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    bool found_cc = false, found_pgm = false;
    for (int i = 0; i + 1 < txn; ) {
        uint8_t status = txbuf[i];
        if ((status & 0xF0) == 0xB0 && i + 2 < txn) {
            if (txbuf[i + 1] == 7 && txbuf[i + 2] == 100)
                found_cc = true;
            i += 3;
        } else if ((status & 0xF0) == 0xC0) {
            if (txbuf[i + 1] == 42)
                found_pgm = true;
            i += 2;
        } else if ((status & 0xF0) == 0xD0) {
            i += 2;
        } else {
            i += 3;
        }
    }

    ASSERT(found_cc);
    ASSERT(found_pgm);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_sequencer:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_load_pattern);
    RUN_TEST(test_start_stop);
    RUN_TEST(test_note_output);
    RUN_TEST(test_tempo);
    RUN_TEST(test_loop);
    RUN_TEST(test_pause_resume);
    RUN_TEST(test_seek);
    RUN_TEST(test_note_expansion);
    RUN_TEST(test_cc_and_program);

    TEST_REPORT();
}
