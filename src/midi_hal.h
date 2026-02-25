#ifndef MIDI_HAL_H
#define MIDI_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Initialize MIDI HAL (creates notification pipe/eventfd).
 * Returns true on success. */
bool midi_hal_init(void);

/* Shutdown MIDI HAL. */
void midi_hal_deinit(void);

/* Configure the SC16IS752 via I2C.
 * Sets up both UART channels at 31250 baud, enables RX FIFO interrupt.
 * Returns true on success. */
bool midi_hal_configure(int i2c_port, uint8_t i2c_addr,
                        int sda, int scl, int irq,
                        uint32_t i2c_freq);

/* Release I2C + IRQ resources. */
void midi_hal_deconfigure(void);

/* Returns the fd to watch (POLLIN) for RX data notifications. */
int midi_hal_get_notify_fd(void);

/* Drain received MIDI bytes from Channel A RX FIFO.
 * Returns number of bytes read, or <= 0 on error/empty. */
int midi_hal_drain_rx(uint8_t *buf, int max);

/* Transmit raw bytes to Channel B TX FIFO.
 * Returns 0 on success, -1 on error. */
int midi_hal_tx(const uint8_t *data, size_t len);

/* ── Test helpers (Linux mock only) ──────────────────────────────── */

#ifndef ESP_PLATFORM
/* Inject raw MIDI bytes into the mock RX buffer.
 * Writes to the notification pipe to trigger poll wakeup. */
void midi_mock_inject_rx(const uint8_t *data, size_t len);

/* Read back bytes sent via midi_hal_tx.
 * Returns number of bytes copied. */
int midi_mock_get_tx(uint8_t *buf, int max);

/* Clear TX capture buffer. */
void midi_mock_clear_tx(void);

/* Check if configured. */
bool midi_mock_is_configured(void);
#endif

#endif /* MIDI_HAL_H */
