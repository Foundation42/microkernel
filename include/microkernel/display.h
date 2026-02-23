#ifndef MICROKERNEL_DISPLAY_H
#define MICROKERNEL_DISPLAY_H

#include "types.h"
#include <stdint.h>

/* Display message types (defined in services.h):
 *   MSG_DISPLAY_DRAW       0xFF000051  → display  display_draw_payload_t (flex array)
 *   MSG_DISPLAY_FILL       0xFF000052  → display  display_fill_payload_t
 *   MSG_DISPLAY_CLEAR      0xFF000053  → display  (empty)
 *   MSG_DISPLAY_BRIGHTNESS 0xFF000054  → display  display_brightness_payload_t
 *   MSG_DISPLAY_POWER      0xFF000055  → display  display_power_payload_t
 *   MSG_DISPLAY_TEXT       0xFF000056  → display  display_text_payload_t (flex array)
 *   MSG_DISPLAY_OK         0xFF00005C  ← display  (empty)
 *   MSG_DISPLAY_ERROR      0xFF00005D  ← display  error string
 */

/* ── Payload structs ──────────────────────────────────────────────── */

typedef struct {
    uint16_t x, y, w, h;
    uint8_t  pixels[];   /* RGB565 pixel data, length = w * h * 2 */
} display_draw_payload_t;

typedef struct {
    uint16_t x, y, w, h;
    uint16_t color;      /* RGB565 */
    uint16_t _pad;
} display_fill_payload_t;

typedef struct {
    uint8_t brightness;  /* 0–255 */
} display_brightness_payload_t;

typedef struct {
    uint8_t on;          /* 0 = off, 1 = on */
} display_power_payload_t;

typedef struct {
    uint16_t x, y;       /* pixel position */
    uint16_t fg, bg;     /* RGB565 colors */
    char     text[];     /* null-terminated */
} display_text_payload_t;

/* Per-cell colored text (sent by console actor, one per dirty row) */
typedef struct {
    uint8_t  ch;
    uint8_t  _pad;
    uint16_t fg;         /* RGB565 */
    uint16_t bg;         /* RGB565 */
} display_text_attr_cell_t;  /* 6 bytes */

typedef struct {
    uint16_t x, y;       /* pixel position */
    uint16_t count;      /* number of cells */
    uint16_t _pad;
    display_text_attr_cell_t cells[];
} display_text_attr_payload_t;

/* ── Character grid helpers ─────────────────────────────────────────── */

/* 8px wide, 16px tall glyphs */
#define DISPLAY_COL(c) ((uint16_t)((c) * 8))
#define DISPLAY_ROW(r) ((uint16_t)((r) * 16))

/* RGB565 color macro */
#define RGB565(r, g, b) \
    ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) >> 3)))

/* ── Convenience functions ──────────────────────────────────────────── */

/* Send text to the display actor via /node/hardware/display.
   Returns true if the message was enqueued. */
bool display_text(runtime_t *rt, uint16_t x, uint16_t y,
                  uint16_t fg, uint16_t bg, const char *text);

/* Send fill rect to display actor via /node/hardware/display. */
bool display_fill_rect(runtime_t *rt, uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h, uint16_t color);

/* Send clear to display actor via /node/hardware/display. */
bool display_clear_screen(runtime_t *rt);

/*
 * Spawn the display actor.  Registers as "/node/hardware/display".
 * Auto-configures on init (pins are fixed per board).
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t display_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_DISPLAY_H */
