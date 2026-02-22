#ifndef MICROKERNEL_I2C_H
#define MICROKERNEL_I2C_H

#include "types.h"
#include <stdint.h>

/* I2C message types (defined in services.h):
 *   MSG_I2C_CONFIGURE     0xFF000030  → i2c  i2c_config_payload_t
 *   MSG_I2C_WRITE         0xFF000031  → i2c  i2c_write_payload_t
 *   MSG_I2C_READ          0xFF000032  → i2c  i2c_read_payload_t
 *   MSG_I2C_WRITE_READ    0xFF000033  → i2c  i2c_write_read_payload_t
 *   MSG_I2C_SCAN          0xFF000034  → i2c  i2c_scan_payload_t
 *   MSG_I2C_OK            0xFF000038  ← i2c  (empty)
 *   MSG_I2C_DATA          0xFF000039  ← i2c  i2c_data_payload_t
 *   MSG_I2C_ERROR         0xFF00003A  ← i2c  error string
 *   MSG_I2C_SCAN_RESULT   0xFF00003B  ← i2c  i2c_scan_result_payload_t
 */

/* ── Payload structs ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  port;      /* 0 or 1 */
    uint8_t  sda_pin;
    uint8_t  scl_pin;
    uint8_t  _pad;
    uint32_t freq_hz;   /* 100000 / 400000 / 1000000 */
} i2c_config_payload_t;

typedef struct {
    uint8_t  port;
    uint8_t  addr;      /* 7-bit */
    uint16_t data_len;
    uint8_t  data[];
} i2c_write_payload_t;

typedef struct {
    uint8_t  port;
    uint8_t  addr;
    uint16_t len;       /* bytes to read */
} i2c_read_payload_t;

typedef struct {
    uint8_t  port;
    uint8_t  addr;
    uint16_t read_len;
    uint16_t write_len;
    uint8_t  write_data[];
} i2c_write_read_payload_t;

typedef struct {
    uint8_t port;
} i2c_scan_payload_t;

typedef struct {
    uint8_t  addr;
    uint8_t  _pad;
    uint16_t data_len;
    uint8_t  data[];
} i2c_data_payload_t;

typedef struct {
    uint8_t count;
    uint8_t addrs[];
} i2c_scan_result_payload_t;

/*
 * Spawn the I2C actor.  Registers as "/node/hardware/i2c".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t i2c_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_I2C_H */
