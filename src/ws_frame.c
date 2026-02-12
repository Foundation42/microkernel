#include "ws_frame.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

int ws_frame_parse_header(const uint8_t *buf, size_t len,
                          ws_frame_info_t *info) {
    if (len < 2) return 0;

    info->fin    = (buf[0] & 0x80) != 0;
    info->opcode = buf[0] & 0x0F;
    info->masked = (buf[1] & 0x80) != 0;

    uint64_t plen = buf[1] & 0x7F;
    size_t pos = 2;

    if (plen == 126) {
        if (len < 4) return 0;
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        pos = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) {
            plen = (plen << 8) | buf[2 + i];
        }
        pos = 10;
    }

    if (info->masked) {
        if (len < pos + 4) return 0;
        memcpy(info->mask_key, buf + pos, 4);
        pos += 4;
    }

    info->payload_length = plen;
    info->header_size = pos;
    return (int)pos;
}

void ws_frame_apply_mask(uint8_t *data, size_t len,
                         const uint8_t mask[4], size_t offset) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask[(offset + i) & 3];
    }
}

/* Simple random mask key generation */
static void random_mask(uint8_t mask[4]) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = true;
    }
    for (int i = 0; i < 4; i++) {
        mask[i] = (uint8_t)(rand() & 0xFF);
    }
}

size_t ws_frame_build(uint8_t opcode, bool fin,
                      const uint8_t *payload, size_t payload_len,
                      uint8_t *out) {
    size_t pos = 0;

    out[pos++] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);

    /* Client frames are always masked */
    if (payload_len < 126) {
        out[pos++] = 0x80 | (uint8_t)payload_len;
    } else if (payload_len <= 0xFFFF) {
        out[pos++] = 0x80 | 126;
        out[pos++] = (uint8_t)(payload_len >> 8);
        out[pos++] = (uint8_t)(payload_len & 0xFF);
    } else {
        out[pos++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) {
            out[pos++] = (uint8_t)(payload_len >> (i * 8));
        }
    }

    uint8_t mask[4];
    random_mask(mask);
    memcpy(out + pos, mask, 4);
    pos += 4;

    if (payload && payload_len > 0) {
        memcpy(out + pos, payload, payload_len);
        ws_frame_apply_mask(out + pos, payload_len, mask, 0);
        pos += payload_len;
    }

    return pos;
}

size_t ws_frame_build_close(uint16_t code, const char *reason,
                            size_t reason_len, uint8_t *out) {
    uint8_t payload[128];
    size_t plen = 0;

    payload[plen++] = (uint8_t)(code >> 8);
    payload[plen++] = (uint8_t)(code & 0xFF);

    if (reason && reason_len > 0) {
        size_t copy = reason_len < sizeof(payload) - 2 ? reason_len : sizeof(payload) - 2;
        memcpy(payload + plen, reason, copy);
        plen += copy;
    }

    return ws_frame_build(WS_OPCODE_CLOSE, true, payload, plen, out);
}

size_t ws_frame_build_unmasked(uint8_t opcode, bool fin,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *out) {
    size_t pos = 0;

    out[pos++] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);

    /* Server frames are unmasked */
    if (payload_len < 126) {
        out[pos++] = (uint8_t)payload_len;
    } else if (payload_len <= 0xFFFF) {
        out[pos++] = 126;
        out[pos++] = (uint8_t)(payload_len >> 8);
        out[pos++] = (uint8_t)(payload_len & 0xFF);
    } else {
        out[pos++] = 127;
        for (int i = 7; i >= 0; i--) {
            out[pos++] = (uint8_t)(payload_len >> (i * 8));
        }
    }

    if (payload && payload_len > 0) {
        memcpy(out + pos, payload, payload_len);
        pos += payload_len;
    }

    return pos;
}

size_t ws_frame_build_close_unmasked(uint16_t code, const char *reason,
                                     size_t reason_len, uint8_t *out) {
    uint8_t payload[128];
    size_t plen = 0;

    payload[plen++] = (uint8_t)(code >> 8);
    payload[plen++] = (uint8_t)(code & 0xFF);

    if (reason && reason_len > 0) {
        size_t copy = reason_len < sizeof(payload) - 2 ? reason_len : sizeof(payload) - 2;
        memcpy(payload + plen, reason, copy);
        plen += copy;
    }

    return ws_frame_build_unmasked(WS_OPCODE_CLOSE, true, payload, plen, out);
}
