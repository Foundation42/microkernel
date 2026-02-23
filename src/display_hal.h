#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize display HAL.  On ESP32 this does full QSPI + SH8601 setup.
   Returns true on success. */
bool display_hal_init(void);

/* Shutdown display HAL. */
void display_hal_deinit(void);

/* Draw raw RGB565 pixel data to a rectangular region. */
bool display_hal_draw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t *data);

/* Fill a rectangular region with a single RGB565 color.
   Uses row-at-a-time strategy to avoid large allocations. */
bool display_hal_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color);

/* Clear the entire display (fill black). */
bool display_hal_clear(void);

/* Set backlight / AMOLED brightness (0–255). */
bool display_hal_set_brightness(uint8_t level);

/* Power on/off the display. */
bool display_hal_power(bool on);

/* Get display dimensions. */
uint16_t display_hal_width(void);
uint16_t display_hal_height(void);

/* ── Test helpers (Linux mock only) ───────────────────────────────── */

#ifndef ESP_PLATFORM
/* Get a pixel's RGB565 value at (x, y).  Returns 0 if out of bounds. */
uint16_t display_mock_get_pixel(uint16_t x, uint16_t y);

/* Check if the display is initialized. */
bool display_mock_is_initialized(void);

/* Get the current brightness level. */
uint8_t display_mock_get_brightness(void);

/* Check if the display is powered on. */
bool display_mock_is_powered(void);
#endif

#endif /* DISPLAY_HAL_H */
