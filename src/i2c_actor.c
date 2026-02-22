#include "microkernel/i2c.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "i2c_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    bool     configured[2];
    int      sda_pin[2];
    int      scl_pin[2];
    uint32_t freq[2];
} i2c_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void reply_error(runtime_t *rt, actor_id_t dest, const char *err) {
    actor_send(rt, dest, MSG_I2C_ERROR, err, strlen(err));
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void handle_configure(i2c_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(i2c_config_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const i2c_config_payload_t *cfg = (const i2c_config_payload_t *)msg->payload;
    int port = cfg->port;

    if (port > 1) {
        reply_error(rt, msg->source, "invalid port");
        return;
    }

    /* Deconfigure if already configured */
    if (s->configured[port])
        i2c_hal_deconfigure(port);

    if (!i2c_hal_configure(port, cfg->sda_pin, cfg->scl_pin, cfg->freq_hz)) {
        reply_error(rt, msg->source, "hal configure failed");
        return;
    }

    s->configured[port] = true;
    s->sda_pin[port] = cfg->sda_pin;
    s->scl_pin[port] = cfg->scl_pin;
    s->freq[port] = cfg->freq_hz;

    actor_send(rt, msg->source, MSG_I2C_OK, NULL, 0);
}

static void handle_write(i2c_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(i2c_write_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const i2c_write_payload_t *req = (const i2c_write_payload_t *)msg->payload;
    int port = req->port;

    if (port > 1 || !s->configured[port]) {
        reply_error(rt, msg->source, port > 1 ? "invalid port" : "port not configured");
        return;
    }

    if (msg->payload_size < sizeof(i2c_write_payload_t) + req->data_len) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    int rc = i2c_hal_write(port, req->addr, req->data, req->data_len);
    if (rc != 0) {
        reply_error(rt, msg->source, rc == -1 ? "NACK" : "bus error");
        return;
    }

    actor_send(rt, msg->source, MSG_I2C_OK, NULL, 0);
}

static void handle_read(i2c_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(i2c_read_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const i2c_read_payload_t *req = (const i2c_read_payload_t *)msg->payload;
    int port = req->port;

    if (port > 1 || !s->configured[port]) {
        reply_error(rt, msg->source, port > 1 ? "invalid port" : "port not configured");
        return;
    }

    uint16_t len = req->len;
    if (len == 0 || len > 256) {
        reply_error(rt, msg->source, "invalid length");
        return;
    }

    /* Allocate response: header + data */
    size_t resp_size = sizeof(i2c_data_payload_t) + len;
    uint8_t *buf = malloc(resp_size);
    if (!buf) {
        reply_error(rt, msg->source, "out of memory");
        return;
    }

    i2c_data_payload_t *resp = (i2c_data_payload_t *)buf;
    resp->addr = req->addr;
    resp->_pad = 0;
    resp->data_len = len;

    int rc = i2c_hal_read(port, req->addr, resp->data, len);
    if (rc != 0) {
        free(buf);
        reply_error(rt, msg->source, rc == -1 ? "NACK" : "bus error");
        return;
    }

    actor_send(rt, msg->source, MSG_I2C_DATA, buf, resp_size);
    free(buf);
}

static void handle_write_read(i2c_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(i2c_write_read_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const i2c_write_read_payload_t *req =
        (const i2c_write_read_payload_t *)msg->payload;
    int port = req->port;

    if (port > 1 || !s->configured[port]) {
        reply_error(rt, msg->source, port > 1 ? "invalid port" : "port not configured");
        return;
    }

    if (msg->payload_size < sizeof(i2c_write_read_payload_t) + req->write_len) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    uint16_t rlen = req->read_len;
    if (rlen == 0 || rlen > 256) {
        reply_error(rt, msg->source, "invalid length");
        return;
    }

    size_t resp_size = sizeof(i2c_data_payload_t) + rlen;
    uint8_t *buf = malloc(resp_size);
    if (!buf) {
        reply_error(rt, msg->source, "out of memory");
        return;
    }

    i2c_data_payload_t *resp = (i2c_data_payload_t *)buf;
    resp->addr = req->addr;
    resp->_pad = 0;
    resp->data_len = rlen;

    int rc;
    if (req->write_len == 0) {
        /* Zero write_len → plain read */
        rc = i2c_hal_read(port, req->addr, resp->data, rlen);
    } else {
        rc = i2c_hal_write_read(port, req->addr,
                                req->write_data, req->write_len,
                                resp->data, rlen);
    }

    if (rc != 0) {
        free(buf);
        reply_error(rt, msg->source, rc == -1 ? "NACK" : "bus error");
        return;
    }

    actor_send(rt, msg->source, MSG_I2C_DATA, buf, resp_size);
    free(buf);
}

static void handle_scan(i2c_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(i2c_scan_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const i2c_scan_payload_t *req = (const i2c_scan_payload_t *)msg->payload;
    int port = req->port;

    if (port > 1 || !s->configured[port]) {
        reply_error(rt, msg->source, port > 1 ? "invalid port" : "port not configured");
        return;
    }

    /* Scan 7-bit address range 0x08–0x77 */
    uint8_t found[112]; /* max possible */
    uint8_t count = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_hal_probe(port, addr))
            found[count++] = addr;
    }

    /* Build response */
    size_t resp_size = sizeof(i2c_scan_result_payload_t) + count;
    uint8_t *buf = malloc(resp_size);
    if (!buf) {
        reply_error(rt, msg->source, "out of memory");
        return;
    }

    i2c_scan_result_payload_t *resp = (i2c_scan_result_payload_t *)buf;
    resp->count = count;
    memcpy(resp->addrs, found, count);

    actor_send(rt, msg->source, MSG_I2C_SCAN_RESULT, buf, resp_size);
    free(buf);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool i2c_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    i2c_state_t *s = state;

    switch (msg->type) {
    case MSG_I2C_CONFIGURE:  handle_configure(s, rt, msg);  break;
    case MSG_I2C_WRITE:      handle_write(s, rt, msg);      break;
    case MSG_I2C_READ:       handle_read(s, rt, msg);       break;
    case MSG_I2C_WRITE_READ: handle_write_read(s, rt, msg); break;
    case MSG_I2C_SCAN:       handle_scan(s, rt, msg);       break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void i2c_state_free(void *state) {
    i2c_state_t *s = state;
    for (int i = 0; i < 2; i++) {
        if (s->configured[i])
            i2c_hal_deconfigure(i);
    }
    i2c_hal_deinit();
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t i2c_actor_init(runtime_t *rt) {
    if (!i2c_hal_init())
        return ACTOR_ID_INVALID;

    i2c_state_t *s = calloc(1, sizeof(*s));
    if (!s) {
        i2c_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    actor_id_t id = actor_spawn(rt, i2c_behavior, s, i2c_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        i2c_hal_deinit();
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/i2c", id);

    return id;
}
