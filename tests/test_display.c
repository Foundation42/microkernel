#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/display.h"
#include "display_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t display_id;
    int        step;
    bool       done;

    msg_type_t last_type;
    uint8_t    last_payload[256];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_error;
    int        ok_count;
} display_tester_t;

static void save_reply(display_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload)
                          ? msg->payload_size : sizeof(s->last_payload);
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);

    if (msg->type == MSG_DISPLAY_OK) {
        s->got_ok = true;
        s->ok_count++;
    }
    if (msg->type == MSG_DISPLAY_ERROR) s->got_error = true;
}

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t did = display_actor_init(rt);
    ASSERT_NE(did, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/node/hardware/display"), did);

    runtime_destroy(rt);
    return 0;
}

/* ── test_fill ─────────────────────────────────────────────────────── */

static bool fill_tester(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        display_fill_payload_t fill = {
            .x = 0, .y = 0, .w = 10, .h = 10,
            .color = 0xF800 /* red in RGB565 */
        };
        actor_send(rt, s->display_id, MSG_DISPLAY_FILL, &fill, sizeof(fill));
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

static int test_fill(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, fill_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* Verify mock pixels in filled region */
    ASSERT_EQ(display_mock_get_pixel(0, 0), 0xF800);
    ASSERT_EQ(display_mock_get_pixel(9, 9), 0xF800);
    /* Outside region should be black */
    ASSERT_EQ(display_mock_get_pixel(10, 0), 0x0000);

    runtime_destroy(rt);
    return 0;
}

/* ── test_draw ─────────────────────────────────────────────────────── */

static bool draw_tester(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        /* 2x2 pixel draw: 4 pixels × 2 bytes = 8 bytes of pixel data */
        uint8_t buf[sizeof(display_draw_payload_t) + 8];
        display_draw_payload_t *req = (display_draw_payload_t *)buf;
        req->x = 5;
        req->y = 5;
        req->w = 2;
        req->h = 2;
        uint16_t *pixels = (uint16_t *)req->pixels;
        pixels[0] = 0xF800; /* (5,5) red */
        pixels[1] = 0x07E0; /* (6,5) green */
        pixels[2] = 0x001F; /* (5,6) blue */
        pixels[3] = 0xFFFF; /* (6,6) white */
        actor_send(rt, s->display_id, MSG_DISPLAY_DRAW, buf, sizeof(buf));
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

static int test_draw(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, draw_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    ASSERT_EQ(display_mock_get_pixel(5, 5), 0xF800);
    ASSERT_EQ(display_mock_get_pixel(6, 5), 0x07E0);
    ASSERT_EQ(display_mock_get_pixel(5, 6), 0x001F);
    ASSERT_EQ(display_mock_get_pixel(6, 6), 0xFFFF);

    runtime_destroy(rt);
    return 0;
}

/* ── test_draw_out_of_bounds ───────────────────────────────────────── */

static bool draw_oob_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        /* Draw at (465, 465) with 2x2 — goes out of 466x466 bounds */
        uint8_t buf[sizeof(display_draw_payload_t) + 8];
        display_draw_payload_t *req = (display_draw_payload_t *)buf;
        req->x = 465;
        req->y = 465;
        req->w = 2;
        req->h = 2;
        uint16_t *pixels = (uint16_t *)req->pixels;
        pixels[0] = pixels[1] = pixels[2] = pixels[3] = 0xFFFF;
        actor_send(rt, s->display_id, MSG_DISPLAY_DRAW, buf, sizeof(buf));
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

static int test_draw_out_of_bounds(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, draw_oob_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "out of bounds") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_brightness ───────────────────────────────────────────────── */

static bool brightness_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        display_brightness_payload_t br = { .brightness = 128 };
        actor_send(rt, s->display_id, MSG_DISPLAY_BRIGHTNESS,
                   &br, sizeof(br));
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

static int test_brightness(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, brightness_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    ASSERT_EQ(display_mock_get_brightness(), 128);

    runtime_destroy(rt);
    return 0;
}

/* ── test_power ────────────────────────────────────────────────────── */

static bool power_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        display_power_payload_t pwr = { .on = 0 };
        actor_send(rt, s->display_id, MSG_DISPLAY_POWER, &pwr, sizeof(pwr));
        return true;
    }

    if (msg->type == MSG_DISPLAY_OK && s->step == 1) {
        s->step = 2;
        display_power_payload_t pwr = { .on = 1 };
        actor_send(rt, s->display_id, MSG_DISPLAY_POWER, &pwr, sizeof(pwr));
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

static int test_power(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, power_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    /* After off→on, display should be powered on */
    ASSERT(display_mock_is_powered());

    runtime_destroy(rt);
    return 0;
}

/* ── test_clear ────────────────────────────────────────────────────── */

static bool clear_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        /* First fill a region */
        display_fill_payload_t fill = {
            .x = 0, .y = 0, .w = 10, .h = 10,
            .color = 0xFFFF
        };
        actor_send(rt, s->display_id, MSG_DISPLAY_FILL, &fill, sizeof(fill));
        return true;
    }

    if (msg->type == MSG_DISPLAY_OK && s->step == 1) {
        s->step = 2;
        /* Then clear */
        actor_send(rt, s->display_id, MSG_DISPLAY_CLEAR, NULL, 0);
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

static int test_clear(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, clear_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* After clear, previously filled pixels should be black */
    ASSERT_EQ(display_mock_get_pixel(0, 0), 0x0000);
    ASSERT_EQ(display_mock_get_pixel(5, 5), 0x0000);

    runtime_destroy(rt);
    return 0;
}

/* ── test_fill_out_of_bounds ───────────────────────────────────────── */

static bool fill_oob_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    display_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        display_fill_payload_t fill = {
            .x = 460, .y = 460, .w = 10, .h = 10,
            .color = 0xFFFF
        };
        actor_send(rt, s->display_id, MSG_DISPLAY_FILL, &fill, sizeof(fill));
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

static int test_fill_out_of_bounds(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    display_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, fill_oob_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "out of bounds") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_display:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_fill);
    RUN_TEST(test_draw);
    RUN_TEST(test_draw_out_of_bounds);
    RUN_TEST(test_brightness);
    RUN_TEST(test_power);
    RUN_TEST(test_clear);
    RUN_TEST(test_fill_out_of_bounds);

    TEST_REPORT();
}
