#include "microkernel/local_kv.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/cf_proxy.h"
#include "microkernel/namespace.h"
#include "payload_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define LKV_MAX_KEY      128
#define LKV_MAX_VALUE    4096
#define LKV_MAX_PATH     256
#define LKV_MAX_KEYS_BUF 4096

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    char base_path[64];
} local_kv_state_t;

/* ── Key encoding/decoding ────────────────────────────────────────── */

/* Encode a raw key for filesystem use: _ -> _u, / -> __ */
static size_t encode_key(const char *raw, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; raw[i] && j < cap - 2; i++) {
        if (raw[i] == '_') {
            out[j++] = '_';
            if (j < cap - 1) out[j++] = 'u';
        } else if (raw[i] == '/') {
            out[j++] = '_';
            if (j < cap - 1) out[j++] = '_';
        } else {
            out[j++] = raw[i];
        }
    }
    out[j] = '\0';
    return j;
}

/* Decode a filesystem-safe key back to raw: _u -> _, __ -> / */
static size_t decode_key(const char *encoded, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; encoded[i] && j < cap - 1; i++) {
        if (encoded[i] == '_' && encoded[i + 1] == 'u') {
            out[j++] = '_';
            i++;
        } else if (encoded[i] == '_' && encoded[i + 1] == '_') {
            out[j++] = '/';
            i++;
        } else {
            out[j++] = encoded[i];
        }
    }
    out[j] = '\0';
    return j;
}

/* Build full file path: base/encoded_key */
static size_t build_path(const char *base, const char *encoded,
                          char *out, size_t cap) {
    return (size_t)snprintf(out, cap, "%s/%s", base, encoded);
}

/* ── File operations ──────────────────────────────────────────────── */

static void handle_put(local_kv_state_t *s, runtime_t *rt, message_t *msg) {
    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char key[LKV_MAX_KEY] = "";
    char value[LKV_MAX_VALUE] = "";

    payload_get(payload, plen, "key", key, sizeof(key));
    size_t vlen = payload_get(payload, plen, "value", value, sizeof(value));

    if (!key[0]) {
        const char *err = "missing key";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }

    char encoded[LKV_MAX_KEY];
    encode_key(key, encoded, sizeof(encoded));

    char path[LKV_MAX_PATH];
    build_path(s->base_path, encoded, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (!f) {
        const char *err = "write failed";
        actor_send(rt, msg->source, MSG_CF_ERROR, err, strlen(err));
        return;
    }
    if (vlen > 0)
        fwrite(value, 1, vlen, f);
    fclose(f);

    actor_send(rt, msg->source, MSG_CF_OK, NULL, 0);
}

static void handle_get(local_kv_state_t *s, runtime_t *rt, message_t *msg) {
    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char key[LKV_MAX_KEY] = "";

    payload_get(payload, plen, "key", key, sizeof(key));

    if (!key[0]) {
        actor_send(rt, msg->source, MSG_CF_NOT_FOUND, NULL, 0);
        return;
    }

    char encoded[LKV_MAX_KEY];
    encode_key(key, encoded, sizeof(encoded));

    char path[LKV_MAX_PATH];
    build_path(s->base_path, encoded, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        actor_send(rt, msg->source, MSG_CF_NOT_FOUND, NULL, 0);
        return;
    }

    char value[LKV_MAX_VALUE];
    size_t n = fread(value, 1, sizeof(value), f);
    fclose(f);

    actor_send(rt, msg->source, MSG_CF_VALUE, value, n);
}

static void handle_delete(local_kv_state_t *s, runtime_t *rt, message_t *msg) {
    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char key[LKV_MAX_KEY] = "";

    payload_get(payload, plen, "key", key, sizeof(key));

    if (key[0]) {
        char encoded[LKV_MAX_KEY];
        encode_key(key, encoded, sizeof(encoded));

        char path[LKV_MAX_PATH];
        build_path(s->base_path, encoded, path, sizeof(path));

        remove(path);
    }

    actor_send(rt, msg->source, MSG_CF_OK, NULL, 0);
}

static void handle_list(local_kv_state_t *s, runtime_t *rt, message_t *msg) {
    const char *payload = msg->payload;
    size_t plen = msg->payload_size;
    char prefix[LKV_MAX_KEY] = "";

    payload_get(payload, plen, "prefix", prefix, sizeof(prefix));

    DIR *d = opendir(s->base_path);
    if (!d) {
        actor_send(rt, msg->source, MSG_CF_KEYS, "", 0);
        return;
    }

    char keys_buf[LKV_MAX_KEYS_BUF];
    size_t keys_len = 0;
    size_t prefix_len = strlen(prefix);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char decoded[LKV_MAX_KEY];
        decode_key(ent->d_name, decoded, sizeof(decoded));

        /* Apply prefix filter */
        if (prefix_len > 0 && strncmp(decoded, prefix, prefix_len) != 0)
            continue;

        size_t dlen = strlen(decoded);
        if (keys_len + dlen + 1 >= sizeof(keys_buf))
            break;

        if (keys_len > 0)
            keys_buf[keys_len++] = '\n';
        memcpy(keys_buf + keys_len, decoded, dlen);
        keys_len += dlen;
    }
    closedir(d);

    actor_send(rt, msg->source, MSG_CF_KEYS, keys_buf, keys_len);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool local_kv_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    local_kv_state_t *s = state;

    switch (msg->type) {
    case MSG_CF_KV_PUT:
        handle_put(s, rt, msg);
        return true;
    case MSG_CF_KV_GET:
        handle_get(s, rt, msg);
        return true;
    case MSG_CF_KV_DELETE:
        handle_delete(s, rt, msg);
        return true;
    case MSG_CF_KV_LIST:
        handle_list(s, rt, msg);
        return true;
    default:
        return true;
    }
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t local_kv_init(runtime_t *rt, const local_kv_config_t *config) {
    local_kv_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ACTOR_ID_INVALID;

    snprintf(s->base_path, sizeof(s->base_path), "%s", config->base_path);

    /* Ensure base directory exists (Linux; ESP32 SPIFFS has flat namespace) */
#ifndef ESP_PLATFORM
    mkdir(s->base_path, 0755);
#endif

    actor_id_t id = actor_spawn(rt, local_kv_behavior, s, free, 32);
    if (id == ACTOR_ID_INVALID) {
        free(s);
        return ACTOR_ID_INVALID;
    }

    /* Register canonical local path (always) */
    actor_register_name(rt, "/node/local/storage/kv", id);

    /* Register virtual short path (only if unclaimed) */
    if (actor_lookup(rt, "/node/storage/kv") == ACTOR_ID_INVALID)
        actor_register_name(rt, "/node/storage/kv", id);

    return id;
}
