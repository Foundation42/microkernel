#ifndef PWM_HAL_H
#define PWM_HAL_H

#include <stdbool.h>
#include <stdint.h>

#define PWM_MAX_CHANNELS 6

/* Initialize PWM HAL. Returns true on success. */
bool pwm_hal_init(void);

/* Shutdown PWM HAL. */
void pwm_hal_deinit(void);

/* Configure a PWM channel. Returns true on success. */
bool pwm_hal_configure(int channel, int pin, uint32_t freq_hz, int resolution);

/* Deconfigure (release) a PWM channel. */
void pwm_hal_deconfigure(int channel);

/* Set duty cycle for a channel. Returns true on success. */
bool pwm_hal_set_duty(int channel, uint32_t duty);

/* ── Test helpers (Linux mock only) ───────────────────────────────── */

#ifndef ESP_PLATFORM
/* Get the current duty for a channel (for test assertions). */
uint32_t pwm_mock_get_duty(int channel);

/* Check if a channel is configured. */
bool pwm_mock_is_configured(int channel);
#endif

#endif /* PWM_HAL_H */
