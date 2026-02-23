#include "text_render.h"
#include "font_8x16.h"
#include <string.h>

int text_render_char(uint16_t *buf, uint16_t buf_stride,
                     char ch, uint16_t fg, uint16_t bg) {
    int idx = (int)ch - 32;
    if (idx < 0 || idx >= 95)
        idx = 0; /* render space for out-of-range chars */

    const uint8_t *glyph = font_8x16_data[idx];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            buf[row * buf_stride + col] = (bits & (0x80 >> col)) ? fg : bg;
        }
    }

    return FONT_WIDTH;
}

int text_render_string(uint16_t *buf, uint16_t buf_w,
                       const char *str, uint16_t fg, uint16_t bg) {
    /* Fill entire buffer with background first */
    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (uint16_t col = 0; col < buf_w; col++) {
            buf[row * buf_w + col] = bg;
        }
    }

    int x = 0;
    for (const char *p = str; *p; p++) {
        if (x + FONT_WIDTH > buf_w)
            break; /* clip at buffer edge */

        text_render_char(&buf[x], buf_w, *p, fg, bg);
        x += FONT_WIDTH;
    }

    return x;
}
