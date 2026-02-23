#include "display_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Mock display state ───────────────────────────────────────────── */

#define MOCK_WIDTH  466
#define MOCK_HEIGHT 466

typedef struct {
    bool     initialized;
    bool     powered;
    uint8_t  brightness;
    uint16_t *pixels;    /* MOCK_WIDTH * MOCK_HEIGHT RGB565 values */
} mock_display_t;

static mock_display_t s_display;

/* ── HAL interface ────────────────────────────────────────────────── */

bool display_hal_init(void) {
    s_display.pixels = calloc((size_t)MOCK_WIDTH * MOCK_HEIGHT, sizeof(uint16_t));
    if (!s_display.pixels)
        return false;
    s_display.initialized = true;
    s_display.powered = true;
    s_display.brightness = 255;
    return true;
}

void display_hal_deinit(void) {
    free(s_display.pixels);
    memset(&s_display, 0, sizeof(s_display));
}

bool display_hal_draw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t *data) {
    if (!s_display.initialized)
        return false;
    if (x + w > MOCK_WIDTH || y + h > MOCK_HEIGHT)
        return false;

    const uint16_t *src = (const uint16_t *)data;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            s_display.pixels[(y + row) * MOCK_WIDTH + (x + col)] =
                src[row * w + col];
        }
    }
    return true;
}

bool display_hal_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color) {
    if (!s_display.initialized)
        return false;
    if (x + w > MOCK_WIDTH || y + h > MOCK_HEIGHT)
        return false;

    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            s_display.pixels[(y + row) * MOCK_WIDTH + (x + col)] = color;
        }
    }
    return true;
}

bool display_hal_clear(void) {
    if (!s_display.initialized)
        return false;
    memset(s_display.pixels, 0,
           (size_t)MOCK_WIDTH * MOCK_HEIGHT * sizeof(uint16_t));
    return true;
}

bool display_hal_set_brightness(uint8_t level) {
    if (!s_display.initialized)
        return false;
    s_display.brightness = level;
    return true;
}

bool display_hal_power(bool on) {
    if (!s_display.initialized)
        return false;
    s_display.powered = on;
    return true;
}

uint16_t display_hal_width(void) {
    return MOCK_WIDTH;
}

uint16_t display_hal_height(void) {
    return MOCK_HEIGHT;
}

/* ── Test helpers ─────────────────────────────────────────────────── */

uint16_t display_mock_get_pixel(uint16_t x, uint16_t y) {
    if (!s_display.initialized || x >= MOCK_WIDTH || y >= MOCK_HEIGHT)
        return 0;
    return s_display.pixels[y * MOCK_WIDTH + x];
}

bool display_mock_is_initialized(void) {
    return s_display.initialized;
}

uint8_t display_mock_get_brightness(void) {
    return s_display.brightness;
}

bool display_mock_is_powered(void) {
    return s_display.powered;
}
