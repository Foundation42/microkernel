/*
 * display_lcd43.c — Display HAL for Waveshare ESP32-S3-Touch-LCD-4.3B
 *
 * 800×480 ST7262 RGB parallel interface with CH422G I2C I/O expander
 * for backlight control.
 *
 * The RGB peripheral maintains a persistent framebuffer in PSRAM that
 * it continuously scans out to the panel.  esp_lcd_panel_draw_bitmap()
 * writes directly to this buffer — no DMA completion semaphore needed,
 * no byte-swap, no even-coordinate constraint.
 */

#include "display_hal.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <driver/i2c.h>

static const char *TAG = "display_lcd43";

/* ── Display dimensions ───────────────────────────────────────────── */

#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

/* ── I2C / CH422G configuration ───────────────────────────────────── */

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_PIN     8
#define I2C_SCL_PIN     9
#define I2C_FREQ_HZ     400000

/* CH422G I2C device addresses (7-bit).
 * i2c_master_write_to_device() shifts these left and adds R/W bit. */
#define CH422G_ADDR_MODE  0x24   /* Set I/O mode register */
#define CH422G_ADDR_OUT   0x38   /* Set EXIO output levels */

/* CH422G EXIO output register values (from Waveshare reference):
 *   Backlight ON  = 0x1E (bits 1-4 set)
 *   Backlight OFF = 0x1A (bit 2 cleared)
 *   → backlight is bit 2 (0x04) */
#define CH422G_BL_ON   0x1E
#define CH422G_BL_OFF  0x1A

/* ── RGB panel pin assignments (from Waveshare wiki) ──────────────── */

#define PIN_PCLK    7
#define PIN_DE      5
#define PIN_HSYNC   46
#define PIN_VSYNC   3

/* 16-bit RGB565 data pins — D[0:15] mapping from Waveshare reference.
 * D0–D4 = B[3:7], D5–D10 = G[2:7], D11–D15 = R[3:7] */
#define PIN_D0   14   /* B3 */
#define PIN_D1   38   /* B4 */
#define PIN_D2   18   /* B5 */
#define PIN_D3   17   /* B6 */
#define PIN_D4   10   /* B7 */
#define PIN_D5   39   /* G2 */
#define PIN_D6    0   /* G3 */
#define PIN_D7   45   /* G4 */
#define PIN_D8   48   /* G5 */
#define PIN_D9   47   /* G6 */
#define PIN_D10  21   /* G7 */
#define PIN_D11   1   /* R3 */
#define PIN_D12   2   /* R4 */
#define PIN_D13  42   /* R5 */
#define PIN_D14  41   /* R6 */
#define PIN_D15  40   /* R7 */

/* ── State ────────────────────────────────────────────────────────── */

static esp_lcd_panel_handle_t s_panel;
static bool s_initialized;
static bool s_i2c_initialized;
static bool s_backlight_on;

/* GDMA stall detection: the LCD peripheral's DMA can hang after
 * accumulated bounce buffer underflows (PSRAM contention from WiFi/TLS).
 * We track the last VSYNC timestamp and restart the DMA if it stalls. */
static volatile int64_t s_last_vsync_us;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                                const esp_lcd_rgb_panel_event_data_t *edata,
                                void *user_ctx) {
    (void)panel; (void)edata; (void)user_ctx;
    s_last_vsync_us = esp_timer_get_time();
    return false;
}

/* ── CH422G helpers ───────────────────────────────────────────────── */

/* Write one byte to a CH422G register.
 * Uses i2c_master_write_to_device() which treats addr as 7-bit and
 * handles the shift + R/W bit internally — matching Waveshare reference. */
static esp_err_t ch422g_write(uint8_t addr, uint8_t val) {
    return i2c_master_write_to_device(I2C_PORT, addr, &val, 1,
                                       pdMS_TO_TICKS(100));
}

static bool ch422g_init(void) {
    /* Initialize I2C master */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &i2c_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    s_i2c_initialized = true;

    /* Enable output mode (from Waveshare reference) */
    err = ch422g_write(CH422G_ADDR_MODE, 0x01);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G mode set failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Turn on backlight */
    err = ch422g_write(CH422G_ADDR_OUT, CH422G_BL_ON);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G backlight on failed: %s", esp_err_to_name(err));
        return false;
    }
    s_backlight_on = true;

    ESP_LOGI(TAG, "CH422G initialized (backlight on)");
    return true;
}

