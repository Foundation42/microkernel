#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/i2c.h"
#include "i2c_hal.h"
#include <string.h>
#include <stdlib.h>

/* ── Tester state ─────────────────────────────────────────────────── */

typedef struct {
    actor_id_t i2c_id;
    int        step;
    bool       done;

    msg_type_t last_type;
    uint8_t    last_payload[256];
    size_t     last_payload_size;

    bool       got_ok;
    bool       got_data;
    bool       got_error;
    bool       got_scan;
} i2c_tester_t;

static void save_reply(i2c_tester_t *s, message_t *msg) {
    s->last_type = msg->type;
    s->last_payload_size = msg->payload_size < sizeof(s->last_payload)
                          ? msg->payload_size : sizeof(s->last_payload);
    if (s->last_payload_size > 0 && msg->payload)
        memcpy(s->last_payload, msg->payload, s->last_payload_size);

    if (msg->type == MSG_I2C_OK)          s->got_ok = true;
    if (msg->type == MSG_I2C_DATA)        s->got_data = true;
    if (msg->type == MSG_I2C_ERROR)       s->got_error = true;
    if (msg->type == MSG_I2C_SCAN_RESULT) s->got_scan = true;
}

/* ── test_init ─────────────────────────────────────────────────────── */

static int test_init(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    actor_id_t i2c_id = i2c_actor_init(rt);
    ASSERT_NE(i2c_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "/node/hardware/i2c"), i2c_id);

    runtime_destroy(rt);
    return 0;
}

/* ── test_configure ────────────────────────────────────────────────── */

