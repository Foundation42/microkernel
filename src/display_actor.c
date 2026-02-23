#include "microkernel/display.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "display_hal.h"
#include "text_render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

/* ── Display state ────────────────────────────────────────────────── */

typedef struct {
    uint16_t *row_buf;   /* width * FONT_HEIGHT pixels for text rendering */
    uint16_t  width;
} display_state_t;

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

/* ── Text rendering ───────────────────────────────────────────────── */

static void handle_text(runtime_t *rt, message_t *msg, display_state_t *ds) {
    if (msg->payload_size < sizeof(display_text_payload_t) + 1) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const display_text_payload_t *req =
        (const display_text_payload_t *)msg->payload;

    /* Ensure null-terminated */
    size_t text_max = msg->payload_size - sizeof(display_text_payload_t);
    size_t text_len = strnlen(req->text, text_max);
    if (text_len == 0) {
        actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
        return;
    }

    uint16_t dw = display_hal_width();
    uint16_t dh = display_hal_height();

    /* Clip: if y is off-screen, nothing to draw */
    if (req->y >= dh) {
        actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
        return;
    }

    /* Calculate drawable width from x to right edge */
    uint16_t avail_w = (req->x < dw) ? (dw - req->x) : 0;
    if (avail_w == 0) {
        actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
        return;
    }

    /* Clip height to display bounds */
    uint16_t draw_h = FONT_HEIGHT;
    if (req->y + draw_h > dh)
        draw_h = dh - req->y;

    /* Render string into row_buf (width = avail_w) */
    uint16_t render_w = (avail_w < ds->width) ? avail_w : ds->width;
    text_render_string(ds->row_buf, render_w, req->text, req->fg, req->bg);

    /* Draw the rendered strip */
    display_hal_draw(req->x, req->y, render_w, draw_h,
                     (const uint8_t *)ds->row_buf);

    actor_send(rt, msg->source, MSG_DISPLAY_OK, NULL, 0);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool display_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    display_state_t *ds = state;

    switch (msg->type) {
    case MSG_DISPLAY_DRAW:       handle_draw(rt, msg);       break;
    case MSG_DISPLAY_FILL:       handle_fill(rt, msg);       break;
    case MSG_DISPLAY_CLEAR:      handle_clear(rt, msg);      break;
    case MSG_DISPLAY_BRIGHTNESS: handle_brightness(rt, msg); break;
    case MSG_DISPLAY_POWER:      handle_power(rt, msg);      break;
    case MSG_DISPLAY_TEXT:       handle_text(rt, msg, ds);    break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void display_state_free(void *state) {
    display_state_t *ds = state;
    free(ds->row_buf);
    free(ds);
    display_hal_deinit();
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t display_actor_init(runtime_t *rt) {
    if (!display_hal_init())
        return ACTOR_ID_INVALID;

    uint16_t w = display_hal_width();

    display_state_t *ds = calloc(1, sizeof(*ds));
    if (!ds) {
        display_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    ds->width = w;
#ifdef ESP_PLATFORM
    /* Force internal RAM — PSRAM buffers cause DMA cache coherency issues
       with the QSPI display controller (GDMA reads bypass D-Cache). */
    ds->row_buf = heap_caps_calloc((size_t)w * FONT_HEIGHT, sizeof(uint16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    ds->row_buf = calloc((size_t)w * FONT_HEIGHT, sizeof(uint16_t));
#endif
    if (!ds->row_buf) {
        free(ds);
        display_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    actor_id_t id = actor_spawn(rt, display_behavior, ds,
                                display_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        free(ds->row_buf);
        free(ds);
        display_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/display", id);

    return id;
}

/* ── Convenience functions ────────────────────────────────────────── */

bool display_text(runtime_t *rt, uint16_t x, uint16_t y,
                  uint16_t fg, uint16_t bg, const char *text) {
    size_t tlen = strlen(text) + 1; /* include null terminator */
    size_t psize = sizeof(display_text_payload_t) + tlen;

    uint8_t buf[sizeof(display_text_payload_t) + 128];
    uint8_t *payload = buf;
    bool heap = false;

    if (psize > sizeof(buf)) {
        payload = malloc(psize);
        if (!payload) return false;
        heap = true;
    }

    display_text_payload_t *req = (display_text_payload_t *)payload;
    req->x = x;
    req->y = y;
    req->fg = fg;
    req->bg = bg;
    memcpy(req->text, text, tlen);

    bool ok = actor_send_named(rt, "/node/hardware/display",
                               MSG_DISPLAY_TEXT, payload, psize);
    if (heap) free(payload);
    return ok;
}

bool display_fill_rect(runtime_t *rt, uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h, uint16_t color) {
    display_fill_payload_t fill = {
        .x = x, .y = y, .w = w, .h = h, .color = color
    };
    return actor_send_named(rt, "/node/hardware/display",
                            MSG_DISPLAY_FILL, &fill, sizeof(fill));
}

bool display_clear_screen(runtime_t *rt) {
    return actor_send_named(rt, "/node/hardware/display",
                            MSG_DISPLAY_CLEAR, NULL, 0);
}
