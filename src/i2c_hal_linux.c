#include "i2c_hal.h"
#include <string.h>

/* ── Mock device state ────────────────────────────────────────────── */

#define MAX_MOCK_DEVICES 16
#define MAX_REG_SIZE     32

typedef struct {
    uint8_t addr;
    bool    present;
    uint8_t reg_ptr;                    /* last written register address */
    uint8_t regs[256][MAX_REG_SIZE];
    uint8_t reg_len[256];
} mock_device_t;

static mock_device_t s_devices[MAX_MOCK_DEVICES];
static bool s_initialized;

/* ── HAL interface ────────────────────────────────────────────────── */

bool i2c_hal_init(void) {
    memset(s_devices, 0, sizeof(s_devices));
    s_initialized = true;
    return true;
}

void i2c_hal_deinit(void) {
    s_initialized = false;
}

bool i2c_hal_configure(int port, int sda, int scl, uint32_t freq) {
    (void)sda; (void)scl; (void)freq;
    if (port < 0 || port > 1) return false;
    return true;
}

void i2c_hal_deconfigure(int port) {
    (void)port;
}

static mock_device_t *find_device(uint8_t addr) {
    for (int i = 0; i < MAX_MOCK_DEVICES; i++) {
        if (s_devices[i].present && s_devices[i].addr == addr)
            return &s_devices[i];
    }
    return NULL;
}

int i2c_hal_write(int port, uint8_t addr, const uint8_t *data, size_t len) {
    (void)port;
    mock_device_t *dev = find_device(addr);
    if (!dev) return -1; /* NACK */
    if (len == 0) return 0;

    /* First byte = register address */
    dev->reg_ptr = data[0];

    /* Remaining bytes = data to store */
    if (len > 1) {
        size_t dlen = len - 1;
        if (dlen > MAX_REG_SIZE) dlen = MAX_REG_SIZE;
        memcpy(dev->regs[dev->reg_ptr], data + 1, dlen);
        dev->reg_len[dev->reg_ptr] = (uint8_t)dlen;
    }

    return 0;
}

int i2c_hal_read(int port, uint8_t addr, uint8_t *buf, size_t len) {
    (void)port;
    mock_device_t *dev = find_device(addr);
    if (!dev) return -1; /* NACK */

    size_t avail = dev->reg_len[dev->reg_ptr];
    size_t copy = len < avail ? len : avail;
    if (copy > 0)
        memcpy(buf, dev->regs[dev->reg_ptr], copy);
    /* Zero-fill remainder */
    if (copy < len)
        memset(buf + copy, 0, len - copy);

    return 0;
}

int i2c_hal_write_read(int port, uint8_t addr,
                       const uint8_t *wdata, size_t wlen,
                       uint8_t *rdata, size_t rlen) {
    (void)port;
    mock_device_t *dev = find_device(addr);
    if (!dev) return -1;

    /* Write phase: set register pointer */
    if (wlen > 0)
        dev->reg_ptr = wdata[0];

    /* Read phase */
    size_t avail = dev->reg_len[dev->reg_ptr];
    size_t copy = rlen < avail ? rlen : avail;
    if (copy > 0)
        memcpy(rdata, dev->regs[dev->reg_ptr], copy);
    if (copy < rlen)
        memset(rdata + copy, 0, rlen - copy);

    return 0;
}

bool i2c_hal_probe(int port, uint8_t addr) {
    (void)port;
    return find_device(addr) != NULL;
}

/* ── Test helpers ─────────────────────────────────────────────────── */

void i2c_mock_add_device(uint8_t addr) {
    /* Check if already present */
    for (int i = 0; i < MAX_MOCK_DEVICES; i++) {
        if (s_devices[i].present && s_devices[i].addr == addr)
            return;
    }
    /* Find free slot */
    for (int i = 0; i < MAX_MOCK_DEVICES; i++) {
        if (!s_devices[i].present) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
            s_devices[i].addr = addr;
            s_devices[i].present = true;
            return;
        }
    }
}

void i2c_mock_remove_device(uint8_t addr) {
    for (int i = 0; i < MAX_MOCK_DEVICES; i++) {
        if (s_devices[i].present && s_devices[i].addr == addr) {
            s_devices[i].present = false;
            return;
        }
    }
}

void i2c_mock_set_register(uint8_t addr, uint8_t reg,
                           const uint8_t *data, size_t len) {
    mock_device_t *dev = find_device(addr);
    if (!dev) return;
    if (len > MAX_REG_SIZE) len = MAX_REG_SIZE;
    memcpy(dev->regs[reg], data, len);
    dev->reg_len[reg] = (uint8_t)len;
}
