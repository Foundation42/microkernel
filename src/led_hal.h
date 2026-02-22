#ifndef LED_HAL_H
#define LED_HAL_H

#include <stdbool.h>
#include <stdint.h>

#define LED_MAX_LEDS 256

/* Initialize LED HAL. Returns true on success. */
bool led_hal_init(void);

/* Shutdown LED HAL. */
void led_hal_deinit(void);

/* Configure the LED strip on a given pin with num_leds pixels. */
bool led_hal_configure(int pin, int num_leds);

/* Deconfigure (release) the LED strip. */
void led_hal_deconfigure(void);

/* Set a single pixel (raw, pre-brightness). Returns true on success. */
bool led_hal_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);

/* Flush pixel buffer to hardware. Returns true on success. */
bool led_hal_show(void);

/* Clear all pixels and flush. Returns true on success. */
bool led_hal_clear(void);

/* ── Test helpers (Linux mock only) ───────────────────────────────── */

#ifndef ESP_PLATFORM
/* Get a pixel's current color (raw, pre-brightness). */
void led_mock_get_pixel(int index, uint8_t *r, uint8_t *g, uint8_t *b);

/* Get the number of show() calls since configure. */
int led_mock_get_show_count(void);

/* Check if the strip is configured. */
bool led_mock_is_configured(void);

/* Get the number of configured LEDs. */
int led_mock_get_num_leds(void);
#endif

#endif /* LED_HAL_H */
