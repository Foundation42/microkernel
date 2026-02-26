/*
 * midi_esp32.c — MIDI HAL for SC16IS752 dual UART-to-I2C bridge on ESP32
 *
 * Channel A = MIDI IN (RX, interrupt-driven)
 * Channel B = MIDI OUT (TX)
 * 31250 baud, 8N1, 12 MHz crystal → divisor 24
 *
 * IRQ: active-low open-drain from SC16IS752 → GPIO ISR → eventfd notification
 * I2C: shares bus with CH422G / touch (I2C_NUM_0, SDA=8, SCL=9)
 */

#include "midi_hal.h"
#include <esp_log.h>
#include <esp_attr.h>
#include <esp_vfs_eventfd.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static const char *TAG = "midi_hal";

/* ── SC16IS752 register definitions ──────────────────────────────── */

/* Register address byte: (reg << 3) | (channel << 1) */
#define SC16_REG(reg, ch)  (uint8_t)(((reg) << 3) | ((ch) << 1))

#define SC16_CH_A  0   /* MIDI IN  */
#define SC16_CH_B  1   /* MIDI OUT */

/* General registers */
#define REG_RHR    0x00  /* Receive Holding Register (read)  */
#define REG_THR    0x00  /* Transmit Holding Register (write) */
#define REG_IER    0x01  /* Interrupt Enable Register */
#define REG_FCR    0x02  /* FIFO Control Register (write) */
#define REG_IIR    0x02  /* Interrupt Identification Register (read) */
#define REG_LCR    0x03  /* Line Control Register */
#define REG_MCR    0x04  /* Modem Control Register */
#define REG_LSR    0x05  /* Line Status Register */
#define REG_TXLVL  0x08  /* TX FIFO Level */
#define REG_RXLVL  0x09  /* RX FIFO Level */

/* Special registers (accessible when LCR[7] = 1) */
#define REG_DLL    0x00  /* Divisor Latch Low */
#define REG_DLH    0x01  /* Divisor Latch High */

/* LCR values */
#define LCR_8N1        0x03  /* 8 data, no parity, 1 stop */
#define LCR_DLAB       0x80  /* Divisor Latch Access Bit */

/* FCR values */
#define FCR_ENABLE     0x01  /* Enable FIFOs */
#define FCR_RESET_RX   0x02  /* Reset RX FIFO */
#define FCR_RESET_TX   0x04  /* Reset TX FIFO */

/* IER values */
#define IER_RHR        0x01  /* RX data available interrupt */

/* MIDI baud rate divisor: 12 MHz / (16 × 31250) = 24 */
#define MIDI_DIVISOR   24

/* I2C timeout */
#define I2C_TIMEOUT_MS 50

/* ── State ───────────────────────────────────────────────────────── */

static int      s_notify_fd = -1;
static int      s_i2c_port  = -1;
static uint8_t  s_i2c_addr;        /* 7-bit address */
static int      s_irq_pin  = -1;
static bool     s_configured;
static bool     s_i2c_installed;    /* did we install the driver? */

/* ── I2C helpers ─────────────────────────────────────────────────── */

static int sc16_write_reg(uint8_t reg_addr, uint8_t value) {
    uint8_t buf[2] = { reg_addr, value };
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)s_i2c_port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK) ? 0 : -1;
}

static int sc16_read_reg(uint8_t reg_addr, uint8_t *value) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, value, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)s_i2c_port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK) ? 0 : -1;
}

/* Bulk read from RHR (FIFO auto-dequeues on repeated reads) */
static int sc16_read_fifo(int channel, uint8_t *buf, int count) {
    if (count <= 0) return 0;

    uint8_t reg_addr = SC16_REG(REG_RHR, channel);

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_READ, true);
    if (count > 1)
        i2c_master_read(cmd, buf, count - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf + count - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)s_i2c_port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK) ? count : -1;
}

