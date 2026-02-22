#ifndef GPIO_HAL_H
#define GPIO_HAL_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize GPIO HAL and create ISR notification channel.
 * Returns true on success. */
bool gpio_hal_init(void);

/* Shutdown GPIO HAL, close notification fds. */
void gpio_hal_deinit(void);

/* Returns the fd to watch (POLLIN) for ISR notifications. */
int gpio_hal_get_notify_fd(void);

/* Drain pending pin-event bytes from the notification channel.
 * Each byte is a pin number that fired an interrupt.
 * Returns number of pins read, or <= 0 on error/empty. */
int gpio_hal_drain_events(uint8_t *pins, int max);

/* Configure a pin.  mode: 0=input, 1=output, 2=input_pullup, 3=input_pulldown */
bool gpio_hal_configure(int pin, int mode);

/* Write digital value (0 or 1) to an output pin. */
bool gpio_hal_write(int pin, int value);

/* Read current digital value from a pin. Returns 0/1, or -1 on error. */
int gpio_hal_read(int pin);

/* Install ISR handler for a pin.  edge: 0=rising, 1=falling, 2=both */
bool gpio_hal_isr_add(int pin, int edge);

/* Remove ISR handler for a pin. */
bool gpio_hal_isr_remove(int pin);

/* ── Test helpers (Linux mock only) ───────────────────────────────── */

#ifndef ESP_PLATFORM
/* Simulate an interrupt on a pin with the given level.
 * Sets the mock value and writes pin number to notification pipe. */
void gpio_mock_trigger_interrupt(int pin, int value);

/* Set the mock value readable by gpio_hal_read (no interrupt). */
void gpio_mock_set_value(int pin, int value);
#endif

#endif /* GPIO_HAL_H */
