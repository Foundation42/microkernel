#include "base64.h"

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *data, size_t len, char *out) {
    size_t i, j = 0;

    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = b64[(data[i] >> 2) & 0x3F];
        out[j++] = b64[((data[i] & 0x3) << 4) | (data[i+1] >> 4)];
        out[j++] = b64[((data[i+1] & 0xF) << 2) | (data[i+2] >> 6)];
        out[j++] = b64[data[i+2] & 0x3F];
    }

    if (i < len) {
        out[j++] = b64[(data[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = b64[((data[i] & 0x3) << 4) | (data[i+1] >> 4)];
            out[j++] = b64[(data[i+1] & 0xF) << 2];
        } else {
            out[j++] = b64[(data[i] & 0x3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }

    out[j] = '\0';
    return j;
}
