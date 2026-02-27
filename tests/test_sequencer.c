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

/* ══════════════════════════════════════════════════════════════════
 *  Phase 29.2 — Multi-track tests
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: count Note On events for a given MIDI note in TX buffer */
static int count_note_ons(const uint8_t *buf, int len, uint8_t note) {
    int count = 0;
    for (int i = 0; i + 2 < len; ) {
        uint8_t status = buf[i];
        if ((status & 0xF0) == 0x90 && buf[i + 1] == note && buf[i + 2] > 0)
            count++;
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }
    return count;
}

/* Helper: check if any Note On for a given note exists */
static bool has_note_on(const uint8_t *buf, int len, uint8_t note) {
    return count_note_ons(buf, len, note) > 0;
}

/* Helper: check if any Note Off (0x8n) for a given note exists */
static bool has_note_off(const uint8_t *buf, int len, uint8_t note) {
    for (int i = 0; i + 2 < len; ) {
        uint8_t status = buf[i];
        if ((status & 0xF0) == 0x80 && buf[i + 1] == note)
            return true;
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }
    return false;
}

/* ── test_multi_track_load ───────────────────────────────────────── */

static bool mt_load_tester(runtime_t *rt, actor_t *self,
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
        /* Load track 0, slot 0 */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 2, 0);
        seq_load_payload_t *p0 = seq_build_load_payload(
            0, 0, SEQ_TICKS_PER_BAR, "track0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p0, seq_load_payload_size(1));
        free(p0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Load track 1, slot 0 */
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN / 2, 1);
        seq_load_payload_t *p1 = seq_build_load_payload(
            1, 0, SEQ_TICKS_PER_BAR, "track1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p1, seq_load_payload_size(1));
        free(p1);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->ok_count = 2;  /* Both loaded OK */
        s->got_ok = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_multi_track_load(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, mt_load_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_multi_track_playback ───────────────────────────────────── */

static bool mt_play_tester(runtime_t *rt, actor_t *self,
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
        /* Track 0: C4 (60) on ch 0 */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN, "t0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Track 1: E4 (64) on ch 1 */
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN / 4, 1);
        seq_load_payload_t *p = seq_build_load_payload(
            1, 0, SEQ_PPQN, "t1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        midi_mock_clear_tx();
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
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

static int test_multi_track_playback(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, mt_play_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* Both tracks should have played: note 60 (ch0) and note 64 (ch1) */
    ASSERT(has_note_on(txbuf, txn, 60));
    ASSERT(has_note_on(txbuf, txn, 64));

    runtime_destroy(rt);
    return 0;
}

/* ── test_polyrhythm ─────────────────────────────────────────────── */

static bool polyrhythm_tester(runtime_t *rt, actor_t *self,
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
        /* Track 0: 2-beat pattern (note 60), loops 3 times in 6 beats */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 2, "2beat", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Track 1: 3-beat pattern (note 64), loops 2 times in 6 beats */
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN / 4, 1);
        seq_load_payload_t *p = seq_build_load_payload(
            1, 0, SEQ_PPQN * 3, "3beat", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* Loop enabled, end at 6 beats = LCM(2,3) */
        seq_loop_payload_t lp = { .enabled = true, .start_tick = 0,
                                  .end_tick = SEQ_PPQN * 6 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        midi_mock_clear_tx();
        /* 1200 BPM: beat = 50ms, 6 beats = 300ms. Run for 400ms = >1 full cycle */
        seq_tempo_payload_t tp = { .bpm_x100 = 120000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        actor_set_timer(rt, 400, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 7) {
        s->step = 8;
        actor_send(rt, s->seq_id, MSG_SEQ_STOP, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 8) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_polyrhythm(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, polyrhythm_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[512];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* 2-beat pattern loops 3 times in 6 beats → note 60 should fire 3+ times
     * 3-beat pattern loops 2 times in 6 beats → note 64 should fire 2+ times */
    int n60 = count_note_ons(txbuf, txn, 60);
    int n64 = count_note_ons(txbuf, txn, 64);

    ASSERT(n60 >= 3);
    ASSERT(n64 >= 2);
    /* Polyrhythm: 2-beat fires more often than 3-beat */
    ASSERT(n60 > n64);

    runtime_destroy(rt);
    return 0;
}

/* ── test_track_mute ─────────────────────────────────────────────── */

static bool mute_tester(runtime_t *rt, actor_t *self,
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
        /* Track 0: note 60 */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN, "t0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Track 1: note 64 */
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN / 4, 1);
        seq_load_payload_t *p = seq_build_load_payload(
            1, 0, SEQ_PPQN, "t1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* Mute track 1 */
        seq_mute_payload_t mp = { .track = 1, .muted = true };
        actor_send(rt, s->seq_id, MSG_SEQ_MUTE_TRACK, &mp, sizeof(mp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        midi_mock_clear_tx();
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->step = 8;
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 8) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_track_mute(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, mute_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* Track 0 should play, track 1 muted → no note 64 */
    ASSERT(has_note_on(txbuf, txn, 60));
    ASSERT(!has_note_on(txbuf, txn, 64));

    runtime_destroy(rt);
    return 0;
}

/* ── test_track_solo ─────────────────────────────────────────────── */

static bool solo_tester(runtime_t *rt, actor_t *self,
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
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN, "t0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN / 4, 1);
        seq_load_payload_t *p = seq_build_load_payload(
            1, 0, SEQ_PPQN, "t1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* Solo track 1 → only track 1 should play */
        seq_solo_payload_t sp = { .track = 1, .soloed = true };
        actor_send(rt, s->seq_id, MSG_SEQ_SOLO_TRACK, &sp, sizeof(sp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        midi_mock_clear_tx();
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->step = 8;
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 8) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_track_solo(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, solo_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* Only soloed track 1 (note 64) should play, not track 0 (note 60) */
    ASSERT(!has_note_on(txbuf, txn, 60));
    ASSERT(has_note_on(txbuf, txn, 64));

    runtime_destroy(rt);
    return 0;
}

/* ── test_double_buffer_load ─────────────────────────────────────── */

static bool dbl_buf_tester(runtime_t *rt, actor_t *self,
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
        /* Load slot 0 on track 0 */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 2, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_TICKS_PER_BAR, "slot0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Load slot 1 on track 0 */
        seq_event_t ev1 = seq_note(0, 72, 100, SEQ_PPQN / 2, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 1, SEQ_TICKS_PER_BAR, "slot1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->got_ok = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_double_buffer_load(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, dbl_buf_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_slot_switch ────────────────────────────────────────────── */

static bool slot_switch_tester(runtime_t *rt, actor_t *self,
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
        /* Slot 0: note 60, short pattern (half beat) */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 8, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN / 2, "slot0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Slot 1: note 72, same short pattern */
        seq_event_t ev1 = seq_note(0, 72, 100, SEQ_PPQN / 8, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 1, SEQ_PPQN / 2, "slot1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
        /* Loop enabled, 1200 BPM: half beat = 25ms */
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
        /* Queue switch to slot 1 AFTER start (start clears pending_switch) */
        seq_switch_slot_payload_t sw = { .track = 0, .slot = 1 };
        actor_send(rt, s->seq_id, MSG_SEQ_SWITCH_SLOT, &sw, sizeof(sw));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        /* Wait for boundary crossing to trigger switch */
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 7) {
        s->step = 8;
        actor_send(rt, s->seq_id, MSG_SEQ_STOP, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 8) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_slot_switch(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, slot_switch_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[512];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* After boundary switch, note 72 (slot 1) should appear in output */
    ASSERT(has_note_on(txbuf, txn, 72));

    runtime_destroy(rt);
    return 0;
}

/* ── test_mute_unmute_toggle ─────────────────────────────────────── */

static bool mute_toggle_tester(runtime_t *rt, actor_t *self,
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
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 8, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN / 2, "t0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Mute track 0 */
        seq_mute_payload_t mp = { .track = 0, .muted = true };
        actor_send(rt, s->seq_id, MSG_SEQ_MUTE_TRACK, &mp, sizeof(mp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        midi_mock_clear_tx();
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
        /* Play muted for 100ms → no notes */
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->step = 7;
        /* Verify: no notes while muted */
        uint8_t txbuf[128];
        int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
        if (has_note_on(txbuf, txn, 60)) {
            /* Failed — note played while muted */
            s->got_error = true;
        }
        midi_mock_clear_tx();
        /* Unmute */
        seq_mute_payload_t mp = { .track = 0, .muted = false };
        actor_send(rt, s->seq_id, MSG_SEQ_MUTE_TRACK, &mp, sizeof(mp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->step = 8;
        /* Play unmuted for 100ms → notes should appear */
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 8) {
        s->step = 9;
        actor_send(rt, s->seq_id, MSG_SEQ_STOP, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 9) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_mute_unmute_toggle(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, mute_toggle_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* No notes while muted (got_error check in behavior) */
    ASSERT(!ts.got_error);

    /* Notes should have played after unmute */
    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    ASSERT(has_note_on(txbuf, txn, 60));

    runtime_destroy(rt);
    return 0;
}

/* ── test_solo_mask_cleared ──────────────────────────────────────── */

static bool solo_clear_tester(runtime_t *rt, actor_t *self,
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
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN, "t0", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN / 4, 1);
        seq_load_payload_t *p = seq_build_load_payload(
            1, 0, SEQ_PPQN, "t1", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* Solo track 1 */
        seq_solo_payload_t sp = { .track = 1, .soloed = true };
        actor_send(rt, s->seq_id, MSG_SEQ_SOLO_TRACK, &sp, sizeof(sp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        /* Unsolo track 1 → both tracks should play */
        seq_solo_payload_t sp = { .track = 1, .soloed = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SOLO_TRACK, &sp, sizeof(sp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        midi_mock_clear_tx();
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->step = 8;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 8) {
        s->step = 9;
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 9) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_solo_mask_cleared(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, solo_clear_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* After unsolo, both tracks should play */
    ASSERT(has_note_on(txbuf, txn, 60));
    ASSERT(has_note_on(txbuf, txn, 64));

    runtime_destroy(rt);
    return 0;
}

/* ── test_empty_track_skip ───────────────────────────────────────── */

static bool empty_skip_tester(runtime_t *rt, actor_t *self,
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
        /* Load ONLY on track 3, leaving tracks 0-2 and 4-7 empty */
        seq_event_t ev = seq_note(0, 67, 100, SEQ_PPQN / 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            3, 0, SEQ_PPQN, "t3", &ev, 1);
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

static int test_empty_track_skip(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, empty_skip_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));

    /* Track 3 note 67 should play; no crash from empty tracks */
    ASSERT(has_note_on(txbuf, txn, 67));

    runtime_destroy(rt);
    return 0;
}

/* ── test_solo_kills_hanging_notes ────────────────────────────────── */
/*
 * Track 0: long note C4 (8 beats).  Track 1: short note E4 (1 beat).
 * Start playback, wait for both notes to sound, then solo track 1.
 * Track 0's C4 should get a Note Off (0x80 nn) — no hanging note.
 */
static bool solo_kill_tester(runtime_t *rt, actor_t *self,
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
        /* Track 0: C4, 4-beat note in 8-beat pattern (note-off at tick 1920,
         * well before pattern end so it doesn't wrap to tick 0) */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN * 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 8, "long_note", &ev0, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 2) {
        s->step = 3;
        /* Track 1: E4, 1-beat note in 8-beat pattern */
        seq_event_t ev1 = seq_note(0, 64, 100, SEQ_PPQN, 1);
        seq_load_payload_t *p = seq_build_load_payload(
            1, 0, SEQ_PPQN * 8, "short_note", &ev1, 1);
        actor_send(rt, s->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(1));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 3) {
        s->step = 4;
        /* Disable loop so pattern doesn't wrap and re-trigger */
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, s->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 4) {
        s->step = 5;
        /* Fast tempo so notes fire quickly */
        seq_tempo_payload_t tp = { .bpm_x100 = 60000 }; /* 600 BPM */
        actor_send(rt, s->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 5) {
        s->step = 6;
        actor_send(rt, s->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 6) {
        s->step = 7;
        /* Wait 50ms for both notes to sound */
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 7) {
        s->step = 8;
        /* Clear TX buffer, then solo track 1 — should kill track 0's C4 */
        midi_mock_clear_tx();
        seq_solo_payload_t sp = { .track = 1, .soloed = true };
        actor_send(rt, s->seq_id, MSG_SEQ_SOLO_TRACK, &sp, sizeof(sp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 8) {
        s->step = 9;
        /* Give a tick for the note-off to be sent */
        actor_set_timer(rt, 20, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 9) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_solo_kills_hanging_notes(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, solo_kill_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);

    /* After solo, TX buffer should contain Note Off for C4 (note 60) */
    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    ASSERT(has_note_off(txbuf, txn, 60));

    /* Track 1's E4 should NOT get a Note Off from the solo action
     * (it's the soloed track, its notes should remain) */

    runtime_destroy(rt);
    return 0;
}

/* ── test_mute_kills_hanging_notes ───────────────────────────────── */

static bool mute_kill_tester(runtime_t *rt, actor_t *self,
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
        /* Track 0: C4, 4-beat note in 8-beat pattern */
        seq_event_t ev0 = seq_note(0, 60, 100, SEQ_PPQN * 4, 0);
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 8, "long_note", &ev0, 1);
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
        /* Wait for note to sound */
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 6) {
        s->step = 7;
        /* Clear TX, then mute track 0 — should kill C4 */
        midi_mock_clear_tx();
        seq_mute_payload_t mp = { .track = 0, .muted = true };
        actor_send(rt, s->seq_id, MSG_SEQ_MUTE_TRACK, &mp, sizeof(mp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && s->step == 7) {
        s->step = 8;
        actor_set_timer(rt, 20, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 8) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_mute_kills_hanging_notes(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    seq_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.seq_id = seq_id;
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, mute_kill_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);

    /* After mute, TX buffer should contain Note Off for C4 */
    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    ASSERT(has_note_off(txbuf, txn, 60));

    runtime_destroy(rt);
    return 0;
}

/* ── FX helper: get velocity of first Note On for a given note ──── */

static int get_note_on_velocity(const uint8_t *buf, int len, uint8_t note) {
    for (int i = 0; i + 2 < len; ) {
        uint8_t status = buf[i];
        if ((status & 0xF0) == 0x90 && buf[i + 1] == note && buf[i + 2] > 0)
            return buf[i + 2];
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }
    return -1;
}

/* Helper: get first CC value for a given cc_number */
static int get_cc_value(const uint8_t *buf, int len, uint8_t cc) {
    for (int i = 0; i + 2 < len; ) {
        uint8_t status = buf[i];
        if ((status & 0xF0) == 0xB0 && buf[i + 1] == cc)
            return buf[i + 2];
        if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
            i += 2;
        else
            i += 3;
    }
    return -1;
}

/* ── FX tester behavior: configures MIDI, loads pattern, sets FX,
 *    plays at fast tempo, waits, stops ──────────────────────────── */

/*
 * FX tester state extension: stores the FX payload to send.
 * The generic flow is:
 *   step 0: bootstrap → configure MIDI
 *   step 1: MIDI OK → load pattern on track 0
 *   step 2: SEQ OK → set FX via MSG_SEQ_SET_FX (or skip if no fx_payload)
 *   step 3: SEQ OK → disable loop
 *   step 4: SEQ OK → set fast tempo + clear TX
 *   step 5: SEQ OK → start
 *   step 6: SEQ OK → wait timer
 *   step 7: TIMER → stop
 */

typedef struct {
    seq_tester_t     base;

    /* Pattern setup */
    seq_event_t     *events;
    uint16_t         event_count;
    tick_t           pattern_length;

    /* FX to apply (up to 2 for chain tests) */
    seq_set_fx_payload_t fx[2];
    int              fx_count;

    /* Optional: enable/disable payload */
    bool             use_enable;
    seq_enable_fx_payload_t enable_pl;

    /* Optional: clear payload */
    bool             use_clear;
    seq_clear_fx_payload_t clear_pl;
} fx_tester_t;

static bool fx_generic_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    fx_tester_t *s = state;
    seq_tester_t *b = &s->base;

    if (msg->type == 1 && b->step == 0) {
        b->step = 1;
        send_midi_config(rt, b->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && b->step == 1) {
        b->step = 2;
        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, s->pattern_length, "fx_test", s->events, s->event_count);
        actor_send(rt, b->seq_id, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size(s->event_count));
        free(p);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && b->step == 2) {
        b->step = 3;
        /* Send FX payloads */
        for (int i = 0; i < s->fx_count; i++) {
            actor_send(rt, b->seq_id, MSG_SEQ_SET_FX,
                       &s->fx[i], sizeof(s->fx[i]));
        }
        if (s->fx_count == 0) {
            /* Skip FX step — proceed to disable loop */
            b->step = 3;
            seq_loop_payload_t lp = { .enabled = false };
            actor_send(rt, b->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        }
        return true;
    }

    /* After FX OK replies (one per fx sent), handle enable/clear if needed */
    if (msg->type == MSG_SEQ_OK && b->step == 3) {
        b->ok_count++;
        if (b->ok_count < s->fx_count + 1) /* +1 for load OK already counted */
            return true;

        /* All FX set; optionally send enable/disable */
        if (s->use_enable) {
            b->step = 30;
            actor_send(rt, b->seq_id, MSG_SEQ_ENABLE_FX,
                       &s->enable_pl, sizeof(s->enable_pl));
            return true;
        }
        if (s->use_clear) {
            b->step = 31;
            actor_send(rt, b->seq_id, MSG_SEQ_CLEAR_FX,
                       &s->clear_pl, sizeof(s->clear_pl));
            return true;
        }

        b->step = 4;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, b->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && (b->step == 30 || b->step == 31)) {
        b->step = 4;
        seq_loop_payload_t lp = { .enabled = false };
        actor_send(rt, b->seq_id, MSG_SEQ_SET_LOOP, &lp, sizeof(lp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && b->step == 4) {
        b->step = 5;
        midi_mock_clear_tx();
        seq_tempo_payload_t tp = { .bpm_x100 = 120000 };  /* 1200 BPM */
        actor_send(rt, b->seq_id, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        return true;
    }

    if (msg->type == MSG_SEQ_OK && b->step == 5) {
        b->step = 6;
        actor_send(rt, b->seq_id, MSG_SEQ_START, NULL, 0);
        return true;
    }

    if (msg->type == MSG_SEQ_OK && b->step == 6) {
        b->step = 7;
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && b->step == 7) {
        b->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static void fx_tester_init(fx_tester_t *s, actor_id_t seq_id,
                            actor_id_t midi_id) {
    memset(s, 0, sizeof(*s));
    s->base.seq_id = seq_id;
    s->base.midi_id = midi_id;
    s->base.ok_count = 1;  /* load pattern OK is already counted in step 2→3 */
}

/* ── test_fx_transpose ──────────────────────────────────────────── */

static int test_fx_transpose(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    /* Single C4 note */
    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Transpose +7 semitones */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_TRANSPOSE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.transpose.semitones = 7;
    ts.fx[0].effect.params.transpose.cents = 0;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    /* Should have note 67 (G4), not 60 (C4) */
    ASSERT(has_note_on(txbuf, txn, 67));
    ASSERT(!has_note_on(txbuf, txn, 60));
    /* Note Off should also be 67 */
    ASSERT(has_note_off(txbuf, txn, 67));

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_velocity_scale ─────────────────────────────────────── */

static int test_fx_velocity_scale(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Scale velocity to 50% */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_VELOCITY_SCALE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.velocity_scale.scale_pct = 50;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    int vel = get_note_on_velocity(txbuf, txn, 60);
    ASSERT(vel == 50);  /* 100 * 50/100 = 50 */

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_humanize ───────────────────────────────────────────── */

static int test_fx_humanize(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Humanize with range=20 */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_HUMANIZE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.humanize.velocity_range = 20;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    int vel = get_note_on_velocity(txbuf, txn, 60);
    ASSERT(vel >= 0);          /* note was played */
    ASSERT(vel >= 80);         /* 100 - 20 */
    ASSERT(vel <= 120);        /* 100 + 20 */
    /* With xorshift32(seed=1), result should differ from 100 */
    /* (But it might not always — the test verifies range is plausible) */

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_chain_order ────────────────────────────────────────── */

static int test_fx_chain_order(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Slot 0: transpose +12, Slot 1: velocity 50% */
    ts.fx_count = 2;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_TRANSPOSE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.transpose.semitones = 12;
    ts.fx[0].effect.params.transpose.cents = 0;

    ts.fx[1].track = 0;
    ts.fx[1].slot = 1;
    ts.fx[1].effect.type = SEQ_FX_VELOCITY_SCALE;
    ts.fx[1].effect.enabled = true;
    ts.fx[1].effect.params.velocity_scale.scale_pct = 50;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    /* Note should be 72 (60+12) */
    ASSERT(has_note_on(txbuf, txn, 72));
    ASSERT(!has_note_on(txbuf, txn, 60));
    /* Velocity should be 50 */
    int vel = get_note_on_velocity(txbuf, txn, 72);
    ASSERT(vel == 50);

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_bypass_flag ────────────────────────────────────────── */

static int test_fx_bypass_flag(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    /* Note with BYPASS_FX flag */
    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ev.flags |= SEQ_FLAG_BYPASS_FX;
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Transpose +7 (should be bypassed) */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_TRANSPOSE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.transpose.semitones = 7;
    ts.fx[0].effect.params.transpose.cents = 0;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    /* Note should still be 60 (bypass) */
    ASSERT(has_note_on(txbuf, txn, 60));
    ASSERT(!has_note_on(txbuf, txn, 67));

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_enable_disable ─────────────────────────────────────── */

static int test_fx_enable_disable(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Set transpose +7 */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_TRANSPOSE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.transpose.semitones = 7;
    ts.fx[0].effect.params.transpose.cents = 0;

    /* Then disable it */
    ts.use_enable = true;
    ts.enable_pl.track = 0;
    ts.enable_pl.slot = 0;
    ts.enable_pl.enabled = false;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    /* Disabled: note should be 60 (untransposed) */
    ASSERT(has_note_on(txbuf, txn, 60));
    ASSERT(!has_note_on(txbuf, txn, 67));

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_clear ──────────────────────────────────────────────── */

static int test_fx_clear(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    seq_event_t ev = seq_note(0, 60, 100, SEQ_PPQN / 4, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* Set transpose +7 */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_TRANSPOSE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.transpose.semitones = 7;
    ts.fx[0].effect.params.transpose.cents = 0;

    /* Then clear all FX */
    ts.use_clear = true;
    ts.clear_pl.track = 0;
    ts.clear_pl.slot = 0xFF;  /* clear all */

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    /* Cleared: note should be 60 */
    ASSERT(has_note_on(txbuf, txn, 60));
    ASSERT(!has_note_on(txbuf, txn, 67));

    runtime_destroy(rt);
    return 0;
}

/* ── test_fx_cc_scale ───────────────────────────────────────────── */

static int test_fx_cc_scale(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t seq_id = sequencer_init(rt);

    fx_tester_t ts;
    fx_tester_init(&ts, seq_id, midi_id);

    /* CC1 (mod wheel) val=64 at tick 0 */
    seq_event_t ev = seq_cc(0, 1, 64, 0);
    ts.events = &ev;
    ts.event_count = 1;
    ts.pattern_length = SEQ_PPQN;

    /* CCScale: CC1, min=0, max=50 */
    ts.fx_count = 1;
    ts.fx[0].track = 0;
    ts.fx[0].slot = 0;
    ts.fx[0].effect.type = SEQ_FX_CC_SCALE;
    ts.fx[0].effect.enabled = true;
    ts.fx[0].effect.params.cc_scale.cc_number = 1;
    ts.fx[0].effect.params.cc_scale.min_val = 0;
    ts.fx[0].effect.params.cc_scale.max_val = 50;

    actor_id_t tester = actor_spawn(rt, fx_generic_tester, &ts, NULL, 64);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.base.done);

    uint8_t txbuf[256];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    int cc_val = get_cc_value(txbuf, txn, 1);
    /* Input val=64, range 0–127 mapped to 0–50: 0 + 64*50/127 = 25 */
    ASSERT(cc_val >= 0);   /* CC was sent */
    ASSERT(cc_val >= 24 && cc_val <= 26);  /* ~25 with integer rounding */

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

    /* Phase 29.2: Multi-track */
    RUN_TEST(test_multi_track_load);
    RUN_TEST(test_multi_track_playback);
    RUN_TEST(test_polyrhythm);
    RUN_TEST(test_track_mute);
    RUN_TEST(test_track_solo);
    RUN_TEST(test_double_buffer_load);
    RUN_TEST(test_slot_switch);
    RUN_TEST(test_mute_unmute_toggle);
    RUN_TEST(test_solo_mask_cleared);
    RUN_TEST(test_empty_track_skip);

    /* Active note tracking */
    RUN_TEST(test_solo_kills_hanging_notes);
    RUN_TEST(test_mute_kills_hanging_notes);

    /* Phase 29.3: Per-track effects */
    RUN_TEST(test_fx_transpose);
    RUN_TEST(test_fx_velocity_scale);
    RUN_TEST(test_fx_humanize);
    RUN_TEST(test_fx_chain_order);
    RUN_TEST(test_fx_bypass_flag);
    RUN_TEST(test_fx_enable_disable);
    RUN_TEST(test_fx_clear);
    RUN_TEST(test_fx_cc_scale);

    TEST_REPORT();
}
