#include "i2c_hal.h"
#include <esp_attr.h>
#include <driver/i2c.h>

#define I2C_TIMEOUT_MS 100

/* ── HAL interface ────────────────────────────────────────────────── */

bool i2c_hal_init(void) {
    return true;
}

void i2c_hal_deinit(void) {
    /* Nothing global to clean up */
}

bool i2c_hal_configure(int port, int sda, int scl, uint32_t freq) {
    if (port < 0 || port >= I2C_NUM_MAX) return false;

    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = sda,
        .scl_io_num       = scl,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq,
    };

    if (i2c_param_config((i2c_port_t)port, &conf) != ESP_OK)
        return false;

    if (i2c_driver_install((i2c_port_t)port, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK)
        return false;

    return true;
}

void i2c_hal_deconfigure(int port) {
    if (port >= 0 && port < I2C_NUM_MAX)
        i2c_driver_delete((i2c_port_t)port);
}

int i2c_hal_write(int port, uint8_t addr, const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    if (len > 0)
        i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) return 0;
    if (ret == ESP_FAIL) return -1; /* NACK */
    return -2; /* bus error / timeout */
}

int i2c_hal_read(int port, uint8_t addr, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (len > 1)
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) return 0;
    if (ret == ESP_FAIL) return -1;
    return -2;
}

int i2c_hal_write_read(int port, uint8_t addr,
                       const uint8_t *wdata, size_t wlen,
                       uint8_t *rdata, size_t rlen) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    /* Write phase */
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    if (wlen > 0)
        i2c_master_write(cmd, wdata, wlen, true);

    /* Repeated start + read phase */
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (rlen > 1)
        i2c_master_read(cmd, rdata, rlen - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, rdata + rlen - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) return 0;
    if (ret == ESP_FAIL) return -1;
    return -2;
}

bool i2c_hal_probe(int port, uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    return ret == ESP_OK;
}
