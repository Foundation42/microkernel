#ifndef I2C_HAL_H
#define I2C_HAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Initialize I2C HAL. Returns true on success. */
bool i2c_hal_init(void);

/* Shutdown I2C HAL. */
void i2c_hal_deinit(void);

/* Configure an I2C port as master. */
bool i2c_hal_configure(int port, int sda, int scl, uint32_t freq);

/* Deconfigure (release) an I2C port. */
void i2c_hal_deconfigure(int port);

/* Write data to a device.  Returns 0=ok, -1=NACK, -2=bus error. */
int i2c_hal_write(int port, uint8_t addr, const uint8_t *data, size_t len);

/* Read data from a device.  Returns 0=ok, -1=NACK, -2=bus error. */
int i2c_hal_read(int port, uint8_t addr, uint8_t *buf, size_t len);

/* Write then read (register access).  Returns 0=ok, -1=NACK, -2=bus error. */
int i2c_hal_write_read(int port, uint8_t addr,
                       const uint8_t *wdata, size_t wlen,
                       uint8_t *rdata, size_t rlen);

/* Probe for a device at addr.  Returns true if ACK received. */
bool i2c_hal_probe(int port, uint8_t addr);

/* ── Test helpers (Linux mock only) ───────────────────────────────── */

#ifndef ESP_PLATFORM
/* Add a mock device at the given 7-bit address. */
void i2c_mock_add_device(uint8_t addr);

/* Remove a mock device. */
void i2c_mock_remove_device(uint8_t addr);

/* Pre-populate register data for a mock device. */
void i2c_mock_set_register(uint8_t addr, uint8_t reg,
                           const uint8_t *data, size_t len);
#endif

#endif /* I2C_HAL_H */
