#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/midi.h"
#include "microkernel/midi_monitor.h"
#include "midi_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    ASSERT_NE(midi_id, ACTOR_ID_INVALID);

    actor_id_t monitor_id = midi_monitor_init(rt);
    ASSERT_NE(monitor_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/sys/midi_monitor"), monitor_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_no_midi_actor ────────────────────────────────────────────── */

static int test_no_midi_actor(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    /* Don't init MIDI actor — monitor should fail gracefully */
    actor_id_t monitor_id = midi_monitor_init(rt);
    ASSERT_EQ(monitor_id, ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

/* ── test_receives_events ──────────────────────────────────────────── */
/* Inject MIDI data and let the monitor process it (just verify no crash) */

typedef struct {
    actor_id_t midi_id;
    int        step;
    bool       done;
} monitor_tester_t;

static bool monitor_test_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    monitor_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        midi_config_payload_t cfg = {
            .i2c_port = 0, .i2c_addr = 0x48,
            .sda_pin = 8, .scl_pin = 9, .irq_pin = 7,
            .i2c_freq_hz = 400000
        };
        actor_send(rt, s->midi_id, MSG_MIDI_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_MIDI_OK && s->step == 1) {
        s->step = 2;
        /* Inject various MIDI messages */
        uint8_t data[] = {
            0x90, 0x3C, 0x64,     /* Note On C4 */
            0x80, 0x3C, 0x00,     /* Note Off C4 */
            0xB0, 0x01, 0x7F,     /* CC #1 */
            0xC0, 0x05,           /* Program Change */
            0xE0, 0x00, 0x40,     /* Pitch Bend center */
            0xF8,                 /* Clock */
        };
        midi_mock_inject_rx(data, sizeof(data));
        /* Give the runtime time to process */
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 2) {
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_receives_events(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t midi_id = midi_actor_init(rt);
    midi_monitor_init(rt);

    monitor_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.midi_id = midi_id;

    actor_id_t tester = actor_spawn(rt, monitor_test_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.done);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_midi_monitor:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_no_midi_actor);
    RUN_TEST(test_receives_events);

    TEST_REPORT();
}
