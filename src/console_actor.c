#include "microkernel/console.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/display.h"
#include "text_render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ── ANSI parser states ──────────────────────────────────────────────── */

enum { ST_NORMAL, ST_ESC, ST_CSI };

/* ── Console state ───────────────────────────────────────────────────── */

typedef struct {
    console_cell_t grid[CONSOLE_ROWS][CONSOLE_COLS];
    uint16_t crow, ccol;        /* cursor position (0-based) */
    uint16_t cur_fg, cur_bg;    /* active ANSI color state */
    uint32_t dirty;             /* bitmask of dirty rows (29 bits) */
    bool bold;                  /* bold = use bright color variant */
    /* ANSI parser FSM */
    int pstate;
    int params[8];
    int nparam;
    uint16_t palette[16];       /* ANSI 16-color → RGB565 */
} console_state_t;

/* File-static pointer for test helpers (Linux only) */
#ifndef ESP_PLATFORM
static console_state_t *s_test_state;
#endif

/* ── ANSI Color Palette (RGB565) ─────────────────────────────────────── */

static void init_palette(uint16_t *pal) {
    pal[0]  = RGB565(0x00, 0x00, 0x00); /* black */
    pal[1]  = RGB565(0xAA, 0x00, 0x00); /* red */
    pal[2]  = RGB565(0x00, 0xAA, 0x00); /* green */
    pal[3]  = RGB565(0xAA, 0xAA, 0x00); /* yellow */
    pal[4]  = RGB565(0x00, 0x00, 0xAA); /* blue */
    pal[5]  = RGB565(0xAA, 0x00, 0xAA); /* magenta */
    pal[6]  = RGB565(0x00, 0xAA, 0xAA); /* cyan */
    pal[7]  = RGB565(0xAA, 0xAA, 0xAA); /* white */
    pal[8]  = RGB565(0x55, 0x55, 0x55); /* bright black */
    pal[9]  = RGB565(0xFF, 0x55, 0x55); /* bright red */
    pal[10] = RGB565(0x55, 0xFF, 0x55); /* bright green */
    pal[11] = RGB565(0xFF, 0xFF, 0x55); /* bright yellow */
    pal[12] = RGB565(0x55, 0x55, 0xFF); /* bright blue */
    pal[13] = RGB565(0xFF, 0x55, 0xFF); /* bright magenta */
    pal[14] = RGB565(0x55, 0xFF, 0xFF); /* bright cyan */
    pal[15] = RGB565(0xFF, 0xFF, 0xFF); /* bright white */
}

/* Default colors: white (7) on black (0) */
#define DEFAULT_FG_IDX 7
#define DEFAULT_BG_IDX 0

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void clear_grid(console_state_t *cs) {
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        for (int c = 0; c < CONSOLE_COLS; c++) {
            cs->grid[r][c].ch = ' ';
            cs->grid[r][c]._pad = 0;
            cs->grid[r][c].fg = cs->palette[DEFAULT_FG_IDX];
            cs->grid[r][c].bg = cs->palette[DEFAULT_BG_IDX];
        }
    }
    cs->dirty = (uint32_t)((1u << CONSOLE_ROWS) - 1);
}

static void scroll_up(console_state_t *cs) {
    memmove(&cs->grid[0], &cs->grid[1],
            (CONSOLE_ROWS - 1) * sizeof(cs->grid[0]));
    /* Clear last row */
    for (int c = 0; c < CONSOLE_COLS; c++) {
        cs->grid[CONSOLE_ROWS - 1][c].ch = ' ';
        cs->grid[CONSOLE_ROWS - 1][c]._pad = 0;
        cs->grid[CONSOLE_ROWS - 1][c].fg = cs->palette[DEFAULT_FG_IDX];
        cs->grid[CONSOLE_ROWS - 1][c].bg = cs->palette[DEFAULT_BG_IDX];
    }
    cs->crow = CONSOLE_ROWS - 1;
    cs->dirty = (uint32_t)((1u << CONSOLE_ROWS) - 1);
}

static void put_char(console_state_t *cs, uint8_t ch) {
    if (cs->ccol >= CONSOLE_COLS) {
        /* Wrap to next line */
        cs->ccol = 0;
        cs->crow++;
        if (cs->crow >= CONSOLE_ROWS)
            scroll_up(cs);
    }

    console_cell_t *cell = &cs->grid[cs->crow][cs->ccol];
    uint16_t fg = cs->cur_fg;
    uint16_t bg = cs->cur_bg;

    if (cell->ch != ch || cell->fg != fg || cell->bg != bg) {
        cell->ch = ch;
        cell->fg = fg;
        cell->bg = bg;
        cs->dirty |= (1u << cs->crow);
    }
    cs->ccol++;
}

/* ── SGR (Select Graphic Rendition) ──────────────────────────────────── */

