#include "pwm_hal.h"
#include <driver/ledc.h>
#include <esp_log.h>

static const char *TAG = "pwm_hal";

/* 1:1 timer-to-channel mapping (simple, C6 has 4 timers + 6 channels).
 * Channels 0–3 get their own timer; channels 4–5 share timers 0–1. */
static ledc_timer_t channel_to_timer(int channel) {
    return (ledc_timer_t)(channel % LEDC_TIMER_MAX);
}

static bool s_configured[PWM_MAX_CHANNELS];

bool pwm_hal_init(void) {
    for (int i = 0; i < PWM_MAX_CHANNELS; i++)
        s_configured[i] = false;
    return true;
}

void pwm_hal_deinit(void) {
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (s_configured[i])
            pwm_hal_deconfigure(i);
    }
}

bool pwm_hal_configure(int channel, int pin, uint32_t freq_hz, int resolution) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return false;

    ledc_timer_t timer = channel_to_timer(channel);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = timer,
        .duty_resolution = (ledc_timer_bit_t)resolution,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config ch=%d: %s", channel, esp_err_to_name(err));
        return false;
    }

    ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)channel,
        .timer_sel = timer,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = pin,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config ch=%d: %s", channel, esp_err_to_name(err));
        return false;
    }

    s_configured[channel] = true;
    ESP_LOGI(TAG, "configured ch=%d pin=%d freq=%lu res=%d",
             channel, pin, (unsigned long)freq_hz, resolution);
    return true;
}

void pwm_hal_deconfigure(int channel) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return;
    if (!s_configured[channel])
        return;

    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, 0);
    s_configured[channel] = false;
}

bool pwm_hal_set_duty(int channel, uint32_t duty) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return false;
    if (!s_configured[channel])
        return false;

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                   (ledc_channel_t)channel, duty);
    if (err != ESP_OK) return false;

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
    return err == ESP_OK;
}
