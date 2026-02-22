#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/pwm.h"
#include "pwm_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t pwm_id;
    int        step;
    bool       done;

    msg_type_t last_type;
    uint8_t    last_payload[256];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_error;
} pwm_tester_t;

static void save_reply(pwm_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload)
                          ? msg->payload_size : sizeof(s->last_payload);
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);

    if (msg->type == MSG_PWM_OK)    s->got_ok = true;
    if (msg->type == MSG_PWM_ERROR) s->got_error = true;
}

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t pwm_id = pwm_actor_init(rt);
    ASSERT_NE(pwm_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/node/hardware/pwm"), pwm_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_configure ────────────────────────────────────────────────── */

static bool configure_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    pwm_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        pwm_config_payload_t cfg = {
            .channel = 0, .pin = 2, .resolution = 10,
            .freq_hz = 5000
        };
        actor_send(rt, s->pwm_id, MSG_PWM_CONFIGURE, &cfg, sizeof(cfg));
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
    actor_id_t pwm_id = pwm_actor_init(rt);

    pwm_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.pwm_id = pwm_id;

    actor_id_t tester = actor_spawn(rt, configure_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    ASSERT(pwm_mock_is_configured(0));

    runtime_destroy(rt);
    return 0;
}

/* ── test_set_duty ─────────────────────────────────────────────────── */

static bool set_duty_tester(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    pwm_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        pwm_config_payload_t cfg = {
            .channel = 0, .pin = 2, .resolution = 10,
            .freq_hz = 5000
        };
        actor_send(rt, s->pwm_id, MSG_PWM_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_PWM_OK && s->step == 1) {
        s->step = 2;
        pwm_duty_payload_t duty = { .channel = 0, .duty = 512 };
        actor_send(rt, s->pwm_id, MSG_PWM_SET_DUTY, &duty, sizeof(duty));
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

static int test_set_duty(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t pwm_id = pwm_actor_init(rt);

    pwm_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.pwm_id = pwm_id;

    actor_id_t tester = actor_spawn(rt, set_duty_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    ASSERT_EQ(pwm_mock_get_duty(0), 512);

    runtime_destroy(rt);
    return 0;
}

/* ── test_error_unconfigured ───────────────────────────────────────── */

static bool unconfigured_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    pwm_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        pwm_duty_payload_t duty = { .channel = 0, .duty = 100 };
        actor_send(rt, s->pwm_id, MSG_PWM_SET_DUTY, &duty, sizeof(duty));
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
    actor_id_t pwm_id = pwm_actor_init(rt);

    pwm_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.pwm_id = pwm_id;

    actor_id_t tester = actor_spawn(rt, unconfigured_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "not configured") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_error_invalid_channel ────────────────────────────────────── */

static bool invalid_channel_tester(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state) {
    (void)self;
    pwm_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        pwm_config_payload_t cfg = {
            .channel = 99, .pin = 2, .resolution = 10,
            .freq_hz = 5000
        };
        actor_send(rt, s->pwm_id, MSG_PWM_CONFIGURE, &cfg, sizeof(cfg));
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

static int test_error_invalid_channel(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t pwm_id = pwm_actor_init(rt);

    pwm_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.pwm_id = pwm_id;

    actor_id_t tester = actor_spawn(rt, invalid_channel_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "invalid channel") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_reconfigure ──────────────────────────────────────────────── */

static bool reconfigure_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    pwm_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        pwm_config_payload_t cfg = {
            .channel = 0, .pin = 2, .resolution = 8,
            .freq_hz = 1000
        };
        actor_send(rt, s->pwm_id, MSG_PWM_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_PWM_OK && s->step == 1) {
        s->step = 2;
        /* Reconfigure same channel with different params */
        pwm_config_payload_t cfg = {
            .channel = 0, .pin = 3, .resolution = 12,
            .freq_hz = 10000
        };
        actor_send(rt, s->pwm_id, MSG_PWM_CONFIGURE, &cfg, sizeof(cfg));
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

static int test_reconfigure(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t pwm_id = pwm_actor_init(rt);

    pwm_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.pwm_id = pwm_id;

    actor_id_t tester = actor_spawn(rt, reconfigure_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    ASSERT(pwm_mock_is_configured(0));

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_pwm:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_configure);
    RUN_TEST(test_set_duty);
    RUN_TEST(test_error_unconfigured);
    RUN_TEST(test_error_invalid_channel);
    RUN_TEST(test_reconfigure);

    TEST_REPORT();
}
