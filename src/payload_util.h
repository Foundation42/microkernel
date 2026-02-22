#ifndef PAYLOAD_UTIL_H
#define PAYLOAD_UTIL_H

#include <string.h>
#include <stddef.h>

/* Find a value for "key=value\n" in payload text.
 * Returns the number of bytes copied (excluding NUL), or 0 if not found. */
static inline size_t payload_get(const char *payload, size_t plen,
                                  const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = payload;
    const char *end = payload + plen;

    while (p < end) {
        if ((size_t)(end - p) > klen &&
            memcmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *vstart = p + klen + 1;
            const char *vend = vstart;
            while (vend < end && *vend != '\n') vend++;
            size_t vlen = (size_t)(vend - vstart);
            size_t copy = vlen < cap - 1 ? vlen : cap - 1;
            memcpy(out, vstart, copy);
            out[copy] = '\0';
            return copy;
        }
        /* skip to next line */
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    out[0] = '\0';
    return 0;
}

#endif /* PAYLOAD_UTIL_H */
