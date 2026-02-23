#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/display.h"
#include "microkernel/console.h"
#include "display_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Helper: pump runtime for N steps ─────────────────────────────────── */

static void pump(runtime_t *rt, int steps) {
    for (int i = 0; i < steps; i++)
        runtime_step(rt);
}

/* ── test_init: spawn display + console, verify /sys/console registered ── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t did = display_actor_init(rt);
    ASSERT_NE(did, ACTOR_ID_INVALID);

    actor_id_t cid = mk_console_actor_init(rt);
    ASSERT_NE(cid, ACTOR_ID_INVALID);

    ASSERT_EQ(actor_lookup(rt, "/sys/console"), cid);

    runtime_destroy(rt);
    return 0;
}

/* ── test_write: write "AB", verify grid cells and cursor ─────────────── */

static int test_write(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    actor_id_t cid = mk_console_actor_init(rt);
    ASSERT_NE(cid, ACTOR_ID_INVALID);

    mk_console_write(rt, "AB", 2);
    pump(rt, 20);

    const console_cell_t *a = mk_console_get_cell(0, 0);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->ch, 'A');

    const console_cell_t *b = mk_console_get_cell(0, 1);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ(b->ch, 'B');

    int row, col;
    ASSERT(mk_console_get_cursor(&row, &col));
    ASSERT_EQ(row, 0);
    ASSERT_EQ(col, 2);

    runtime_destroy(rt);
    return 0;
}

/* ── test_cursor_move: send \e[3;5H, verify cursor at (2,4) 0-based ──── */

static int test_cursor_move(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    mk_console_actor_init(rt);

    mk_console_write(rt, "\033[3;5H", 6);
    pump(rt, 20);

    int row, col;
    ASSERT(mk_console_get_cursor(&row, &col));
    ASSERT_EQ(row, 2);
    ASSERT_EQ(col, 4);

    runtime_destroy(rt);
    return 0;
}

/* ── test_colors: send \e[31mX, verify fg = red ──────────────────────── */

static int test_colors(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    mk_console_actor_init(rt);

    /* \e[31m = set fg to red (palette index 1) */
    mk_console_write(rt, "\033[31mX", 6);
    pump(rt, 20);

    const console_cell_t *cell = mk_console_get_cell(0, 0);
    ASSERT_NOT_NULL(cell);
    ASSERT_EQ(cell->ch, 'X');

    /* Red = RGB565(0xAA, 0x00, 0x00) */
    uint16_t expected_red = RGB565(0xAA, 0x00, 0x00);
    ASSERT_EQ(cell->fg, expected_red);

    runtime_destroy(rt);
    return 0;
}

/* ── test_newline_scroll: fill row 28, write \n + Z ──────────────────── */

static int test_newline_scroll(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    mk_console_actor_init(rt);

    /* Position cursor at row 28 (last row), col 0 */
    mk_console_write(rt, "\033[29;1H", 7);
    pump(rt, 10);

    /* Write "Y" at row 28 */
    mk_console_write(rt, "Y", 1);
    pump(rt, 10);

    /* Newline should trigger scroll, then write "Z" on the new last row */
    mk_console_write(rt, "\nZ", 2);
    pump(rt, 20);

    /* After scroll, "Y" should have moved up to row 27 */
    const console_cell_t *y_cell = mk_console_get_cell(27, 0);
    ASSERT_NOT_NULL(y_cell);
    ASSERT_EQ(y_cell->ch, 'Y');

    /* "Z" should be on row 28, col 0 */
    const console_cell_t *z_cell = mk_console_get_cell(28, 0);
    ASSERT_NOT_NULL(z_cell);
    ASSERT_EQ(z_cell->ch, 'Z');

    runtime_destroy(rt);
    return 0;
}

/* ── test_clear: write text, then \e[2J, verify all cells are spaces ──── */

static int test_clear(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    mk_console_actor_init(rt);

    /* Write some text */
    mk_console_write(rt, "Hello World", 11);
    pump(rt, 20);

    /* Clear screen */
    mk_console_write(rt, "\033[2J", 4);
    pump(rt, 20);

    /* Default colors */
    uint16_t def_fg = RGB565(0xAA, 0xAA, 0xAA); /* white (7) */
    uint16_t def_bg = RGB565(0x00, 0x00, 0x00);  /* black (0) */

    /* All cells should be spaces with default colors */
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        for (int c = 0; c < CONSOLE_COLS; c++) {
            const console_cell_t *cell = mk_console_get_cell(r, c);
            ASSERT_NOT_NULL(cell);
            ASSERT_EQ(cell->ch, ' ');
            ASSERT_EQ(cell->fg, def_fg);
            ASSERT_EQ(cell->bg, def_bg);
        }
    }

    /* Cursor at home */
    int row, col;
    ASSERT(mk_console_get_cursor(&row, &col));
    ASSERT_EQ(row, 0);
    ASSERT_EQ(col, 0);

    runtime_destroy(rt);
    return 0;
}

/* ── test_clear_eol: position at col 3, send \e[K ────────────────────── */

static int test_clear_eol(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    mk_console_actor_init(rt);

    /* Write "ABCDEF" at row 0 */
    mk_console_write(rt, "ABCDEF", 6);
    pump(rt, 20);

    /* Move cursor to col 3, then clear to end of line */
    mk_console_write(rt, "\033[1;4H\033[K", 9);
    pump(rt, 20);

    /* Cols 0-2 should be preserved */
    ASSERT_EQ(mk_console_get_cell(0, 0)->ch, 'A');
    ASSERT_EQ(mk_console_get_cell(0, 1)->ch, 'B');
    ASSERT_EQ(mk_console_get_cell(0, 2)->ch, 'C');

    /* Cols 3-57 should be cleared to space */
    ASSERT_EQ(mk_console_get_cell(0, 3)->ch, ' ');
    ASSERT_EQ(mk_console_get_cell(0, 4)->ch, ' ');
    ASSERT_EQ(mk_console_get_cell(0, 57)->ch, ' ');

    runtime_destroy(rt);
    return 0;
}

/* ── test_tab: send \tX, verify cursor jumped to col 8 ────────────────── */

static int test_tab(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    display_actor_init(rt);
    mk_console_actor_init(rt);

    mk_console_write(rt, "\tX", 2);
    pump(rt, 20);

    /* Tab from col 0 should jump to col 8 */
    const console_cell_t *x_cell = mk_console_get_cell(0, 8);
    ASSERT_NOT_NULL(x_cell);
    ASSERT_EQ(x_cell->ch, 'X');

    int row, col;
    ASSERT(mk_console_get_cursor(&row, &col));
    ASSERT_EQ(row, 0);
    ASSERT_EQ(col, 9); /* cursor after 'X' */

    runtime_destroy(rt);
    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_console:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_write);
    RUN_TEST(test_cursor_move);
    RUN_TEST(test_colors);
    RUN_TEST(test_newline_scroll);
    RUN_TEST(test_clear);
    RUN_TEST(test_clear_eol);
    RUN_TEST(test_tab);

    TEST_REPORT();
}
