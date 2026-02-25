#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/midi.h"
#include "microkernel/arpeggiator.h"
#include "midi_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static void send_midi_config(runtime_t *rt, actor_id_t midi_id) {
    midi_config_payload_t cfg = {
        .i2c_port = 0, .i2c_addr = 0x48,
        .sda_pin = 8, .scl_pin = 9, .irq_pin = 7,
        .i2c_freq_hz = 400000
    };
    actor_send(rt, midi_id, MSG_MIDI_CONFIGURE, &cfg, sizeof(cfg));
}

/* Count Note On messages (0x9n with vel > 0) in TX buffer */
static int count_note_ons(const uint8_t *buf, int len) {
    int count = 0;
    int i = 0;
    while (i < len) {
        uint8_t status = buf[i];
        if (status < 0x80) { i++; continue; } /* stray data byte */
        int msg_len = 0;
        switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
            msg_len = 3; break;
        case 0xC0: case 0xD0:
            msg_len = 2; break;
        default:
            msg_len = 1; break;
        }
        if (i + msg_len > len) break;
        if ((status & 0xF0) == 0x90 && msg_len >= 3 && buf[i + 2] > 0)
            count++;
        i += msg_len;
    }
    return count;
}

/* Extract Note On pitches from TX buffer into an array.
 * Returns number of Note On events found. */
static int extract_note_on_pitches(const uint8_t *buf, int len,
                                    uint8_t *pitches, int max) {
    int count = 0;
    int i = 0;
    while (i < len && count < max) {
        uint8_t status = buf[i];
        if (status < 0x80) { i++; continue; }
        int msg_len = 0;
        switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
            msg_len = 3; break;
        case 0xC0: case 0xD0:
            msg_len = 2; break;
        default:
            msg_len = 1; break;
        }
        if (i + msg_len > len) break;
        if ((status & 0xF0) == 0x90 && msg_len >= 3 && buf[i + 2] > 0)
            pitches[count++] = buf[i + 1];
        i += msg_len;
    }
    return count;
}

/* ── Tester state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t midi_id;
    actor_id_t arp_id;
    int        step;
    bool       done;
    uint8_t    tx_buf[512];
    int        tx_len;
} arp_tester_t;

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    ASSERT_NE(midi_id, ACTOR_ID_INVALID);

    actor_id_t arp_id = arpeggiator_init(rt);
    ASSERT_NE(arp_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/sys/arpeggiator"), arp_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_single_note ──────────────────────────────────────────────── */
/* Hold one note, verify arp plays it repeatedly */

