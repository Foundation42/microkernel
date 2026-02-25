#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/midi.h"
#include "midi_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state (shared across sub-tests) ───────────────────────── */

typedef struct {
    actor_id_t midi_id;
    int        step;
    bool       done;

    msg_type_t last_type;
    uint8_t    last_payload[256];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_error;
    bool       got_event;
    bool       got_sysex_event;
    bool       timeout;
} midi_tester_t;

static void save_reply(midi_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload)
                          ? msg->payload_size : sizeof(s->last_payload);
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);

    if (msg->type == MSG_MIDI_OK)          s->got_ok = true;
    if (msg->type == MSG_MIDI_ERROR)       s->got_error = true;
    if (msg->type == MSG_MIDI_EVENT)       s->got_event = true;
    if (msg->type == MSG_MIDI_SYSEX_EVENT) s->got_sysex_event = true;
}

static void send_config(runtime_t *rt, actor_id_t midi_id) {
    midi_config_payload_t cfg = {
        .i2c_port = 0, .i2c_addr = 0x48,
        .sda_pin = 8, .scl_pin = 9, .irq_pin = 7,
        .i2c_freq_hz = 400000
    };
    actor_send(rt, midi_id, MSG_MIDI_CONFIGURE, &cfg, sizeof(cfg));
}

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t midi_id = midi_actor_init(rt);
    ASSERT_NE(midi_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/node/hardware/midi"), midi_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_configure ────────────────────────────────────────────────── */

static bool configure_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_configure(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, configure_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    ASSERT(midi_mock_is_configured());

    runtime_destroy(rt);
    return 0;
}

/* ── test_send_note_on ─────────────────────────────────────────────── */

static bool send_note_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_mock_clear_tx();
        midi_send_payload_t p = { .status = 0x90, .data1 = 60, .data2 = 127 };
        actor_send(rt, s->midi_id, MSG_MIDI_SEND, &p, sizeof(p));
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

static int test_send_note_on(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, send_note_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* Verify TX bytes */
    uint8_t txbuf[16];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    ASSERT_EQ(txn, 3);
    ASSERT_EQ(txbuf[0], 0x90);
    ASSERT_EQ(txbuf[1], 60);
    ASSERT_EQ(txbuf[2], 127);

    runtime_destroy(rt);
    return 0;
}

/* ── test_send_program_change ──────────────────────────────────────── */

static bool send_pgm_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_mock_clear_tx();
        midi_send_payload_t p = { .status = 0xC0, .data1 = 5, .data2 = 0 };
        actor_send(rt, s->midi_id, MSG_MIDI_SEND, &p, sizeof(p));
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

static int test_send_program_change(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, send_pgm_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* Program Change is 2 bytes only */
    uint8_t txbuf[16];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    ASSERT_EQ(txn, 2);
    ASSERT_EQ(txbuf[0], 0xC0);
    ASSERT_EQ(txbuf[1], 5);

    runtime_destroy(rt);
    return 0;
}

/* ── test_send_sysex ───────────────────────────────────────────────── */

static bool send_sysex_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_mock_clear_tx();

        uint8_t sysex[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
        size_t psize = sizeof(midi_sysex_payload_t) + sizeof(sysex);
        uint8_t *buf = calloc(1, psize);
        midi_sysex_payload_t *p = (midi_sysex_payload_t *)buf;
        p->length = sizeof(sysex);
        memcpy(p->data, sysex, sizeof(sysex));
        actor_send(rt, s->midi_id, MSG_MIDI_SEND_SYSEX, buf, psize);
        free(buf);
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

static int test_send_sysex(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, send_sysex_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    uint8_t txbuf[16];
    int txn = midi_mock_get_tx(txbuf, sizeof(txbuf));
    ASSERT_EQ(txn, 6);
    ASSERT_EQ(txbuf[0], 0xF0);
    ASSERT_EQ(txbuf[5], 0xF7);

    runtime_destroy(rt);
    return 0;
}

/* ── test_subscribe_note_event ─────────────────────────────────────── */

static bool sub_note_tester(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_NOTE
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        /* Inject Note On: channel 0, note 60, velocity 100 */
        uint8_t data[] = { 0x90, 0x3C, 0x64 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 3) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_subscribe_note_event(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, sub_note_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_event);
    ASSERT(!ts.timeout);

    const midi_event_payload_t *ev =
        (const midi_event_payload_t *)ts.last_payload;
    ASSERT_EQ(ev->status, 0x90);
    ASSERT_EQ(ev->data1, 0x3C);
    ASSERT_EQ(ev->data2, 0x64);
    ASSERT_EQ(ev->channel, 0);

    runtime_destroy(rt);
    return 0;
}

/* ── test_subscribe_filter ─────────────────────────────────────────── */

static bool sub_filter_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Subscribe to CC only */
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_CC
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        /* Inject Note On — should NOT match CC filter */
        uint8_t data[] = { 0x90, 0x3C, 0x64 };
        midi_mock_inject_rx(data, sizeof(data));
        /* Short timer to check no event arrives */
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 3) {
        /* Bad: got note event with CC filter */
        s->got_event = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        /* Good: no note event. Now inject CC */
        s->step = 4;
        uint8_t data[] = { 0xB0, 0x01, 0x7F }; /* CC#1, value 127 */
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 4) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_subscribe_filter(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, sub_filter_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    /* Should NOT have gotten note event (step 3) */
    ASSERT(!ts.timeout);
    /* Should have gotten CC event (step 4) */
    ASSERT(ts.got_event);

    const midi_event_payload_t *ev =
        (const midi_event_payload_t *)ts.last_payload;
    ASSERT_EQ(ev->status, 0xB0);
    ASSERT_EQ(ev->data1, 0x01);
    ASSERT_EQ(ev->data2, 0x7F);

    runtime_destroy(rt);
    return 0;
}

/* ── test_subscribe_channel_filter ─────────────────────────────────── */

static bool sub_ch_tester(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Subscribe to channel 1 only, all message types */
        midi_subscribe_payload_t sub = {
            .channel = 1, .msg_filter = MIDI_FILTER_ALL
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        /* Inject Note On on channel 0 — should NOT match */
        uint8_t data[] = { 0x90, 0x3C, 0x64 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 3) {
        /* Bad: got ch0 event with ch1 filter */
        s->got_event = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        /* Good: no ch0 event. Now inject on channel 1 */
        s->step = 4;
        uint8_t data[] = { 0x91, 0x3C, 0x64 }; /* Note On ch1 */
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 4) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_subscribe_channel_filter(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, sub_ch_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(!ts.timeout);
    ASSERT(ts.got_event);

    const midi_event_payload_t *ev =
        (const midi_event_payload_t *)ts.last_payload;
    ASSERT_EQ(ev->status, 0x91);
    ASSERT_EQ(ev->channel, 1);

    runtime_destroy(rt);
    return 0;
}

/* ── test_sysex_event ──────────────────────────────────────────────── */

static bool sysex_event_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_SYSEX
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        uint8_t data[] = { 0xF0, 0x7E, 0x01, 0xF7 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_MIDI_SYSEX_EVENT && s->step == 3) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_sysex_event(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, sysex_event_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_sysex_event);
    ASSERT(!ts.timeout);

    const midi_sysex_event_payload_t *ev =
        (const midi_sysex_event_payload_t *)ts.last_payload;
    ASSERT_EQ(ev->length, 4);
    ASSERT_EQ(ev->data[0], 0xF0);
    ASSERT_EQ(ev->data[1], 0x7E);
    ASSERT_EQ(ev->data[2], 0x01);
    ASSERT_EQ(ev->data[3], 0xF7);

    runtime_destroy(rt);
    return 0;
}

/* ── test_unsubscribe ──────────────────────────────────────────────── */

static bool unsub_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_ALL
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        actor_send(rt, s->midi_id, MSG_MIDI_UNSUBSCRIBE, NULL, 0);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 3) {
        s->step = 4;
        /* Inject data — should NOT receive event */
        uint8_t data[] = { 0x90, 0x3C, 0x64 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 4) {
        s->got_event = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_unsubscribe(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, unsub_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(!ts.got_event);
    ASSERT(ts.timeout);

    runtime_destroy(rt);
    return 0;
}

/* ── test_error_unconfigured ───────────────────────────────────────── */

static bool unconfigured_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        /* Send without configuring first */
        midi_send_payload_t p = { .status = 0x90, .data1 = 60, .data2 = 127 };
        actor_send(rt, s->midi_id, MSG_MIDI_SEND, &p, sizeof(p));
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_error_unconfigured(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, unconfigured_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "not configured") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_running_status ───────────────────────────────────────────── */

static bool running_status_tester(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_NOTE
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        /* Inject: Note On + running status second note (no status byte) */
        uint8_t data[] = { 0x90, 0x3C, 0x64, 0x40, 0x50 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 3) {
        /* First note event */
        save_reply(s, msg);
        s->step = 4;
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 4) {
        /* Second note event (running status) */
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_running_status(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, running_status_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(!ts.timeout);
    ASSERT(ts.got_event);

    /* The second (running status) note should have data1=0x40, data2=0x50 */
    const midi_event_payload_t *ev =
        (const midi_event_payload_t *)ts.last_payload;
    ASSERT_EQ(ev->status, 0x90);
    ASSERT_EQ(ev->data1, 0x40);
    ASSERT_EQ(ev->data2, 0x50);

    runtime_destroy(rt);
    return 0;
}

/* ── test_realtime_interleaved ─────────────────────────────────────── */

static bool realtime_tester(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    midi_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_NOTE | MIDI_FILTER_REALTIME
        };
        actor_send(rt, s->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 2) {
        s->step = 3;
        /* Inject: Note On with Clock byte interleaved */
        uint8_t data[] = { 0x90, 0xF8, 0x3C, 0x64 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && s->step == 3) {
        const midi_event_payload_t *ev =
            (const midi_event_payload_t *)msg->payload;
        if (ev->status == 0xF8) {
            /* Clock event — wait for note */
            s->step = 4;
            return true;
        }
        /* Got note before clock? Still fine — save it */
        save_reply(s, msg);
        s->step = 5;
        return true;
    }

    if (msg->type == MSG_MIDI_EVENT && (s->step == 4 || s->step == 5)) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER) {
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_realtime_interleaved(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, realtime_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(!ts.timeout);
    ASSERT(ts.got_event);

    /* The note should still parse correctly despite interleaved Clock */
    const midi_event_payload_t *ev =
        (const midi_event_payload_t *)ts.last_payload;
    ASSERT_EQ(ev->status & 0xF0, 0x90);
    ASSERT_EQ(ev->data1, 0x3C);
    ASSERT_EQ(ev->data2, 0x64);

    runtime_destroy(rt);
    return 0;
}

/* ── test_dead_subscriber ──────────────────────────────────────────── */

typedef struct {
    actor_id_t midi_id;
    actor_id_t tester_id;
    bool       subscribed;
} midi_victim_state_t;

static bool midi_victim_behavior(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state) {
    (void)self;
    midi_victim_state_t *v = state;

    if (msg->type == 1) {
        midi_subscribe_payload_t sub = {
            .channel = 0xFF, .msg_filter = MIDI_FILTER_ALL
        };
        actor_send(rt, v->midi_id, MSG_MIDI_SUBSCRIBE, &sub, sizeof(sub));
        return true;
    }

    if (msg->type == MSG_MIDI_OK) {
        v->subscribed = true;
        actor_send(rt, v->tester_id, 42, NULL, 0);
        return false; /* die */
    }

    return true;
}

typedef struct {
    actor_id_t midi_id;
    int        step;
    bool       done;
    bool       no_crash;
} midi_dead_sub_tester_t;

static bool midi_dead_sub_behavior(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state) {
    (void)self;
    midi_dead_sub_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        send_config(rt, s->midi_id);
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        midi_victim_state_t *vs = calloc(1, sizeof(*vs));
        vs->midi_id = s->midi_id;
        vs->tester_id = actor_self(rt);
        actor_id_t victim = actor_spawn(rt, midi_victim_behavior, vs, free, 16);
        actor_send(rt, victim, 1, NULL, 0);
        return true;
    }

    if (msg->type == 42 && s->step == 2) {
        s->step = 3;
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        s->step = 4;
        uint8_t data[] = { 0x90, 0x3C, 0x64 };
        midi_mock_inject_rx(data, sizeof(data));
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        s->no_crash = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_dead_subscriber(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);

    midi_dead_sub_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, midi_dead_sub_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.no_crash);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_midi:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_configure);
    RUN_TEST(test_send_note_on);
    RUN_TEST(test_send_program_change);
    RUN_TEST(test_send_sysex);
    RUN_TEST(test_subscribe_note_event);
    RUN_TEST(test_subscribe_filter);
    RUN_TEST(test_subscribe_channel_filter);
    RUN_TEST(test_sysex_event);
    RUN_TEST(test_unsubscribe);
    RUN_TEST(test_error_unconfigured);
    RUN_TEST(test_running_status);
    RUN_TEST(test_realtime_interleaved);
    RUN_TEST(test_dead_subscriber);

    TEST_REPORT();
}