/* Bulk write to THR (FIFO auto-enqueues on repeated writes) */
static int sc16_write_fifo(int channel, const uint8_t *data, int count) {
    if (count <= 0) return 0;

    uint8_t reg_addr = SC16_REG(REG_THR, channel);

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write(cmd, data, count, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin((i2c_port_t)s_i2c_port, cmd,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return (ret == ESP_OK) ? 0 : -1;
}

/* Configure one UART channel for MIDI 31250 baud 8N1 */
static bool sc16_configure_channel(int channel, bool enable_rx_irq) {
    /* Enable FIFO, reset both FIFOs */
    if (sc16_write_reg(SC16_REG(REG_FCR, channel),
                       FCR_ENABLE | FCR_RESET_RX | FCR_RESET_TX) != 0)
        return false;

    /* Set divisor latch access bit */
    if (sc16_write_reg(SC16_REG(REG_LCR, channel), LCR_DLAB) != 0)
        return false;

    /* Write divisor (24 for 31250 baud @ 12 MHz) */
    if (sc16_write_reg(SC16_REG(REG_DLL, channel), MIDI_DIVISOR & 0xFF) != 0)
        return false;
    if (sc16_write_reg(SC16_REG(REG_DLH, channel), (MIDI_DIVISOR >> 8) & 0xFF) != 0)
        return false;

    /* Clear DLAB, set 8N1 */
    if (sc16_write_reg(SC16_REG(REG_LCR, channel), LCR_8N1) != 0)
        return false;

    /* Enable RX interrupt on Channel A only */
    if (sc16_write_reg(SC16_REG(REG_IER, channel),
                       enable_rx_irq ? IER_RHR : 0x00) != 0)
        return false;

    /* Enable FIFO (normal operation, no reset) */
    if (sc16_write_reg(SC16_REG(REG_FCR, channel), FCR_ENABLE) != 0)
        return false;

    return true;
}

/* ── GPIO ISR for SC16IS752 IRQ (active low) ─────────────────────── */

static void IRAM_ATTR midi_irq_handler(void *arg) {
    (void)arg;
    /* Signal eventfd to wake poll loop */
    uint64_t val = 1;
    write(s_notify_fd, &val, sizeof(val));
}

/* ── HAL interface ───────────────────────────────────────────────── */

bool midi_hal_init(void) {
    s_configured = false;
    s_i2c_installed = false;
    s_irq_pin = -1;

    s_notify_fd = eventfd(0, 0);
    if (s_notify_fd < 0) {
        ESP_LOGE(TAG, "eventfd failed");
        return false;
    }

    /* Set non-blocking */
    int flags = fcntl(s_notify_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(s_notify_fd, F_SETFL, flags | O_NONBLOCK);

    return true;
}

void midi_hal_deinit(void) {
    if (s_notify_fd >= 0) {
        close(s_notify_fd);
        s_notify_fd = -1;
    }
    s_configured = false;
}

bool midi_hal_configure(int i2c_port, uint8_t i2c_addr,
                        int sda, int scl, int irq,
                        uint32_t i2c_freq) {
    s_i2c_port = i2c_port;
    s_i2c_addr = i2c_addr;

    /* Try to install I2C driver — likely already installed by display HAL.
     * ESP-IDF v5.5 returns ESP_FAIL (not ESP_ERR_INVALID_STATE) when
     * the driver is already installed, so we attempt install and treat
     * any non-OK result as "bus already set up". */
    esp_err_t ret = i2c_driver_install((i2c_port_t)i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_OK) {
        /* We installed it — configure pins */
        s_i2c_installed = true;
        i2c_config_t conf = {
            .mode             = I2C_MODE_MASTER,
            .sda_io_num       = sda,
            .scl_io_num       = scl,
            .sda_pullup_en    = GPIO_PULLUP_ENABLE,
            .scl_pullup_en    = GPIO_PULLUP_ENABLE,
            .master.clk_speed = i2c_freq,
        };
        i2c_param_config((i2c_port_t)i2c_port, &conf);
    } else {
        /* Already installed by display/i2c HAL — just share the bus */
        ESP_LOGI(TAG, "I2C%d already installed, sharing bus", i2c_port);
        s_i2c_installed = false;
    }

    /* Probe the SC16IS752 */
    i2c_cmd_handle_t probe = i2c_cmd_link_create();
    i2c_master_start(probe);
    i2c_master_write_byte(probe, (i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(probe);
    ret = i2c_master_cmd_begin((i2c_port_t)i2c_port, probe,
                                pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(probe);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SC16IS752 not found at 0x%02X", i2c_addr);
        if (s_i2c_installed) {
            i2c_driver_delete((i2c_port_t)i2c_port);
            s_i2c_installed = false;
        }
        return false;
    }

    ESP_LOGI(TAG, "SC16IS752 found at 0x%02X", i2c_addr);

    /* Configure Channel A (MIDI IN) with RX interrupt */
    if (!sc16_configure_channel(SC16_CH_A, true)) {
        ESP_LOGE(TAG, "failed to configure Channel A (MIDI IN)");
        return false;
    }

    /* Configure Channel B (MIDI OUT) without interrupt */
    if (!sc16_configure_channel(SC16_CH_B, false)) {
        ESP_LOGE(TAG, "failed to configure Channel B (MIDI OUT)");
        return false;
    }

    /* Set up GPIO ISR on IRQ pin (active low, falling edge) */
    s_irq_pin = irq;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << irq),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* IRQ is open-drain */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);

    /* Install ISR service if not already installed by GPIO actor */
    gpio_install_isr_service(0);  /* ignore error if already installed */

    gpio_isr_handler_add((gpio_num_t)irq, midi_irq_handler, NULL);
    gpio_intr_enable((gpio_num_t)irq);

    ESP_LOGI(TAG, "configured: I2C%d addr=0x%02X irq=GPIO%d", i2c_port, i2c_addr, irq);
    s_configured = true;
    return true;
}

void midi_hal_deconfigure(void) {
    if (s_irq_pin >= 0) {
        gpio_isr_handler_remove((gpio_num_t)s_irq_pin);
        gpio_intr_disable((gpio_num_t)s_irq_pin);
        s_irq_pin = -1;
    }

    if (s_i2c_installed) {
        i2c_driver_delete((i2c_port_t)s_i2c_port);
        s_i2c_installed = false;
    }

    s_configured = false;
}

int midi_hal_get_notify_fd(void) {
    return s_notify_fd;
}

int midi_hal_drain_rx(uint8_t *buf, int max) {
    if (!s_configured) return 0;

    /* Clear the eventfd counter */
    uint64_t val;
    read(s_notify_fd, &val, sizeof(val));

    /* Read RXLVL to know how many bytes are available */
    uint8_t rxlvl = 0;
    if (sc16_read_reg(SC16_REG(REG_RXLVL, SC16_CH_A), &rxlvl) != 0)
        return -1;

    if (rxlvl == 0)
        return 0;

    int count = (rxlvl < max) ? rxlvl : max;

    /* Bulk read from RX FIFO */
    if (sc16_read_fifo(SC16_CH_A, buf, count) < 0)
        return -1;

    return count;
}

int midi_hal_tx(const uint8_t *data, size_t len) {
    if (!s_configured) return -1;

    /* Check TX FIFO space (64-byte FIFO) */
    uint8_t txlvl = 0;
    if (sc16_read_reg(SC16_REG(REG_TXLVL, SC16_CH_B), &txlvl) != 0)
        return -1;

    if (txlvl < len) {
        ESP_LOGW(TAG, "TX FIFO full (need %d, have %d)", (int)len, txlvl);
        return -1;
    }

    return sc16_write_fifo(SC16_CH_B, data, (int)len);
}

bool midi_hal_read_status(midi_hal_status_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s_configured) return false;

    sc16_read_reg(SC16_REG(REG_RXLVL, SC16_CH_A), &out->rxlvl);
    sc16_read_reg(SC16_REG(REG_TXLVL, SC16_CH_B), &out->txlvl);
    sc16_read_reg(SC16_REG(REG_LSR,   SC16_CH_A), &out->lsr);
    sc16_read_reg(SC16_REG(REG_IIR,   SC16_CH_A), &out->iir);
    sc16_read_reg(SC16_REG(REG_IER,   SC16_CH_A), &out->ier);
    return true;
}
