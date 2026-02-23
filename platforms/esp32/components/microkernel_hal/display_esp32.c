#include "display_hal.h"
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_sh8601.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <string.h>

static const char *TAG = "display_hal";

/* ── Waveshare ESP32-S3-Touch-AMOLED-1.43 pin definitions ─────────── */

#define DISPLAY_WIDTH   466
#define DISPLAY_HEIGHT  466

#define PIN_SCLK  11
#define PIN_D0    12
#define PIN_D1    13
#define PIN_D2    14
#define PIN_D3    15
#define PIN_CS    10
#define PIN_RST   38
#define PIN_EN    43

#define SPI_HOST_ID     SPI2_HOST
#define SPI_FREQ_HZ     (40 * 1000 * 1000)  /* 40 MHz */
#define LCD_CMD_BITS     32
#define LCD_PARAM_BITS   8

/* ── State ────────────────────────────────────────────────────────── */

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static bool s_initialized;

/* ── Row buffer for fill (466 * 2 = 932 bytes on stack) ────────── */

#define ROW_BUF_PIXELS  DISPLAY_WIDTH

/* ── HAL interface ────────────────────────────────────────────────── */

bool display_hal_init(void) {
    /* Drive EN high to enable the display */
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << PIN_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en_cfg);
    gpio_set_level(PIN_EN, 1);

    /* Initialize QSPI bus */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCLK,
        .data0_io_num = PIN_D0,
        .data1_io_num = PIN_D1,
        .data2_io_num = PIN_D2,
        .data3_io_num = PIN_D3,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    esp_err_t err = spi_bus_initialize(SPI_HOST_ID, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return false;
    }

    /* Panel IO — QSPI mode, no DC pin */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .flags = {
            .quad_mode = true,
        },
    };
    err = esp_lcd_new_panel_io_spi(SPI_HOST_ID, &io_cfg, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(err));
        spi_bus_free(SPI_HOST_ID);
        return false;
    }

    /* SH8601 panel with QSPI vendor config */
    sh8601_vendor_config_t vendor_cfg = {
        .flags = {
            .use_qspi_interface = true,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    err = esp_lcd_new_panel_sh8601(s_panel_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_sh8601: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_panel_io);
        spi_bus_free(SPI_HOST_ID);
        return false;
    }

    /* Reset → init → display on */
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_initialized = true;
    ESP_LOGI(TAG, "SH8601 AMOLED initialized (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return true;
}

void display_hal_deinit(void) {
    if (!s_initialized)
        return;
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_del(s_panel);
    esp_lcd_panel_io_del(s_panel_io);
    spi_bus_free(SPI_HOST_ID);
    gpio_set_level(PIN_EN, 0);
    s_panel = NULL;
    s_panel_io = NULL;
    s_initialized = false;
}

/* SH8601 requires even coordinates — round down to even */
static inline uint16_t even_floor(uint16_t v) { return v & ~1; }
static inline uint16_t even_ceil(uint16_t v)  { return (v + 1) & ~1; }

bool display_hal_draw(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      const uint8_t *data) {
    if (!s_initialized)
        return false;

    /* Align to even coordinates for SH8601 */
    uint16_t x0 = even_floor(x);
    uint16_t y0 = even_floor(y);
    uint16_t x1 = even_ceil(x + w) - 1;
    uint16_t y1 = even_ceil(y + h) - 1;

    if (x1 >= DISPLAY_WIDTH || y1 >= DISPLAY_HEIGHT)
        return false;

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0,
                                               x1 + 1, y1 + 1, data);
    return err == ESP_OK;
}

bool display_hal_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t color) {
    if (!s_initialized)
        return false;
    if (x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT)
        return false;

    /* Fill row-at-a-time with a stack buffer to avoid large heap allocs */
    uint16_t row_buf[ROW_BUF_PIXELS];
    uint16_t fill_w = (w <= ROW_BUF_PIXELS) ? w : ROW_BUF_PIXELS;

    for (uint16_t i = 0; i < fill_w; i++)
        row_buf[i] = color;

    /* Align to even coordinates */
    uint16_t x0 = even_floor(x);
    uint16_t x1 = even_ceil(x + w);

    for (uint16_t row = 0; row < h; row++) {
        uint16_t cy = even_floor(y + row);
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, cy,
                                                   x1, cy + 1, row_buf);
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
    /* SH8601 supports brightness via panel_io command (0x51) */
    uint8_t param = level;
    esp_err_t err = esp_lcd_panel_io_tx_param(s_panel_io, 0x5100, &param, 1);
    return err == ESP_OK;
}

bool display_hal_power(bool on) {
    if (!s_initialized)
        return false;
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, on);
    return err == ESP_OK;
}

uint16_t display_hal_width(void) {
    return DISPLAY_WIDTH;
}

uint16_t display_hal_height(void) {
    return DISPLAY_HEIGHT;
}
