#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/led.h"
#include "led_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t led_id;
    int        step;
    bool       done;

    msg_type_t last_type;
    uint8_t    last_payload[256];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_error;
    int        ok_count;
} led_tester_t;

static void save_reply(led_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload)
                          ? msg->payload_size : sizeof(s->last_payload);
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);

    if (msg->type == MSG_LED_OK) {
        s->got_ok = true;
        s->ok_count++;
    }
    if (msg->type == MSG_LED_ERROR) s->got_error = true;
}

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t led_id = led_actor_init(rt);
    ASSERT_NE(led_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/node/hardware/led"), led_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_configure ────────────────────────────────────────────────── */

static bool configure_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    led_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        led_config_payload_t cfg = { .pin = 8, .num_leds = 1 };
        actor_send(rt, s->led_id, MSG_LED_CONFIGURE, &cfg, sizeof(cfg));
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
    actor_id_t led_id = led_actor_init(rt);

    led_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.led_id = led_id;

    actor_id_t tester = actor_spawn(rt, configure_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);
    ASSERT(led_mock_is_configured());
    ASSERT_EQ(led_mock_get_num_leds(), 1);

    runtime_destroy(rt);
    return 0;
}

/* ── test_set_pixel ────────────────────────────────────────────────── */

static bool set_pixel_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    led_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        led_config_payload_t cfg = { .pin = 8, .num_leds = 1 };
        actor_send(rt, s->led_id, MSG_LED_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 1) {
        s->step = 2;
        led_pixel_payload_t px = { .index = 0, .r = 255, .g = 0, .b = 0 };
        actor_send(rt, s->led_id, MSG_LED_SET_PIXEL, &px, sizeof(px));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 2) {
        s->step = 3;
        /* Now flush with SHOW */
        actor_send(rt, s->led_id, MSG_LED_SHOW, NULL, 0);
        return true;
    }

    if (s->step == 3) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_set_pixel(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t led_id = led_actor_init(rt);

    led_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.led_id = led_id;

    actor_id_t tester = actor_spawn(rt, set_pixel_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* Verify mock: pixel 0 should be (255,0,0) after SHOW with brightness=255 */
    uint8_t r, g, b;
    led_mock_get_pixel(0, &r, &g, &b);
    ASSERT_EQ(r, 255);
    ASSERT_EQ(g, 0);
    ASSERT_EQ(b, 0);
    ASSERT_EQ(led_mock_get_show_count(), 1);

    runtime_destroy(rt);
    return 0;
}

/* ── test_set_all ──────────────────────────────────────────────────── */

static bool set_all_tester(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)self;
    led_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        led_config_payload_t cfg = { .pin = 8, .num_leds = 3 };
        actor_send(rt, s->led_id, MSG_LED_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 1) {
        s->step = 2;
        /* SET_ALL: 3 pixels × 3 bytes = 9 bytes of pixel data */
        uint8_t buf[sizeof(led_all_payload_t) + 9];
        led_all_payload_t *req = (led_all_payload_t *)buf;
        req->num_leds = 3;
        req->pixels[0] = 10; req->pixels[1] = 20; req->pixels[2] = 30;  /* px 0 */
        req->pixels[3] = 40; req->pixels[4] = 50; req->pixels[5] = 60;  /* px 1 */
        req->pixels[6] = 70; req->pixels[7] = 80; req->pixels[8] = 90;  /* px 2 */
        actor_send(rt, s->led_id, MSG_LED_SET_ALL, buf, sizeof(buf));
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

static int test_set_all(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t led_id = led_actor_init(rt);

    led_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.led_id = led_id;

    actor_id_t tester = actor_spawn(rt, set_all_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* SET_ALL auto-flushes, so mock should have the scaled pixels */
    uint8_t r, g, b;
    led_mock_get_pixel(0, &r, &g, &b);
    ASSERT_EQ(r, 10); ASSERT_EQ(g, 20); ASSERT_EQ(b, 30);
    led_mock_get_pixel(1, &r, &g, &b);
    ASSERT_EQ(r, 40); ASSERT_EQ(g, 50); ASSERT_EQ(b, 60);
    led_mock_get_pixel(2, &r, &g, &b);
    ASSERT_EQ(r, 70); ASSERT_EQ(g, 80); ASSERT_EQ(b, 90);

    runtime_destroy(rt);
    return 0;
}

/* ── test_brightness ───────────────────────────────────────────────── */

static bool brightness_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    led_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        led_config_payload_t cfg = { .pin = 8, .num_leds = 1 };
        actor_send(rt, s->led_id, MSG_LED_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 1) {
        s->step = 2;
        led_pixel_payload_t px = { .index = 0, .r = 255, .g = 255, .b = 255 };
        actor_send(rt, s->led_id, MSG_LED_SET_PIXEL, &px, sizeof(px));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 2) {
        s->step = 3;
        led_brightness_payload_t br = { .brightness = 128 };
        actor_send(rt, s->led_id, MSG_LED_SET_BRIGHTNESS, &br, sizeof(br));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 3) {
        s->step = 4;
        actor_send(rt, s->led_id, MSG_LED_SHOW, NULL, 0);
        return true;
    }

    if (s->step == 4) {
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
    actor_id_t led_id = led_actor_init(rt);

    led_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.led_id = led_id;

    actor_id_t tester = actor_spawn(rt, brightness_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* (255 * 128) / 255 = 128 */
    uint8_t r, g, b;
    led_mock_get_pixel(0, &r, &g, &b);
    ASSERT_EQ(r, 128);
    ASSERT_EQ(g, 128);
    ASSERT_EQ(b, 128);

    runtime_destroy(rt);
    return 0;
}

/* ── test_clear ────────────────────────────────────────────────────── */

static bool clear_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    led_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        led_config_payload_t cfg = { .pin = 8, .num_leds = 1 };
        actor_send(rt, s->led_id, MSG_LED_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 1) {
        s->step = 2;
        led_pixel_payload_t px = { .index = 0, .r = 100, .g = 200, .b = 50 };
        actor_send(rt, s->led_id, MSG_LED_SET_PIXEL, &px, sizeof(px));
        return true;
    }

    if (msg->type == MSG_LED_OK && s->step == 2) {
        s->step = 3;
        actor_send(rt, s->led_id, MSG_LED_CLEAR, NULL, 0);
        return true;
    }

    if (s->step == 3) {
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
    actor_id_t led_id = led_actor_init(rt);

    led_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.led_id = led_id;

    actor_id_t tester = actor_spawn(rt, clear_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* CLEAR auto-flushes with zeros */
    uint8_t r, g, b;
    led_mock_get_pixel(0, &r, &g, &b);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(g, 0);
    ASSERT_EQ(b, 0);

    runtime_destroy(rt);
    return 0;
}

/* ── test_error_unconfigured ───────────────────────────────────────── */

static bool unconfigured_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    led_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        led_pixel_payload_t px = { .index = 0, .r = 255, .g = 0, .b = 0 };
        actor_send(rt, s->led_id, MSG_LED_SET_PIXEL, &px, sizeof(px));
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
    actor_id_t led_id = led_actor_init(rt);

    led_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.led_id = led_id;

    actor_id_t tester = actor_spawn(rt, unconfigured_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "not configured") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_led:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_configure);
    RUN_TEST(test_set_pixel);
    RUN_TEST(test_set_all);
    RUN_TEST(test_brightness);
    RUN_TEST(test_clear);
    RUN_TEST(test_error_unconfigured);

    TEST_REPORT();
}
