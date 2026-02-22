#include "led_hal.h"
#include <string.h>

/* ── Mock LED strip state ────────────────────────────────────────── */

typedef struct {
    bool     configured;
    int      pin;
    int      num_leds;
    int      show_count;
    uint8_t  pixels[LED_MAX_LEDS][3]; /* R, G, B */
} mock_strip_t;

static mock_strip_t s_strip;
static bool s_initialized;

/* ── HAL interface ────────────────────────────────────────────────── */

bool led_hal_init(void) {
    memset(&s_strip, 0, sizeof(s_strip));
    s_initialized = true;
    return true;
}

void led_hal_deinit(void) {
    s_initialized = false;
}

bool led_hal_configure(int pin, int num_leds) {
    if (num_leds <= 0 || num_leds > LED_MAX_LEDS)
        return false;
    s_strip.configured = true;
    s_strip.pin = pin;
    s_strip.num_leds = num_leds;
    s_strip.show_count = 0;
    memset(s_strip.pixels, 0, sizeof(s_strip.pixels));
    return true;
}

void led_hal_deconfigure(void) {
    memset(&s_strip, 0, sizeof(s_strip));
}

bool led_hal_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip.configured || index < 0 || index >= s_strip.num_leds)
        return false;
    s_strip.pixels[index][0] = r;
    s_strip.pixels[index][1] = g;
    s_strip.pixels[index][2] = b;
    return true;
}

bool led_hal_show(void) {
    if (!s_strip.configured)
        return false;
    s_strip.show_count++;
    return true;
}

bool led_hal_clear(void) {
    if (!s_strip.configured)
        return false;
    memset(s_strip.pixels, 0, (size_t)s_strip.num_leds * 3);
    s_strip.show_count++;
    return true;
}

/* ── Test helpers ─────────────────────────────────────────────────── */

void led_mock_get_pixel(int index, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (index < 0 || index >= s_strip.num_leds) {
        *r = *g = *b = 0;
        return;
    }
    *r = s_strip.pixels[index][0];
    *g = s_strip.pixels[index][1];
    *b = s_strip.pixels[index][2];
}

int led_mock_get_show_count(void) {
    return s_strip.show_count;
}

bool led_mock_is_configured(void) {
    return s_strip.configured;
}

int led_mock_get_num_leds(void) {
    return s_strip.num_leds;
}
