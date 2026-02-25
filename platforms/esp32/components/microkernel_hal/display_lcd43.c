/*
 * display_lcd43.c — Display HAL for Waveshare ESP32-S3-Touch-LCD-4.3B
 *
 * 800×480 ST7262 RGB parallel interface with CH422G I2C I/O expander
 * for backlight and reset control.
 *
 * The RGB peripheral maintains a persistent framebuffer in PSRAM that
 * it continuously scans out to the panel.  esp_lcd_panel_draw_bitmap()
 * writes directly to this buffer — no DMA completion semaphore needed,
 * no byte-swap, no even-coordinate constraint.
 */

#include "display_hal.h"
#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <driver/i2c.h>
#include <string.h>

static const char *TAG = "display_lcd43";

/* ── Display dimensions ───────────────────────────────────────────── */

#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

/* ── I2C / CH422G configuration ───────────────────────────────────── */

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_PIN     8
#define I2C_SCL_PIN     9
#define I2C_FREQ_HZ     400000

/* CH422G register addresses (7-bit addr shifted into write byte) */
#define CH422G_MODE_REG   0x24   /* Set I/O mode */
#define CH422G_OUT_REG    0x38   /* Set output levels */

/* CH422G output bits:
 *   EXIO1 (bit 1) = unused
 *   EXIO2 (bit 2) = backlight (active high)
 *   EXIO3 (bit 3) = LCD reset (active high = release from reset)
 */
#define CH422G_BIT_BACKLIGHT  (1 << 2)
#define CH422G_BIT_LCD_RESET  (1 << 3)

/* ── RGB panel pin assignments (from Waveshare wiki) ──────────────── */

#define PIN_PCLK    7
#define PIN_DE      5
#define PIN_HSYNC   46
#define PIN_VSYNC   3

/* 16-bit RGB565 data pins: R[3:7], G[2:7], B[3:7] */
#define PIN_R3  1
#define PIN_R4  2
#define PIN_R5  42
#define PIN_R6  41
#define PIN_R7  40
#define PIN_G2  39
#define PIN_G3  0
#define PIN_G4  45
#define PIN_G5  48
#define PIN_G6  47
#define PIN_G7  21
#define PIN_B3  14
#define PIN_B4  38
#define PIN_B5  18
#define PIN_B6  17
#define PIN_B7  10

/* ── State ────────────────────────────────────────────────────────── */

static esp_lcd_panel_handle_t s_panel;
static bool s_initialized;
static bool s_i2c_initialized;
static uint8_t s_ch422g_output;   /* current CH422G output register state */

/* ── CH422G helpers ───────────────────────────────────────────────── */

static esp_err_t ch422g_write_reg(uint8_t reg, uint8_t val) {
    /* CH422G uses a special addressing: the register address IS the
     * I2C device address.  Write one data byte to it. */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, reg, true);  /* reg as address byte */
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
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

    /* Enable bidirectional output mode */
    err = ch422g_write_reg(CH422G_MODE_REG, 0x01);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G mode set failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Assert LCD reset (active low) then release:
     * First pull reset LOW (clear bit 3), then set HIGH */
    s_ch422g_output = CH422G_BIT_BACKLIGHT;  /* backlight on, reset low */
    err = ch422g_write_reg(CH422G_OUT_REG, s_ch422g_output);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G output set failed: %s", esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Release reset */
    s_ch422g_output |= CH422G_BIT_LCD_RESET;
    err = ch422g_write_reg(CH422G_OUT_REG, s_ch422g_output);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CH422G reset release failed: %s", esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "CH422G initialized (backlight on, reset released)");
    return true;
}

/* ── HAL interface ────────────────────────────────────────────────── */

bool display_hal_init(void) {
    if (s_initialized)
        return true;

    /* Step 1: I2C + CH422G (reset + backlight) */
    if (!ch422g_init())
        return false;

    /* Step 2: RGB panel */
    esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
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
        .psram_trans_align = 64,
        .sram_trans_align = 4,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            PIN_B3, PIN_B4, PIN_B5, PIN_B6, PIN_B7,  /* B[3:7] → D[0:4]  */
            PIN_G2, PIN_G3, PIN_G4, PIN_G5, PIN_G6, PIN_G7,  /* G[2:7] → D[5:10] */
            PIN_R3, PIN_R4, PIN_R5, PIN_R6, PIN_R7,  /* R[3:7] → D[11:15] */
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
    s_ch422g_output &= ~CH422G_BIT_BACKLIGHT;
    ch422g_write_reg(CH422G_OUT_REG, s_ch422g_output);

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
    if (level > 0)
        s_ch422g_output |= CH422G_BIT_BACKLIGHT;
    else
        s_ch422g_output &= ~CH422G_BIT_BACKLIGHT;

    esp_err_t err = ch422g_write_reg(CH422G_OUT_REG, s_ch422g_output);
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