static bool single_note_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    arp_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Set fast BPM for quick test: 480 BPM → 16th = 31ms */
        arp_bpm_payload_t bpm = { .bpm = 300 };
        actor_send(rt, s->arp_id, MSG_ARP_SET_BPM, &bpm, sizeof(bpm));
        /* Small delay for messages to process */
        actor_set_timer(rt, 10, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 2) {
        s->step = 3;
        midi_mock_clear_tx();
        /* Inject Note On C4 */
        uint8_t note_on[] = { 0x90, 60, 100 };
        midi_mock_inject_rx(note_on, sizeof(note_on));
        /* Wait for several arp steps */
        actor_set_timer(rt, 300, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->tx_len = midi_mock_get_tx(s->tx_buf, (int)sizeof(s->tx_buf));
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_single_note(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t arp_id = arpeggiator_init(rt);

    arp_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;
    ts.arp_id = arp_id;

    actor_id_t tester = actor_spawn(rt, single_note_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);
    /* Should have generated several Note On events, all on C4 (note 60) */
    int note_ons = count_note_ons(ts.tx_buf, ts.tx_len);
    ASSERT(note_ons >= 3); /* at least 3 steps in 300ms at 300 BPM */

    uint8_t pitches[32];
    int n = extract_note_on_pitches(ts.tx_buf, ts.tx_len, pitches, 32);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(pitches[i], 60); /* all should be C4 */

    runtime_destroy(rt);
    return 0;
}

/* ── test_up_pattern ───────────────────────────────────────────────── */
/* Hold C4, E4, G4 — verify UP pattern cycles through them */

static bool up_pattern_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    arp_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        arp_bpm_payload_t bpm = { .bpm = 300 };
        actor_send(rt, s->arp_id, MSG_ARP_SET_BPM, &bpm, sizeof(bpm));
        actor_set_timer(rt, 10, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 2) {
        s->step = 3;
        midi_mock_clear_tx();
        /* Inject C4, E4, G4 Note On */
        uint8_t notes[] = {
            0x90, 60, 100,  /* C4 */
            0x90, 64, 100,  /* E4 */
            0x90, 67, 100,  /* G4 */
        };
        midi_mock_inject_rx(notes, sizeof(notes));
        /* Wait for several cycles */
        actor_set_timer(rt, 400, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->tx_len = midi_mock_get_tx(s->tx_buf, (int)sizeof(s->tx_buf));
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_up_pattern(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t arp_id = arpeggiator_init(rt);

    arp_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;
    ts.arp_id = arp_id;

    actor_id_t tester = actor_spawn(rt, up_pattern_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);

    uint8_t pitches[32];
    int n = extract_note_on_pitches(ts.tx_buf, ts.tx_len, pitches, 32);
    ASSERT(n >= 6); /* at least 2 full cycles */

    /* First note is the immediate play (C4), then timer advances */
    /* UP pattern with [C4=60, E4=64, G4=67]: 60, 64, 67, 60, 64, 67, ... */
    /* Check that we see the 3-note cycle */
    uint8_t expected[] = { 60, 64, 67 };
    for (int i = 0; i < n; i++)
        ASSERT_EQ(pitches[i], expected[i % 3]);

    runtime_destroy(rt);
    return 0;
}

/* ── test_down_pattern ─────────────────────────────────────────────── */

static bool down_pattern_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    arp_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        arp_bpm_payload_t bpm = { .bpm = 300 };
        actor_send(rt, s->arp_id, MSG_ARP_SET_BPM, &bpm, sizeof(bpm));
        arp_pattern_payload_t pat = { .pattern = ARP_DOWN };
        actor_send(rt, s->arp_id, MSG_ARP_SET_PATTERN, &pat, sizeof(pat));
        actor_set_timer(rt, 10, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 2) {
        s->step = 3;
        midi_mock_clear_tx();
        uint8_t notes[] = {
            0x90, 60, 100,
            0x90, 64, 100,
            0x90, 67, 100,
        };
        midi_mock_inject_rx(notes, sizeof(notes));
        actor_set_timer(rt, 400, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->tx_len = midi_mock_get_tx(s->tx_buf, (int)sizeof(s->tx_buf));
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_down_pattern(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t arp_id = arpeggiator_init(rt);

    arp_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;
    ts.arp_id = arp_id;

    actor_id_t tester = actor_spawn(rt, down_pattern_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);

    uint8_t pitches[32];
    int n = extract_note_on_pitches(ts.tx_buf, ts.tx_len, pitches, 32);
    ASSERT(n >= 6);

    /* DOWN pattern: first note played immediately at idx 0 (C4=60),
     * then advance_index goes to idx 2 (G4), 1 (E4), 0 (C4), 2 (G4)...
     * So sequence: 60, 67, 64, 60, 67, 64, ... */
    uint8_t expected[] = { 60, 67, 64 };
    for (int i = 0; i < n; i++)
        ASSERT_EQ(pitches[i], expected[i % 3]);

    runtime_destroy(rt);
    return 0;
}

/* ── test_note_release_stops ───────────────────────────────────────── */
/* Hold note, release, verify arp stops (final Note Off sent) */

static bool release_tester(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)self;
    arp_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        arp_bpm_payload_t bpm = { .bpm = 300 };
        actor_send(rt, s->arp_id, MSG_ARP_SET_BPM, &bpm, sizeof(bpm));
        actor_set_timer(rt, 10, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 2) {
        s->step = 3;
        /* Note On C4 */
        uint8_t note_on[] = { 0x90, 60, 100 };
        midi_mock_inject_rx(note_on, sizeof(note_on));
        /* Let it play a bit */
        actor_set_timer(rt, 150, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->step = 4;
        /* Note Off C4 */
        uint8_t note_off[] = { 0x80, 60, 0 };
        midi_mock_inject_rx(note_off, sizeof(note_off));
        /* Brief wait for Note Off to be processed by arp */
        actor_set_timer(rt, 80, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        s->step = 5;
        /* Now arp should have stopped — clear TX and wait to verify silence */
        midi_mock_clear_tx();
        actor_set_timer(rt, 200, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 5) {
        s->tx_len = midi_mock_get_tx(s->tx_buf, (int)sizeof(s->tx_buf));
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_note_release_stops(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t arp_id = arpeggiator_init(rt);

    arp_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;
    ts.arp_id = arp_id;

    actor_id_t tester = actor_spawn(rt, release_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);
    /* After Note Off + 200ms wait, we should see a final Note Off
     * but NO new Note On events (arp stopped) */
    int note_ons = count_note_ons(ts.tx_buf, ts.tx_len);
    ASSERT_EQ(note_ons, 0);

    runtime_destroy(rt);
    return 0;
}

/* ── test_enable_disable ───────────────────────────────────────────── */

static bool enable_tester(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    arp_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_midi_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Disable arp */
        arp_enable_payload_t en = { .enable = 0 };
        actor_send(rt, s->arp_id, MSG_ARP_ENABLE, &en, sizeof(en));
        arp_bpm_payload_t bpm = { .bpm = 300 };
        actor_send(rt, s->arp_id, MSG_ARP_SET_BPM, &bpm, sizeof(bpm));
        actor_set_timer(rt, 10, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 2) {
        s->step = 3;
        midi_mock_clear_tx();
        /* Inject note — arp disabled, should NOT generate output */
        uint8_t note_on[] = { 0x90, 60, 100 };
        midi_mock_inject_rx(note_on, sizeof(note_on));
        actor_set_timer(rt, 150, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->tx_len = midi_mock_get_tx(s->tx_buf, (int)sizeof(s->tx_buf));
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_enable_disable(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    actor_id_t arp_id = arpeggiator_init(rt);

    arp_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;
    ts.arp_id = arp_id;

    actor_id_t tester = actor_spawn(rt, enable_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);
    /* Arp was disabled — should see no Note On output */
    int note_ons = count_note_ons(ts.tx_buf, ts.tx_len);
    ASSERT_EQ(note_ons, 0);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_arpeggiator:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_single_note);
    RUN_TEST(test_up_pattern);
    RUN_TEST(test_down_pattern);
    RUN_TEST(test_note_release_stops);
    RUN_TEST(test_enable_disable);

    TEST_REPORT();
}
