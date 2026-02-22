#include "pwm_hal.h"
#include <string.h>

/* ── Mock channel state ──────────────────────────────────────────── */

typedef struct {
    bool     configured;
    int      pin;
    uint32_t freq_hz;
    int      resolution;
    uint32_t duty;
} mock_channel_t;

static mock_channel_t s_channels[PWM_MAX_CHANNELS];
static bool s_initialized;

/* ── HAL interface ────────────────────────────────────────────────── */

bool pwm_hal_init(void) {
    memset(s_channels, 0, sizeof(s_channels));
    s_initialized = true;
    return true;
}

void pwm_hal_deinit(void) {
    s_initialized = false;
}

bool pwm_hal_configure(int channel, int pin, uint32_t freq_hz, int resolution) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return false;
    s_channels[channel].configured = true;
    s_channels[channel].pin = pin;
    s_channels[channel].freq_hz = freq_hz;
    s_channels[channel].resolution = resolution;
    s_channels[channel].duty = 0;
    return true;
}

void pwm_hal_deconfigure(int channel) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return;
    memset(&s_channels[channel], 0, sizeof(s_channels[channel]));
}

bool pwm_hal_set_duty(int channel, uint32_t duty) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return false;
    if (!s_channels[channel].configured)
        return false;
    s_channels[channel].duty = duty;
    return true;
}

/* ── Test helpers ─────────────────────────────────────────────────── */

uint32_t pwm_mock_get_duty(int channel) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return 0;
    return s_channels[channel].duty;
}

bool pwm_mock_is_configured(int channel) {
    if (channel < 0 || channel >= PWM_MAX_CHANNELS)
        return false;
    return s_channels[channel].configured;
}
