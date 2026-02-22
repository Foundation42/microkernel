#include "led_hal.h"
#include <led_strip.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "led_hal";

static led_strip_handle_t s_strip;
static bool s_configured;
static int s_num_leds;

bool led_hal_init(void) {
    s_strip = NULL;
    s_configured = false;
    s_num_leds = 0;
    return true;
}

void led_hal_deinit(void) {
    if (s_configured)
        led_hal_deconfigure();
}

bool led_hal_configure(int pin, int num_leds) {
    if (num_leds <= 0 || num_leds > LED_MAX_LEDS)
        return false;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = pin,
        .max_leds = num_leds,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device: %s", esp_err_to_name(err));
        return false;
    }

    /* Start with all off */
    led_strip_clear(s_strip);

    s_configured = true;
    s_num_leds = num_leds;
    ESP_LOGI(TAG, "configured pin=%d num_leds=%d", pin, num_leds);
    return true;
}

void led_hal_deconfigure(void) {
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    s_configured = false;
    s_num_leds = 0;
}

bool led_hal_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_configured || index < 0 || index >= s_num_leds)
        return false;
    return led_strip_set_pixel(s_strip, index, r, g, b) == ESP_OK;
}

bool led_hal_show(void) {
    if (!s_configured)
        return false;
    return led_strip_refresh(s_strip) == ESP_OK;
}

bool led_hal_clear(void) {
    if (!s_configured)
        return false;
    return led_strip_clear(s_strip) == ESP_OK;
}
