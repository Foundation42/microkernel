#ifndef JSON_UTIL_H
#define JSON_UTIL_H

/*
 * Minimal JSON builder + parser (static inline, no .c file).
 * Builder: offset-tracking snprintf into a provided buffer.
 * Parser: simple string scanning for flat JSON objects.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Builder ──────────────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t cap;
    size_t off;
    int    need_comma; /* nonzero if next kv needs a comma prefix */
} json_buf_t;

static inline void json_init(json_buf_t *j, char *buf, size_t cap) {
    j->buf = buf;
    j->cap = cap;
    j->off = 0;
    j->need_comma = 0;
    if (cap > 0) buf[0] = '\0';
}

static inline void json_raw(json_buf_t *j, const char *s, size_t len) {
    if (j->off + len < j->cap) {
        memcpy(j->buf + j->off, s, len);
        j->off += len;
        j->buf[j->off] = '\0';
    }
}

static inline void json_obj_open(json_buf_t *j) {
    json_raw(j, "{", 1);
    j->need_comma = 0;
}

static inline void json_obj_close(json_buf_t *j) {
    json_raw(j, "}", 1);
}

static inline void json_comma(json_buf_t *j) {
    if (j->need_comma)
        json_raw(j, ",", 1);
    j->need_comma = 1;
}

/* Emit "key":"val" with basic escaping of \ and " in val */
static inline void json_str(json_buf_t *j, const char *key, const char *val) {
    json_comma(j);
    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "\"%s\":\"", key);
    json_raw(j, tmp, (size_t)n);

    /* Escape val */
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') {
            json_raw(j, "\\", 1);
        }
        json_raw(j, p, 1);
    }
    json_raw(j, "\"", 1);
}

static inline void json_int(json_buf_t *j, const char *key, int64_t val) {
    json_comma(j);
    char tmp[128];
    int n = snprintf(tmp, sizeof(tmp), "\"%s\":%lld", key, (long long)val);
    json_raw(j, tmp, (size_t)n);
}

static inline size_t json_len(const json_buf_t *j) {
    return j->off;
}

/* ── Parser ───────────────────────────────────────────────────────── */

/* Skip whitespace */
static inline const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/*
 * Find the value string for a given key in a flat JSON object.
 * Returns pointer into json string (past opening quote) and sets *len.
 * Returns NULL if key not found.
 */
static inline const char *json_find_key(const char *json, const char *key,
                                         size_t *len) {
    if (!json || !key) return NULL;

    /* Build search pattern: "key": */
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0) return NULL;

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += plen;
        p = json_skip_ws(p);
        if (*p != ':') continue;
        p++;
        p = json_skip_ws(p);

        if (*p == '"') {
            /* String value */
            p++;
            const char *start = p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) p++; /* skip escaped */
                p++;
            }
            *len = (size_t)(p - start);
            return start;
        } else if (*p == '[') {
            /* Array value — return pointer to '[' */
            const char *start = p;
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }
            *len = (size_t)(p - start);
            return start;
        } else {
            /* Number / bool / null */
            const char *start = p;
            while (*p && *p != ',' && *p != '}' && *p != ' ' &&
                   *p != '\n' && *p != '\r')
                p++;
            *len = (size_t)(p - start);
            return start;
        }
    }
    return NULL;
}

/*
 * Extract a string value. Returns length copied, 0 if missing.
 */
static inline size_t json_get_str(const char *json, const char *key,
                                   char *out, size_t cap) {
    size_t vlen = 0;
    const char *v = json_find_key(json, key, &vlen);
    if (!v) return 0;

    /* Check if it's a string (we were positioned after the opening quote) */
    size_t copy = vlen < cap - 1 ? vlen : cap - 1;
    memcpy(out, v, copy);
    out[copy] = '\0';
    return copy;
}

/*
 * Extract an integer value. Returns dflt if missing.
 */
static inline int64_t json_get_int(const char *json, const char *key,
                                    int64_t dflt) {
    size_t vlen = 0;
    const char *v = json_find_key(json, key, &vlen);
    if (!v || vlen == 0) return dflt;

    char tmp[32];
    size_t copy = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
    memcpy(tmp, v, copy);
    tmp[copy] = '\0';
    return strtoll(tmp, NULL, 10);
}

/*
 * Extract a JSON array of strings into "item\nitem\n" format.
 * Returns total bytes written.
 */
static inline size_t json_get_array_str(const char *json, const char *key,
                                         char *out, size_t cap) {
    size_t vlen = 0;
    const char *v = json_find_key(json, key, &vlen);
    if (!v || *v != '[') return 0;

    size_t written = 0;
    const char *p = v + 1; /* skip '[' */
    const char *end = v + vlen;

    while (p < end) {
        p = json_skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }

        if (*p == '"') {
            p++;
            const char *start = p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            size_t slen = (size_t)(p - start);
            if (*p == '"') p++;

            if (written + slen + 1 < cap) {
                memcpy(out + written, start, slen);
                written += slen;
                out[written++] = '\n';
            }
        } else {
            p++;
        }
    }

    if (written < cap) out[written] = '\0';
    return written;
}

#endif /* JSON_UTIL_H */
