#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/display.h"
#include "microkernel/console.h"
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
    actor_id_t cid = mk_console_actor_init(rt);
    ASSERT_NE(cid, ACTOR_ID_INVALID);

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
    actor_id_t cid = mk_console_actor_init(rt);
    ASSERT_NE(cid, ACTOR_ID_INVALID);

    actor_id_t dash_id = dashboard_actor_init(rt);
    ASSERT_NE(dash_id, ACTOR_ID_INVALID);

    /* Pump runtime to process bootstrap + console + display messages.
       Dashboard → console → display pipeline requires more steps. */
    pump(rt, 300);

    /* Dashboard now renders via ANSI console at circular margin offsets.
       Row 5 (y=80..95): node info text at margin=6 cols (x=48+).
       Scan for any non-zero pixel in that row to confirm rendering worked. */
    bool found_text = false;
    for (int x = 48; x < 200; x += 8) {
        for (int y = 80; y < 96; y += 4) {
            if (display_mock_get_pixel((uint16_t)x, (uint16_t)y) != 0x0000) {
                found_text = true;
                break;
            }
        }
        if (found_text) break;
    }
    ASSERT(found_text);

    /* Row 3 (y=48..63): "MICROKERNEL" header in bright cyan.
       Check any pixel in that row area is non-zero. */
    bool found_header = false;
    for (int x = 80; x < 400; x += 8) {
        for (int y = 48; y < 64; y += 4) {
            if (display_mock_get_pixel((uint16_t)x, (uint16_t)y) != 0x0000) {
                found_header = true;
                break;
            }
        }
        if (found_header) break;
    }
    ASSERT(found_header);

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
    actor_id_t cid = mk_console_actor_init(rt);
    ASSERT_NE(cid, ACTOR_ID_INVALID);

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

    /* Pump runtime to process bootstrap + console + display messages */
    pump(rt, 400);

    /* Actor heading at console row 13 (y=208..223), margin=0.
       Scan for non-zero pixel confirming "ACTORS" rendered. */
    bool found_actors = false;
    for (int x = 0; x < 200; x += 8) {
        for (int y = 208; y < 224; y += 4) {
            if (display_mock_get_pixel((uint16_t)x, (uint16_t)y) != 0x0000) {
                found_actors = true;
                break;
            }
        }
        if (found_actors) break;
    }
    ASSERT(found_actors);

    /* Actor list at row 14+ (y=224+), margin=0.
       Scan for non-zero pixel in first actor row. */
    bool found_actor_row = false;
    for (int x = 0; x < 200; x += 8) {
        for (int y = 224; y < 240; y += 4) {
            if (display_mock_get_pixel((uint16_t)x, (uint16_t)y) != 0x0000) {
                found_actor_row = true;
                break;
            }
        }
        if (found_actor_row) break;
    }
    ASSERT(found_actor_row);

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
