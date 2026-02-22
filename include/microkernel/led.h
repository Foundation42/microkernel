#ifndef MICROKERNEL_LED_H
#define MICROKERNEL_LED_H

#include "types.h"
#include <stdint.h>

/* LED message types (defined in services.h):
 *   MSG_LED_CONFIGURE      0xFF000040  → led  led_config_payload_t
 *   MSG_LED_SET_PIXEL      0xFF000041  → led  led_pixel_payload_t
 *   MSG_LED_SET_ALL        0xFF000042  → led  led_all_payload_t (flex array)
 *   MSG_LED_SET_BRIGHTNESS 0xFF000043  → led  led_brightness_payload_t
 *   MSG_LED_CLEAR          0xFF000044  → led  (empty)
 *   MSG_LED_SHOW           0xFF000045  → led  (empty)
 *   MSG_LED_OK             0xFF00004C  ← led  (empty)
 *   MSG_LED_ERROR          0xFF00004D  ← led  error string
 */

/* ── Payload structs ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  pin;
    uint8_t  _pad;
    uint16_t num_leds;
} led_config_payload_t;

typedef struct {
    uint16_t index;
    uint8_t  r, g, b;
    uint8_t  _pad;
} led_pixel_payload_t;

typedef struct {
    uint8_t brightness;   /* 0–255, default 255 */
} led_brightness_payload_t;

typedef struct {
    uint16_t num_leds;
    uint8_t  pixels[];    /* R,G,B triplets, length = num_leds * 3 */
} led_all_payload_t;

/*
 * Spawn the LED actor.  Registers as "/node/hardware/led".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t led_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_LED_H */
