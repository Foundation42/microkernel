#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/display.h"
#include "microkernel/dashboard.h"
#include "display_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Helper: pump runtime for N steps ─────────────────────────────── */

static void pump(runtime_t *rt, int steps) {
    for (int i = 0; i < steps; i++)
        runtime_step(rt);
}

/* ── test_init: spawn display + dashboard, verify registered ───────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);
    ASSERT_NE(did, ACTOR_ID_INVALID);

    actor_id_t dash_id = dashboard_actor_init(rt);
    ASSERT_NE(dash_id, ACTOR_ID_INVALID);

    ASSERT_EQ(actor_lookup(rt, "/sys/dashboard"), dash_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_renders_on_bootstrap: verify header pixels after bootstrap ── */

static int test_renders_on_bootstrap(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);
    ASSERT_NE(did, ACTOR_ID_INVALID);

    actor_id_t dash_id = dashboard_actor_init(rt);
    ASSERT_NE(dash_id, ACTOR_ID_INVALID);

    /* Pump runtime to process bootstrap + display messages */
    pump(rt, 100);

    /* Header bar: fill rect at (0,0) 466x16 with COLOR_HEADER_BG.
       COLOR_HEADER_BG = RGB565(0x00, 0x40, 0x60) = 0x0030.
       But text rendering overwrites parts of it.
       Pixel at far right (465, 0) should have header bg or text. */
    uint16_t header_bg = display_mock_get_pixel(460, 0);
    /* Should not be all black (0x0000) — header was rendered */
    ASSERT_NE(header_bg, 0x0000);

    /* Text "MICROKERNEL" at (8, 0) — check a pixel in glyph area.
       'M' glyph row 2 = 0xEE → pixel at (8, 2) should be non-zero (accent color). */
    uint16_t text_px = display_mock_get_pixel(8, 2);
    ASSERT_NE(text_px, 0x0000);

    runtime_destroy(rt);
    return 0;
}

/* ── test_actor_list: spawn named actors, verify actor list renders ── */

static bool dummy_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

static int test_actor_list(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t did = display_actor_init(rt);
    ASSERT_NE(did, ACTOR_ID_INVALID);

    /* Spawn 3 named dummy actors */
    static int dummy_state;
    actor_id_t a1 = actor_spawn(rt, dummy_behavior, &dummy_state, NULL, 8);
    actor_register_name(rt, "actor-a", a1);
    actor_id_t a2 = actor_spawn(rt, dummy_behavior, &dummy_state, NULL, 8);
    actor_register_name(rt, "actor-b", a2);
    actor_id_t a3 = actor_spawn(rt, dummy_behavior, &dummy_state, NULL, 8);
    actor_register_name(rt, "actor-c", a3);

    actor_id_t dash_id = dashboard_actor_init(rt);
    ASSERT_NE(dash_id, ACTOR_ID_INVALID);

    /* Pump runtime to process bootstrap + all display messages */
    pump(rt, 200);

    /* Actor list section header at row 9 → y=144.
       "ACTORS" text rendered in accent color.
       Check a pixel in that region is non-zero. */
    uint16_t actors_px = display_mock_get_pixel(DISPLAY_COL(1), DISPLAY_ROW(9) + 2);
    ASSERT_NE(actors_px, 0x0000);

    /* Actor rows at row 10+ → y=160+.
       Some pixel in row 10 area should have text (off-white). */
    uint16_t row10_px = display_mock_get_pixel(DISPLAY_COL(2), DISPLAY_ROW(10) + 2);
    ASSERT_NE(row10_px, 0x0000);

    runtime_destroy(rt);
    return 0;
}

/* ── test_no_display_returns_invalid ───────────────────────────────── */

static int test_no_display_returns_invalid(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    /* Do NOT init display actor */

    actor_id_t dash_id = dashboard_actor_init(rt);
    ASSERT_EQ(dash_id, ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_dashboard:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_renders_on_bootstrap);
    RUN_TEST(test_actor_list);
    RUN_TEST(test_no_display_returns_invalid);

    TEST_REPORT();
}
