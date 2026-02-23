#ifndef TEXT_RENDER_H
#define TEXT_RENDER_H

#include <stdint.h>
#include <stddef.h>

/* Font dimensions */
#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/*
 * Render a single character to an RGB565 buffer.
 * buf must be at least FONT_WIDTH * FONT_HEIGHT * 2 bytes.
 * Returns the number of pixels wide (always FONT_WIDTH for valid chars).
 */
int text_render_char(uint16_t *buf, uint16_t buf_stride,
                     char ch, uint16_t fg, uint16_t bg);

/*
 * Render a null-terminated string to an RGB565 row buffer.
 * buf_w = width of buffer in pixels (characters beyond this are clipped).
 * buf must be at least buf_w * FONT_HEIGHT uint16_t values.
 * Returns width in pixels of rendered text (may exceed buf_w if clipped).
 */
int text_render_string(uint16_t *buf, uint16_t buf_w,
                       const char *str, uint16_t fg, uint16_t bg);

#endif /* TEXT_RENDER_H */