static void apply_sgr(console_state_t *cs, int code) {
    if (code == 0) {
        /* Reset */
        cs->bold = false;
        cs->cur_fg = cs->palette[DEFAULT_FG_IDX];
        cs->cur_bg = cs->palette[DEFAULT_BG_IDX];
    } else if (code == 1) {
        cs->bold = true;
        /* If current fg is standard (0-7), promote to bright */
        for (int i = 0; i < 8; i++) {
            if (cs->cur_fg == cs->palette[i]) {
                cs->cur_fg = cs->palette[i + 8];
                break;
            }
        }
    } else if (code >= 30 && code <= 37) {
        int idx = code - 30;
        cs->cur_fg = cs->palette[cs->bold ? idx + 8 : idx];
    } else if (code >= 40 && code <= 47) {
        cs->cur_bg = cs->palette[code - 40];
    } else if (code >= 90 && code <= 97) {
        cs->cur_fg = cs->palette[code - 90 + 8];
    } else if (code >= 100 && code <= 107) {
        cs->cur_bg = cs->palette[code - 100 + 8];
    }
}

/* ── CSI dispatch ────────────────────────────────────────────────────── */

static void dispatch_csi(console_state_t *cs, char final) {
    int p0 = (cs->nparam >= 1) ? cs->params[0] : 0;
    int p1 = (cs->nparam >= 2) ? cs->params[1] : 0;

    switch (final) {
    case 'H': /* Cursor position: \e[row;colH (1-based) */
    case 'f': {
        int row = (p0 > 0 ? p0 : 1) - 1;
        int col = (p1 > 0 ? p1 : 1) - 1;
        if (row >= CONSOLE_ROWS) row = CONSOLE_ROWS - 1;
        if (col >= CONSOLE_COLS) col = CONSOLE_COLS - 1;
        cs->crow = (uint16_t)row;
        cs->ccol = (uint16_t)col;
        break;
    }
    case 'A': /* Cursor up */
        if (p0 == 0) p0 = 1;
        cs->crow = (uint16_t)((int)cs->crow - p0 < 0 ? 0 : cs->crow - p0);
        break;
    case 'B': /* Cursor down */
        if (p0 == 0) p0 = 1;
        cs->crow = (uint16_t)(cs->crow + p0 >= CONSOLE_ROWS ?
                    CONSOLE_ROWS - 1 : cs->crow + p0);
        break;
    case 'C': /* Cursor forward */
        if (p0 == 0) p0 = 1;
        cs->ccol = (uint16_t)(cs->ccol + p0 >= CONSOLE_COLS ?
                    CONSOLE_COLS - 1 : cs->ccol + p0);
        break;
    case 'D': /* Cursor back */
        if (p0 == 0) p0 = 1;
        cs->ccol = (uint16_t)((int)cs->ccol - p0 < 0 ? 0 : cs->ccol - p0);
        break;
    case 'J': /* Erase in display */
        if (p0 == 2) {
            clear_grid(cs);
            cs->crow = 0;
            cs->ccol = 0;
        }
        break;
    case 'K': /* Erase in line */
        if (p0 == 0) {
            /* Clear from cursor to end of line */
            for (int c = cs->ccol; c < CONSOLE_COLS; c++) {
                cs->grid[cs->crow][c].ch = ' ';
                cs->grid[cs->crow][c].fg = cs->palette[DEFAULT_FG_IDX];
                cs->grid[cs->crow][c].bg = cs->palette[DEFAULT_BG_IDX];
            }
            cs->dirty |= (1u << cs->crow);
        }
        break;
    case 'm': /* SGR */
        if (cs->nparam == 0) {
            apply_sgr(cs, 0);
        } else {
            for (int i = 0; i < cs->nparam; i++)
                apply_sgr(cs, cs->params[i]);
        }
        break;
    default:
        break;
    }
}

/* ── ANSI parser ─────────────────────────────────────────────────────── */

static void console_feed(console_state_t *cs, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = data[i];

        switch (cs->pstate) {
        case ST_NORMAL:
            if (ch == 0x1B) {
                cs->pstate = ST_ESC;
            } else if (ch == '\n') {
                cs->crow++;
                cs->ccol = 0;
                if (cs->crow >= CONSOLE_ROWS)
                    scroll_up(cs);
            } else if (ch == '\r') {
                cs->ccol = 0;
            } else if (ch == '\t') {
                cs->ccol = (uint16_t)((cs->ccol + 8) & ~7);
                if (cs->ccol >= CONSOLE_COLS)
                    cs->ccol = CONSOLE_COLS - 1;
            } else if (ch == '\b') {
                if (cs->ccol > 0)
                    cs->ccol--;
            } else if (ch >= 0x20) {
                put_char(cs, ch);
            }
            break;

        case ST_ESC:
            if (ch == '[') {
                cs->pstate = ST_CSI;
                cs->nparam = 0;
                memset(cs->params, 0, sizeof(cs->params));
            } else {
                cs->pstate = ST_NORMAL;
            }
            break;

        case ST_CSI:
            if (ch >= '0' && ch <= '9') {
                if (cs->nparam == 0)
                    cs->nparam = 1;
                cs->params[cs->nparam - 1] =
                    cs->params[cs->nparam - 1] * 10 + (ch - '0');
            } else if (ch == ';') {
                if (cs->nparam < 8)
                    cs->nparam++;
            } else if (ch >= 0x40 && ch <= 0x7E) {
                dispatch_csi(cs, (char)ch);
                cs->pstate = ST_NORMAL;
            } else {
                /* Invalid CSI, bail */
                cs->pstate = ST_NORMAL;
            }
            break;
        }
    }
}

