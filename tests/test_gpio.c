#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/gpio.h"
#include "gpio_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state (shared across sub-tests) ───────────────────────── */

typedef struct {
    actor_id_t gpio_id;
    int        step;
    bool       done;

    msg_type_t last_type;
    char       last_payload[128];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_value;
    bool       got_error;
    bool       got_event;
    bool       timeout;
} gpio_tester_t;

static void save_reply(gpio_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload) - 1
                          ? msg->payload_size : sizeof(s->last_payload) - 1;
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);
    s->last_payload[s->last_payload_size] = '\0';

    if (msg->type == MSG_GPIO_OK)    s->got_ok = true;
    if (msg->type == MSG_GPIO_VALUE) s->got_value = true;
    if (msg->type == MSG_GPIO_ERROR) s->got_error = true;
    if (msg->type == MSG_GPIO_EVENT) s->got_event = true;
}

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t gpio_id = gpio_actor_init(rt);
    ASSERT_NE(gpio_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/node/hardware/gpio"), gpio_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_configure_output ─────────────────────────────────────────── */

static bool configure_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    gpio_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "pin=4\nmode=output";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
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

static int test_configure_output(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t gpio_id = gpio_actor_init(rt);

    gpio_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, configure_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_write_read ───────────────────────────────────────────────── */

static bool write_read_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    gpio_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "pin=4\nmode=output";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 1) {
        s->step = 2;
        const char *p = "pin=4\nvalue=1";
        actor_send(rt, s->gpio_id, MSG_GPIO_WRITE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 2) {
        s->step = 3;
        const char *p = "pin=4";
        actor_send(rt, s->gpio_id, MSG_GPIO_READ, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_VALUE && s->step == 3) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_write_read(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t gpio_id = gpio_actor_init(rt);

    gpio_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, write_read_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_value);
    ASSERT(strstr(ts.last_payload, "pin=4") != NULL);
    ASSERT(strstr(ts.last_payload, "value=1") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_invalid_mode ─────────────────────────────────────────────── */

static bool invalid_mode_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    gpio_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "pin=4\nmode=bogus";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
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

static int test_invalid_mode(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t gpio_id = gpio_actor_init(rt);

    gpio_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, invalid_mode_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr(ts.last_payload, "invalid mode") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_subscribe_event ──────────────────────────────────────────── */

static bool subscribe_event_tester(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state) {
    (void)self;
    gpio_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        /* Configure pin 2 as input */
        s->step = 1;
        const char *p = "pin=2\nmode=input";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 1) {
        /* Subscribe to pin 2 rising edge */
        s->step = 2;
        const char *p = "pin=2\nedge=rising";
        actor_send(rt, s->gpio_id, MSG_GPIO_SUBSCRIBE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 2) {
        /* Subscription confirmed — trigger mock interrupt */
        s->step = 3;
        gpio_mock_trigger_interrupt(2, 1);
        /* Set a safety timeout */
        actor_set_timer(rt, 500, false);
        return true;
    }

    if (msg->type == MSG_GPIO_EVENT && s->step == 3) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        /* Timeout — no event received */
        s->timeout = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_subscribe_event(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t gpio_id = gpio_actor_init(rt);

    gpio_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, subscribe_event_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_event);
    ASSERT(!ts.timeout);
    ASSERT(strstr(ts.last_payload, "pin=2") != NULL);
    ASSERT(strstr(ts.last_payload, "value=1") != NULL);
    ASSERT(strstr(ts.last_payload, "edge=rising") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_unsubscribe ──────────────────────────────────────────────── */

static bool unsubscribe_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    gpio_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "pin=2\nmode=input";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 1) {
        s->step = 2;
        const char *p = "pin=2\nedge=rising";
        actor_send(rt, s->gpio_id, MSG_GPIO_SUBSCRIBE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 2) {
        /* Now unsubscribe */
        s->step = 3;
        const char *p = "pin=2";
        actor_send(rt, s->gpio_id, MSG_GPIO_UNSUBSCRIBE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 3) {
        /* Trigger interrupt — should NOT receive event */
        s->step = 4;
        gpio_mock_trigger_interrupt(2, 1);
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_GPIO_EVENT && s->step == 4) {
        /* Bad: got event after unsubscribe */
        s->got_event = true;
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        /* Good: timeout, no event */
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
    actor_id_t gpio_id = gpio_actor_init(rt);

    gpio_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, unsubscribe_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(!ts.got_event);
    ASSERT(ts.timeout);

    runtime_destroy(rt);
    return 0;
}

/* ── test_reconfigure_clears_subs ──────────────────────────────────── */

static bool reconfig_tester(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    gpio_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "pin=3\nmode=input";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 1) {
        s->step = 2;
        const char *p = "pin=3\nedge=both";
        actor_send(rt, s->gpio_id, MSG_GPIO_SUBSCRIBE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 2) {
        /* Reconfigure to output — should clear subscriptions */
        s->step = 3;
        const char *p = "pin=3\nmode=output";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 3) {
        /* Trigger interrupt — should NOT receive event */
        s->step = 4;
        gpio_mock_trigger_interrupt(3, 1);
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_GPIO_EVENT && s->step == 4) {
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

static int test_reconfigure_clears_subs(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t gpio_id = gpio_actor_init(rt);

    gpio_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, reconfig_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(!ts.got_event);
    ASSERT(ts.timeout);

    runtime_destroy(rt);
    return 0;
}

/* ── test_dead_subscriber ──────────────────────────────────────────── */

typedef struct {
    actor_id_t gpio_id;
    actor_id_t tester_id;
    bool       subscribed;
} victim_state_t;

static bool victim_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    victim_state_t *v = state;

    if (msg->type == 1) {
        /* Subscribe to pin 5 */
        const char *p = "pin=5\nedge=both";
        actor_send(rt, v->gpio_id, MSG_GPIO_SUBSCRIBE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK) {
        v->subscribed = true;
        /* Signal tester, then die */
        actor_send(rt, v->tester_id, 42, NULL, 0);
        return false;
    }

    return true;
}

typedef struct {
    actor_id_t gpio_id;
    int        step;
    bool       done;
    bool       no_crash;
} dead_sub_tester_t;

static bool dead_sub_behavior(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    dead_sub_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        /* Configure pin 5 as input */
        s->step = 1;
        const char *p = "pin=5\nmode=input";
        actor_send(rt, s->gpio_id, MSG_GPIO_CONFIGURE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_GPIO_OK && s->step == 1) {
        /* Spawn victim */
        s->step = 2;
        victim_state_t *vs = calloc(1, sizeof(*vs));
        vs->gpio_id = s->gpio_id;
        vs->tester_id = actor_self(rt);
        actor_id_t victim = actor_spawn(rt, victim_behavior, vs, free, 16);
        actor_send(rt, victim, 1, NULL, 0);
        return true;
    }

    if (msg->type == 42 && s->step == 2) {
        /* Victim subscribed and is dying — wait for cleanup */
        s->step = 3;
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 3) {
        /* Victim should be dead. Trigger interrupt. */
        s->step = 4;
        gpio_mock_trigger_interrupt(5, 1);
        actor_set_timer(rt, 100, false);
        return true;
    }

    if (msg->type == MSG_TIMER && s->step == 4) {
        /* No crash — success */
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
    actor_id_t gpio_id = gpio_actor_init(rt);

    dead_sub_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.gpio_id = gpio_id;

    actor_id_t tester = actor_spawn(rt, dead_sub_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.no_crash);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gpio:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_configure_output);
    RUN_TEST(test_write_read);
    RUN_TEST(test_invalid_mode);
    RUN_TEST(test_subscribe_event);
    RUN_TEST(test_unsubscribe);
    RUN_TEST(test_reconfigure_clears_subs);
    RUN_TEST(test_dead_subscriber);

    TEST_REPORT();
}
