#include "sha1.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;

    /* Pad to 64-byte blocks: data + 0x80 + zeros + 8-byte big-endian length */
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *padded = calloc(1, padded_len);
    if (!padded) return;

    memcpy(padded, data, len);
    padded[len] = 0x80;

    uint64_t bit_len = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        padded[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));
    }

    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)padded[offset + i*4]     << 24) |
                    ((uint32_t)padded[offset + i*4 + 1] << 16) |
                    ((uint32_t)padded[offset + i*4 + 2] <<  8) |
                    ((uint32_t)padded[offset + i*4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    free(padded);

    for (int i = 0; i < 4; i++) {
        out[i]      = (uint8_t)(h0 >> (24 - i*8));
        out[4 + i]  = (uint8_t)(h1 >> (24 - i*8));
        out[8 + i]  = (uint8_t)(h2 >> (24 - i*8));
        out[12 + i] = (uint8_t)(h3 >> (24 - i*8));
        out[16 + i] = (uint8_t)(h4 >> (24 - i*8));
    }
}
