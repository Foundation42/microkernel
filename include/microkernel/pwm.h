#ifndef MICROKERNEL_PWM_H
#define MICROKERNEL_PWM_H

#include "types.h"
#include <stdint.h>

/* PWM message types (defined in services.h):
 *   MSG_PWM_CONFIGURE   0xFF00002C  → pwm  pwm_config_payload_t
 *   MSG_PWM_SET_DUTY    0xFF00002D  → pwm  pwm_duty_payload_t
 *   MSG_PWM_OK          0xFF00002E  ← pwm  (empty)
 *   MSG_PWM_ERROR       0xFF00002F  ← pwm  error string
 */

/* ── Payload structs ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  channel;     /* 0–5 (ESP32-C6 has 6 LEDC channels) */
    uint8_t  pin;
    uint8_t  resolution;  /* bits: 8, 10, 12, 14 */
    uint8_t  _pad;
    uint32_t freq_hz;     /* e.g. 5000 */
} pwm_config_payload_t;

typedef struct {
    uint8_t  channel;
    uint8_t  _pad[3];
    uint32_t duty;        /* 0 to (1<<resolution)-1 */
} pwm_duty_payload_t;

/*
 * Spawn the PWM actor.  Registers as "/node/hardware/pwm".
 * Returns the actor ID, or ACTOR_ID_INVALID on failure.
 */
actor_id_t pwm_actor_init(runtime_t *rt);

#endif /* MICROKERNEL_PWM_H */
