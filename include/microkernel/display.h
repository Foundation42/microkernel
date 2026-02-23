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

/*
 * Spawn the display actor.  Registers as "/node/hardware/display".
 * Auto-configures on init (pins are fixed per board).
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t display_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_DISPLAY_H */