/* ── HAL interface ────────────────────────────────────────────────── */

bool display_hal_init(void) {
    if (s_initialized)
        return true;

    /* Step 1: I2C + CH422G (backlight) */
    if (!ch422g_init())
        return false;

    /* Step 2: RGB panel */
    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            /* ~29 Hz refresh — reduced from 16 MHz (39 Hz) to leave PSRAM
             * bandwidth headroom for WiFi/TLS bursts (WSS keepalive etc). */
            .pclk_hz = 12 * 1000 * 1000,
            .h_res = DISPLAY_WIDTH,
            .v_res = DISPLAY_HEIGHT,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = DISPLAY_WIDTH * 10,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            PIN_D0,  PIN_D1,  PIN_D2,  PIN_D3,  PIN_D4,
            PIN_D5,  PIN_D6,  PIN_D7,  PIN_D8,  PIN_D9,
            PIN_D10, PIN_D11, PIN_D12, PIN_D13, PIN_D14, PIN_D15,
        },
        .flags.fb_in_psram = true,
    };

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel reset: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        return false;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        return false;
    }

    /* Register VSYNC callback for GDMA stall detection */
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync,
    };
    esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
    s_last_vsync_us = esp_timer_get_time();

    s_initialized = true;
    ESP_LOGI(TAG, "ST7262 RGB LCD initialized (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return true;
}

void display_hal_deinit(void) {
    if (!s_initialized)
        return;

    esp_lcd_panel_del(s_panel);
    s_panel = NULL;

    /* Turn off backlight */
    ch422g_write(CH422G_ADDR_OUT, CH422G_BL_OFF);
    s_backlight_on = false;

    if (s_i2c_initialized) {
        i2c_driver_delete(I2C_PORT);
        s_i2c_initialized = false;
    }

    s_initialized = false;
}

bool display_hal_draw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t *data) {
    if (!s_initialized)
        return false;
    if (x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT)
        return false;

    /* Auto-restart GDMA if it has stalled (no VSYNC for >500ms).
     * At 29 Hz each frame is ~34ms, so 500ms means ~15 missed frames. */
    int64_t now = esp_timer_get_time();
    if (now - s_last_vsync_us > 500000) {
        ESP_LOGW(TAG, "GDMA stall detected, restarting LCD panel");
        esp_lcd_rgb_panel_restart(s_panel);
        s_last_vsync_us = now;
    }

    /* RGB panel reads from PSRAM framebuffer continuously.
     * draw_bitmap copies pixel data into the framebuffer — no DMA
     * semaphore needed, no byte-swap (RGB peripheral handles it). */
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel,
                                               x, y, x + w, y + h, data);
    return err == ESP_OK;
}

bool display_hal_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color) {
    if (!s_initialized)
        return false;
    if (x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT)
        return false;

    /* Fill row-at-a-time using a stack buffer (800 * 2 = 1600 bytes) */
    uint16_t row_buf[DISPLAY_WIDTH];
    uint16_t fill_w = (w <= DISPLAY_WIDTH) ? w : DISPLAY_WIDTH;

    for (uint16_t i = 0; i < fill_w; i++)
        row_buf[i] = color;

    for (uint16_t cy = y; cy < y + h; cy++) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel,
                                                   x, cy, x + w, cy + 1,
                                                   row_buf);
        if (err != ESP_OK)
            return false;
    }
    return true;
}

bool display_hal_clear(void) {
    return display_hal_fill(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0x0000);
}

bool display_hal_set_brightness(uint8_t level) {
    if (!s_initialized)
        return false;

    /* CH422G backlight is on/off only — no analog dimming.
     * level > 0 → on, level == 0 → off */
    uint8_t val = (level > 0) ? CH422G_BL_ON : CH422G_BL_OFF;
    esp_err_t err = ch422g_write(CH422G_ADDR_OUT, val);
    if (err == ESP_OK)
        s_backlight_on = (level > 0);
    return err == ESP_OK;
}

bool display_hal_power(bool on) {
    if (!s_initialized)
        return false;
    /* For RGB panel, power = backlight */
    return display_hal_set_brightness(on ? 255 : 0);
}

uint16_t display_hal_width(void) {
    return DISPLAY_WIDTH;
}

uint16_t display_hal_height(void) {
    return DISPLAY_HEIGHT;
}