static bool configure_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        i2c_config_payload_t cfg = {
            .port = 0, .sda_pin = 21, .scl_pin = 22, .freq_hz = 100000
        };
        actor_send(rt, s->i2c_id, MSG_I2C_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_configure(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, configure_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_write ────────────────────────────────────────────────────── */

static bool write_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        i2c_config_payload_t cfg = {
            .port = 0, .sda_pin = 21, .scl_pin = 22, .freq_hz = 100000
        };
        actor_send(rt, s->i2c_id, MSG_I2C_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_I2C_OK && s->step == 1) {
        s->step = 2;
        /* Write 3 bytes: reg_addr=0x10, data=0xAA,0xBB */
        uint8_t buf[sizeof(i2c_write_payload_t) + 3];
        i2c_write_payload_t *req = (i2c_write_payload_t *)buf;
        req->port = 0;
        req->addr = 0x50;
        req->data_len = 3;
        req->data[0] = 0x10; /* register */
        req->data[1] = 0xAA;
        req->data[2] = 0xBB;
        actor_send(rt, s->i2c_id, MSG_I2C_WRITE, buf, sizeof(buf));
        return true;
    }

    if (s->step == 2) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_write(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    i2c_mock_add_device(0x50);

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, write_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_ok);

    runtime_destroy(rt);
    return 0;
}

/* ── test_read ─────────────────────────────────────────────────────── */

static bool read_tester(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        i2c_config_payload_t cfg = {
            .port = 0, .sda_pin = 21, .scl_pin = 22, .freq_hz = 100000
        };
        actor_send(rt, s->i2c_id, MSG_I2C_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_I2C_OK && s->step == 1) {
        /* Write register address first, then read */
        s->step = 2;
        /* Write reg_addr=0x00 (sets reg_ptr) */
        uint8_t buf[sizeof(i2c_write_payload_t) + 1];
        i2c_write_payload_t *req = (i2c_write_payload_t *)buf;
        req->port = 0;
        req->addr = 0x50;
        req->data_len = 1;
        req->data[0] = 0x00; /* register 0 */
        actor_send(rt, s->i2c_id, MSG_I2C_WRITE, buf, sizeof(buf));
        return true;
    }

    if (msg->type == MSG_I2C_OK && s->step == 2) {
        s->step = 3;
        i2c_read_payload_t req = { .port = 0, .addr = 0x50, .len = 2 };
        actor_send(rt, s->i2c_id, MSG_I2C_READ, &req, sizeof(req));
        return true;
    }

    if (s->step == 3) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_read(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    i2c_mock_add_device(0x50);
    uint8_t data[] = { 0xDE, 0xAD };
    i2c_mock_set_register(0x50, 0x00, data, sizeof(data));

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, read_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_data);
    const i2c_data_payload_t *resp = (const i2c_data_payload_t *)ts.last_payload;
    ASSERT_EQ(resp->addr, 0x50);
    ASSERT_EQ(resp->data_len, 2);
    ASSERT_EQ(resp->data[0], 0xDE);
    ASSERT_EQ(resp->data[1], 0xAD);

    runtime_destroy(rt);
    return 0;
}

/* ── test_write_read ───────────────────────────────────────────────── */

static bool write_read_tester(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        i2c_config_payload_t cfg = {
            .port = 0, .sda_pin = 21, .scl_pin = 22, .freq_hz = 400000
        };
        actor_send(rt, s->i2c_id, MSG_I2C_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_I2C_OK && s->step == 1) {
        s->step = 2;
        /* Write-read: write reg=0x0A, read 2 bytes */
        uint8_t buf[sizeof(i2c_write_read_payload_t) + 1];
        i2c_write_read_payload_t *req = (i2c_write_read_payload_t *)buf;
        req->port = 0;
        req->addr = 0x68;
        req->read_len = 2;
        req->write_len = 1;
        req->write_data[0] = 0x0A;
        actor_send(rt, s->i2c_id, MSG_I2C_WRITE_READ, buf, sizeof(buf));
        return true;
    }

    if (s->step == 2) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_write_read(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    i2c_mock_add_device(0x68);
    uint8_t data[] = { 0xCA, 0xFE };
    i2c_mock_set_register(0x68, 0x0A, data, sizeof(data));

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, write_read_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_data);
    const i2c_data_payload_t *resp = (const i2c_data_payload_t *)ts.last_payload;
    ASSERT_EQ(resp->addr, 0x68);
    ASSERT_EQ(resp->data_len, 2);
    ASSERT_EQ(resp->data[0], 0xCA);
    ASSERT_EQ(resp->data[1], 0xFE);

    runtime_destroy(rt);
    return 0;
}

/* ── test_scan ─────────────────────────────────────────────────────── */

static bool scan_tester(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        i2c_config_payload_t cfg = {
            .port = 0, .sda_pin = 21, .scl_pin = 22, .freq_hz = 100000
        };
        actor_send(rt, s->i2c_id, MSG_I2C_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_I2C_OK && s->step == 1) {
        s->step = 2;
        i2c_scan_payload_t req = { .port = 0 };
        actor_send(rt, s->i2c_id, MSG_I2C_SCAN, &req, sizeof(req));
        return true;
    }

    if (s->step == 2) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_scan(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    i2c_mock_add_device(0x50);
    i2c_mock_add_device(0x68);

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, scan_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_scan);
    const i2c_scan_result_payload_t *resp =
        (const i2c_scan_result_payload_t *)ts.last_payload;
    ASSERT_EQ(resp->count, 2);
    ASSERT_EQ(resp->addrs[0], 0x50);
    ASSERT_EQ(resp->addrs[1], 0x68);

    runtime_destroy(rt);
    return 0;
}

/* ── test_error_no_device ──────────────────────────────────────────── */

static bool no_device_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        i2c_config_payload_t cfg = {
            .port = 0, .sda_pin = 21, .scl_pin = 22, .freq_hz = 100000
        };
        actor_send(rt, s->i2c_id, MSG_I2C_CONFIGURE, &cfg, sizeof(cfg));
        return true;
    }

    if (msg->type == MSG_I2C_OK && s->step == 1) {
        s->step = 2;
        i2c_read_payload_t req = { .port = 0, .addr = 0x77, .len = 1 };
        actor_send(rt, s->i2c_id, MSG_I2C_READ, &req, sizeof(req));
        return true;
    }

    if (s->step == 2) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_error_no_device(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    /* No mock devices added */

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, no_device_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "NACK") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── test_error_unconfigured ───────────────────────────────────────── */

static bool unconfigured_tester(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    i2c_tester_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        /* Try to read without configuring the port first */
        i2c_read_payload_t req = { .port = 0, .addr = 0x50, .len = 1 };
        actor_send(rt, s->i2c_id, MSG_I2C_READ, &req, sizeof(req));
        return true;
    }

    if (s->step == 1) {
        save_reply(s, msg);
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_error_unconfigured(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t i2c_id = i2c_actor_init(rt);

    i2c_tester_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.i2c_id = i2c_id;

    actor_id_t tester = actor_spawn(rt, unconfigured_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.got_error);
    ASSERT(strstr((const char *)ts.last_payload, "not configured") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_i2c:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_configure);
    RUN_TEST(test_write);
    RUN_TEST(test_read);
    RUN_TEST(test_write_read);
    RUN_TEST(test_scan);
    RUN_TEST(test_error_no_device);
    RUN_TEST(test_error_unconfigured);

    TEST_REPORT();
}
