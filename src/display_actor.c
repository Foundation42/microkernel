#include "microkernel/display.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "display_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static void reply_error(runtime_t *rt, actor_id_t dest, const char *err) {
    actor_send(rt, dest, MSG_DISPLAY_ERROR, err, strlen(err));
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void handle_draw(runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(display_draw_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const display_draw_payload_t *req =
        (const display_draw_payload_t *)msg->payload;

    size_t expected = sizeof(display_draw_payload_t) +
                      (size_t)req->w * req->h * 2;
    if (msg->payload_size < expected) {
        reply_error(rt, msg->source, "pixel data too short");
        return;
    }

    if (!display_hal_draw(req->x, req->y, req->w, req->h, req->pixels)) {
        reply_error(rt, msg->source, "draw out of bounds");
        return;
    }

    actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
}

static void handle_fill(runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(display_fill_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const display_fill_payload_t *req =
        (const display_fill_payload_t *)msg->payload;

    if (!display_hal_fill(req->x, req->y, req->w, req->h, req->color)) {
        reply_error(rt, msg->source, "fill out of bounds");
        return;
    }

    actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
}

static void handle_clear(runtime_t *rt, message_t *msg) {
    if (!display_hal_clear()) {
        reply_error(rt, msg->source, "clear failed");
        return;
    }
    actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
}

static void handle_brightness(runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(display_brightness_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const display_brightness_payload_t *req =
        (const display_brightness_payload_t *)msg->payload;

    if (!display_hal_set_brightness(req->brightness)) {
        reply_error(rt, msg->source, "brightness failed");
        return;
    }

    actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
}

static void handle_power(runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(display_power_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const display_power_payload_t *req =
        (const display_power_payload_t *)msg->payload;

    if (!display_hal_power(req->on != 0)) {
        reply_error(rt, msg->source, "power failed");
        return;
    }

    actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool display_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    (void)state;

    switch (msg->type) {
    case MSG_DISPLAY_DRAW:       handle_draw(rt, msg);       break;
    case MSG_DISPLAY_FILL:       handle_fill(rt, msg);       break;
    case MSG_DISPLAY_CLEAR:      handle_clear(rt, msg);      break;
    case MSG_DISPLAY_BRIGHTNESS: handle_brightness(rt, msg); break;
    case MSG_DISPLAY_POWER:      handle_power(rt, msg);      break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void display_state_free(void *state) {
    (void)state;
    display_hal_deinit();
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t display_actor_init(runtime_t *rt) {
    if (!display_hal_init())
        return ACTOR_ID_INVALID;

    /* No per-actor state needed — HAL is a singleton.
       Pass a non-NULL dummy so state_free gets called. */
    static char dummy;

    actor_id_t id = actor_spawn(rt, display_behavior, &dummy,
                                display_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        display_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/display", id);

    return id;
}
