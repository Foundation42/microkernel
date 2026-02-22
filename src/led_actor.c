#include "microkernel/led.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "led_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    bool     configured;
    uint8_t  pin;
    uint16_t num_leds;
    uint8_t  brightness;              /* 0–255, default 255 */
    uint8_t  pixels[LED_MAX_LEDS][3]; /* raw R,G,B (pre-brightness) */
} led_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void reply_error(runtime_t *rt, actor_id_t dest, const char *err) {
    actor_send(rt, dest, MSG_LED_ERROR, err, strlen(err));
}

/* Apply brightness scaling and push to HAL */
static void flush_pixels(led_state_t *s) {
    for (int i = 0; i < s->num_leds; i++) {
        uint8_t r = (uint8_t)((s->pixels[i][0] * s->brightness) / 255);
        uint8_t g = (uint8_t)((s->pixels[i][1] * s->brightness) / 255);
        uint8_t b = (uint8_t)((s->pixels[i][2] * s->brightness) / 255);
        led_hal_set_pixel(i, r, g, b);
    }
    led_hal_show();
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void handle_configure(led_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(led_config_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const led_config_payload_t *cfg = (const led_config_payload_t *)msg->payload;

    if (cfg->num_leds == 0 || cfg->num_leds > LED_MAX_LEDS) {
        reply_error(rt, msg->source, "invalid num_leds");
        return;
    }

    /* Deconfigure if already configured */
    if (s->configured)
        led_hal_deconfigure();

    if (!led_hal_configure(cfg->pin, cfg->num_leds)) {
        reply_error(rt, msg->source, "hal configure failed");
        return;
    }

    s->configured = true;
    s->pin = cfg->pin;
    s->num_leds = cfg->num_leds;
    s->brightness = 255;
    memset(s->pixels, 0, sizeof(s->pixels));

    actor_send(rt, msg->source, MSG_LED_OK, NULL, 0);
}

static void handle_set_pixel(led_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    if (msg->payload_size < sizeof(led_pixel_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const led_pixel_payload_t *px = (const led_pixel_payload_t *)msg->payload;

    if (px->index >= s->num_leds) {
        reply_error(rt, msg->source, "invalid index");
        return;
    }

    s->pixels[px->index][0] = px->r;
    s->pixels[px->index][1] = px->g;
    s->pixels[px->index][2] = px->b;

    /* SET_PIXEL does NOT auto-flush (batch-friendly) */
    actor_send(rt, msg->source, MSG_LED_OK, NULL, 0);
}

static void handle_set_all(led_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    if (msg->payload_size < sizeof(led_all_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const led_all_payload_t *req = (const led_all_payload_t *)msg->payload;
    size_t expected = sizeof(led_all_payload_t) + (size_t)req->num_leds * 3;

    if (msg->payload_size < expected || req->num_leds > s->num_leds) {
        reply_error(rt, msg->source, "payload size mismatch");
        return;
    }

    for (int i = 0; i < req->num_leds; i++) {
        s->pixels[i][0] = req->pixels[i * 3 + 0];
        s->pixels[i][1] = req->pixels[i * 3 + 1];
        s->pixels[i][2] = req->pixels[i * 3 + 2];
    }

    /* SET_ALL auto-flushes */
    flush_pixels(s);
    actor_send(rt, msg->source, MSG_LED_OK, NULL, 0);
}

static void handle_set_brightness(led_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    if (msg->payload_size < sizeof(led_brightness_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const led_brightness_payload_t *req =
        (const led_brightness_payload_t *)msg->payload;
    s->brightness = req->brightness;

    actor_send(rt, msg->source, MSG_LED_OK, NULL, 0);
}

static void handle_clear(led_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    memset(s->pixels, 0, (size_t)s->num_leds * 3);
    /* CLEAR auto-flushes */
    led_hal_clear();
    actor_send(rt, msg->source, MSG_LED_OK, NULL, 0);
}

static void handle_show(led_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    flush_pixels(s);
    actor_send(rt, msg->source, MSG_LED_OK, NULL, 0);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool led_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    led_state_t *s = state;

    switch (msg->type) {
    case MSG_LED_CONFIGURE:      handle_configure(s, rt, msg);       break;
    case MSG_LED_SET_PIXEL:      handle_set_pixel(s, rt, msg);       break;
    case MSG_LED_SET_ALL:        handle_set_all(s, rt, msg);         break;
    case MSG_LED_SET_BRIGHTNESS: handle_set_brightness(s, rt, msg);  break;
    case MSG_LED_CLEAR:          handle_clear(s, rt, msg);           break;
    case MSG_LED_SHOW:           handle_show(s, rt, msg);            break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void led_state_free(void *state) {
    led_state_t *s = state;
    if (s->configured)
        led_hal_deconfigure();
    led_hal_deinit();
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t led_actor_init(runtime_t *rt) {
    if (!led_hal_init())
        return ACTOR_ID_INVALID;

    led_state_t *s = calloc(1, sizeof(*s));
    if (!s) {
        led_hal_deinit();
        return ACTOR_ID_INVALID;
    }
    s->brightness = 255;

    actor_id_t id = actor_spawn(rt, led_behavior, s, led_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        led_hal_deinit();
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/led", id);

    return id;
}
