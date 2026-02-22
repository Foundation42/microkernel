#include "microkernel/pwm.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "pwm_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    bool     configured[PWM_MAX_CHANNELS];
    uint8_t  pin[PWM_MAX_CHANNELS];
    uint32_t freq[PWM_MAX_CHANNELS];
    uint8_t  resolution[PWM_MAX_CHANNELS];
} pwm_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void reply_error(runtime_t *rt, actor_id_t dest, const char *err) {
    actor_send(rt, dest, MSG_PWM_ERROR, err, strlen(err));
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void handle_configure(pwm_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(pwm_config_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const pwm_config_payload_t *cfg = (const pwm_config_payload_t *)msg->payload;
    int ch = cfg->channel;

    if (ch >= PWM_MAX_CHANNELS) {
        reply_error(rt, msg->source, "invalid channel");
        return;
    }

    /* Deconfigure if already configured */
    if (s->configured[ch])
        pwm_hal_deconfigure(ch);

    if (!pwm_hal_configure(ch, cfg->pin, cfg->freq_hz, cfg->resolution)) {
        reply_error(rt, msg->source, "hal configure failed");
        return;
    }

    s->configured[ch] = true;
    s->pin[ch] = cfg->pin;
    s->freq[ch] = cfg->freq_hz;
    s->resolution[ch] = cfg->resolution;

    actor_send(rt, msg->source, MSG_PWM_OK, NULL, 0);
}

static void handle_set_duty(pwm_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(pwm_duty_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const pwm_duty_payload_t *req = (const pwm_duty_payload_t *)msg->payload;
    int ch = req->channel;

    if (ch >= PWM_MAX_CHANNELS) {
        reply_error(rt, msg->source, "invalid channel");
        return;
    }

    if (!s->configured[ch]) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    if (!pwm_hal_set_duty(ch, req->duty)) {
        reply_error(rt, msg->source, "hal set duty failed");
        return;
    }

    actor_send(rt, msg->source, MSG_PWM_OK, NULL, 0);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool pwm_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    pwm_state_t *s = state;

    switch (msg->type) {
    case MSG_PWM_CONFIGURE: handle_configure(s, rt, msg); break;
    case MSG_PWM_SET_DUTY:  handle_set_duty(s, rt, msg);  break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void pwm_state_free(void *state) {
    pwm_state_t *s = state;
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (s->configured[i])
            pwm_hal_deconfigure(i);
    }
    pwm_hal_deinit();
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t pwm_actor_init(runtime_t *rt) {
    if (!pwm_hal_init())
        return ACTOR_ID_INVALID;

    pwm_state_t *s = calloc(1, sizeof(*s));
    if (!s) {
        pwm_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    actor_id_t id = actor_spawn(rt, pwm_behavior, s, pwm_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        pwm_hal_deinit();
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/pwm", id);

    return id;
}
