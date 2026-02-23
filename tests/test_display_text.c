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
    bool       got_ok;
    bool       got_error;
} text_tester_t;

static void save_reply(text_tester_t *s, message_t *msg) {
    if (msg->type == MSG_DISPLAY_OK)    s->got_ok = true;
    if (msg->type == MSG_DISPLAY_ERROR) s->got_error = true;
}

/* ── test_text: render "AB" at (0,0), verify glyph pixels ──────────── */

static bool text_render_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    text_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        /* Build text payload for "AB" */
        uint8_t buf[sizeof(display_text_payload_t) + 4];
        display_text_payload_t *req = (display_text_payload_t *)buf;
        req->x = 0;
        req->y = 0;
        req->fg = 0xFFFF; /* white */
        req->bg = 0x0000; /* black */
        memcpy(req->text, "AB", 3); /* include null */
        actor_send(rt, s->display_id, MSG_DISPLAY_TEXT, buf,
                   sizeof(display_text_payload_t) + 3);
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_text(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    text_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, text_render_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* 'A' glyph (font_8x16_data[33]): row 2 = 0x10, row 3 = 0x38
       'A' is ASCII 65, index 65-32 = 33.
       Row 2 has bit 0x10 set → pixel at (3,2) should be white (fg).
       Row 0 is all zeros → pixel at (0,0) should be bg (black). */
    ASSERT_EQ(display_mock_get_pixel(0, 0), 0x0000); /* bg in row 0 */

    /* Somewhere in the 'A' glyph should have fg pixels.
       Row 5 = 0xC6 for 'A' → pixels at x=0,1 should be fg (bits 7,6 set) */
    uint16_t row5_px0 = display_mock_get_pixel(0, 5);
    uint16_t row5_px1 = display_mock_get_pixel(1, 5);
    ASSERT(row5_px0 == 0xFFFF || row5_px1 == 0xFFFF);

    /* 'B' starts at x=8. 'B' row 2 = 0xFC → pixel at (8,2) should be fg */
    uint16_t b_px = display_mock_get_pixel(8, 2);
    ASSERT(b_px == 0xFFFF);

    runtime_destroy(rt);
    return 0;
}

/* ── test_text_colors: verify fg/bg colors ─────────────────────────── */

static bool text_color_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    text_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        uint8_t buf[sizeof(display_text_payload_t) + 4];
        display_text_payload_t *req = (display_text_payload_t *)buf;
        req->x = 0;
        req->y = 0;
        req->fg = 0xF800; /* red */
        req->bg = 0x001F; /* blue */
        memcpy(req->text, "A", 2);
        actor_send(rt, s->display_id, MSG_DISPLAY_TEXT, buf,
                   sizeof(display_text_payload_t) + 2);
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_text_colors(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);

    text_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, text_color_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* Row 0 of 'A' is all zeros → should be bg (blue) */
    ASSERT_EQ(display_mock_get_pixel(0, 0), 0x001F);

    /* Row 5 of 'A' = 0xC6 → bits 7,6 set → pixels 0,1 should be fg (red) */
    ASSERT_EQ(display_mock_get_pixel(0, 5), 0xF800);
    ASSERT_EQ(display_mock_get_pixel(1, 5), 0xF800);

    runtime_destroy(rt);
    return 0;
}

/* ── test_text_convenience: use display_text() wrapper ─────────────── */

static bool text_conv_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    text_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        display_text(rt, 0, DISPLAY_ROW(2), 0xFFFF, 0x0000, "Hi");
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_text_convenience(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);
    ASSERT_NE(did, ACTOR_ID_INVALID);

    text_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.display_id = did;

    actor_id_t tester = actor_spawn(rt, text_conv_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    /* "Hi" at row 2 → y = 32. 'H' glyph row 2 = 0xC6 → pixel (0, 34) = fg */
    uint16_t px = display_mock_get_pixel(0, 34);
    ASSERT_EQ(px, 0xFFFF);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_display_text:\n");

    RUN_TEST(test_text);
    RUN_TEST(test_text_colors);
    RUN_TEST(test_text_convenience);

    TEST_REPORT();
}