/* ── Flush dirty rows to display ─────────────────────────────────────── */

static void flush_dirty(runtime_t *rt, console_state_t *cs) {
    if (cs->dirty == 0) return;

    for (int row = 0; row < CONSOLE_ROWS; row++) {
        if (!(cs->dirty & (1u << row))) continue;

        size_t payload_size = sizeof(display_text_attr_payload_t) +
                              (size_t)CONSOLE_COLS * sizeof(display_text_attr_cell_t);

        /* Stack buffer: header (8) + 58 cells × 6 = 356 bytes */
        uint8_t buf[sizeof(display_text_attr_payload_t) +
                     CONSOLE_COLS * sizeof(display_text_attr_cell_t)];

        display_text_attr_payload_t *p = (display_text_attr_payload_t *)buf;
        p->x = DISPLAY_COL(0);
        p->y = DISPLAY_ROW(row);
        p->count = CONSOLE_COLS;
        p->_pad = 0;

        for (int c = 0; c < CONSOLE_COLS; c++) {
            p->cells[c].ch = cs->grid[row][c].ch;
            p->cells[c]._pad = 0;
            p->cells[c].fg = cs->grid[row][c].fg;
            p->cells[c].bg = cs->grid[row][c].bg;
        }

        actor_send_named(rt, "/node/hardware/display",
                         MSG_DISPLAY_TEXT_ATTR, buf, payload_size);
    }

    cs->dirty = 0;
}

/* ── Actor behavior ──────────────────────────────────────────────────── */

static bool console_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    console_state_t *cs = state;

    switch (msg->type) {
    case MSG_CONSOLE_WRITE:
        if (msg->payload && msg->payload_size > 0) {
            console_feed(cs, msg->payload, msg->payload_size);
            flush_dirty(rt, cs);
        }
        break;

    case MSG_CONSOLE_CLEAR:
        clear_grid(cs);
        cs->crow = 0;
        cs->ccol = 0;
        flush_dirty(rt, cs);
        break;

    default:
        break;
    }

    return true;
}

/* ── Cleanup ─────────────────────────────────────────────────────────── */

static void console_state_free(void *state) {
#ifndef ESP_PLATFORM
    if (s_test_state == state)
        s_test_state = NULL;
#endif
    free(state);
}

/* ── Init ────────────────────────────────────────────────────────────── */

actor_id_t mk_console_actor_init(runtime_t *rt) {
    /* Verify display actor exists */
    actor_id_t display_id = actor_lookup(rt, "/node/hardware/display");
    if (display_id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    console_state_t *cs = calloc(1, sizeof(*cs));
    if (!cs)
        return ACTOR_ID_INVALID;

    init_palette(cs->palette);
    cs->cur_fg = cs->palette[DEFAULT_FG_IDX];
    cs->cur_bg = cs->palette[DEFAULT_BG_IDX];

    /* Initialize grid with spaces */
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        for (int c = 0; c < CONSOLE_COLS; c++) {
            cs->grid[r][c].ch = ' ';
            cs->grid[r][c].fg = cs->cur_fg;
            cs->grid[r][c].bg = cs->cur_bg;
        }
    }

    actor_id_t id = actor_spawn(rt, console_behavior, cs,
                                console_state_free, 64);
    if (id == ACTOR_ID_INVALID) {
        free(cs);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/sys/console", id);

#ifndef ESP_PLATFORM
    s_test_state = cs;
#endif

    return id;
}

/* ── Convenience functions ───────────────────────────────────────────── */

bool mk_console_write(runtime_t *rt, const char *text, size_t len) {
    return actor_send_named(rt, "/sys/console",
                            MSG_CONSOLE_WRITE, text, len);
}

bool mk_console_printf(runtime_t *rt, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) return false;
    size_t len = (size_t)n;
    if (len > sizeof(buf) - 1)
        len = sizeof(buf) - 1;

    return mk_console_write(rt, buf, len);
}

bool mk_console_clear(runtime_t *rt) {
    return actor_send_named(rt, "/sys/console",
                            MSG_CONSOLE_CLEAR, NULL, 0);
}

/* ── Test helpers (Linux only) ───────────────────────────────────────── */

#ifndef ESP_PLATFORM
const console_cell_t *mk_console_get_cell(int row, int col) {
    if (!s_test_state) return NULL;
    if (row < 0 || row >= CONSOLE_ROWS || col < 0 || col >= CONSOLE_COLS)
        return NULL;
    return &s_test_state->grid[row][col];
}

bool mk_console_get_cursor(int *row, int *col) {
    if (!s_test_state) return false;
    *row = (int)s_test_state->crow;
    *col = (int)s_test_state->ccol;
    return true;
}
#endif
