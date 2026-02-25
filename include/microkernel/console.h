#ifndef MICROKERNEL_CONSOLE_H
#define MICROKERNEL_CONSOLE_H

#include "types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* Console message types (defined in services.h):
 *   MSG_CONSOLE_WRITE  0xFF000060  → console  raw bytes (may contain ANSI)
 *   MSG_CONSOLE_CLEAR  0xFF000061  → console  (empty) — clear + home
 *   0xFF000062–0xFF00006F reserved for console extensions
 */

/* ── Console grid dimensions ─────────────────────────────────────────── */
/* Default: 466 / 8 = 58 columns, 466 / 16 = 29 rows (AMOLED 1.43")
 * Actual dimensions are computed at runtime from display size.
 * Max: 800 / 8 = 100 columns, 480 / 16 = 30 rows (LCD 4.3B) */

#define CONSOLE_COLS     58
#define CONSOLE_ROWS     29
#define CONSOLE_MAX_COLS 100
#define CONSOLE_MAX_ROWS 32

/* ── Cell structure ──────────────────────────────────────────────────── */

typedef struct {
    uint8_t  ch;
    uint8_t  _pad;
    uint16_t fg;      /* RGB565 */
    uint16_t bg;      /* RGB565 */
} console_cell_t;     /* 6 bytes */

/* ── Convenience functions ───────────────────────────────────────────── */

/*
 * Spawn the console actor.  Registers as "/sys/console".
 * Requires the display actor at "/node/hardware/display".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t mk_console_actor_init(runtime_t *rt);

/* Write raw bytes to the console (may contain ANSI escape sequences).
   Returns true if the message was enqueued. */
bool mk_console_write(runtime_t *rt, const char *text, size_t len);

/* printf-style write to the console. */
bool mk_console_printf(runtime_t *rt, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Clear the console screen and home cursor. */
bool mk_console_clear(runtime_t *rt);

/* Get actual console dimensions (may differ from CONSOLE_COLS/ROWS).
   Returns true if the console is initialized. */
bool mk_console_get_size(int *cols, int *rows);

/* ── Test helpers (Linux only) ───────────────────────────────────────── */

#ifndef ESP_PLATFORM
/* Read a cell from the console grid.  Returns pointer to cell, or NULL. */
const console_cell_t *mk_console_get_cell(int row, int col);

/* Get current cursor position. Returns false if console not initialized. */
bool mk_console_get_cursor(int *row, int *col);
#endif

#endif /* MICROKERNEL_CONSOLE_H */
