#include "microkernel/state_persist.h"
#include "microkernel/runtime.h"
#include "microkernel/services.h"
#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Recursively create directories (like mkdir -p) */
static void mkdirs(const char *path) {
    char tmp[256];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Resolve actor name: use provided name, or reverse-lookup current actor,
   or fall back to _id_{seq}. */
static const char *resolve_name(runtime_t *rt, const char *actor_name,
                                 char *buf, size_t buf_size) {
    if (actor_name && actor_name[0])
        return actor_name;

    actor_id_t self = runtime_current_actor_id(rt);
    if (self != ACTOR_ID_INVALID) {
        size_t len = actor_reverse_lookup(rt, self, buf, buf_size);
        if (len > 0)
            return buf;
        snprintf(buf, buf_size, "_id_%u", (unsigned)actor_id_seq(self));
        return buf;
    }

    return "_unknown";
}

/* Build full path: {base}/{actor_name}/{key} */
static int build_path(runtime_t *rt, const char *actor_name, const char *key,
                       char *out, size_t out_size) {
    const char *base = runtime_get_state_path(rt);
    if (!base) return -1;

    char name_buf[64];
    const char *name = resolve_name(rt, actor_name, name_buf, sizeof(name_buf));

    int n = snprintf(out, out_size, "%s/%s/%s", base, name, key);
    if (n < 0 || (size_t)n >= out_size) return -1;

    /* Ensure parent directory exists */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    mkdirs(dir);

    return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

void state_persist_init(runtime_t *rt, const char *base_path) {
    runtime_set_state_path(rt, base_path);
    mkdirs(base_path);
}

int state_save(runtime_t *rt, const char *actor_name,
               const char *key, const void *data, size_t size) {
    char path[256];
    if (build_path(rt, actor_name, key, path, sizeof(path)) < 0)
        return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    return (written == size) ? 0 : -1;
}

int state_load(runtime_t *rt, const char *actor_name,
               const char *key, void *buf, size_t cap) {
    char path[256];
    if (build_path(rt, actor_name, key, path, sizeof(path)) < 0)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t n = fread(buf, 1, cap, f);
    fclose(f);

    return (int)n;
}

int state_delete(runtime_t *rt, const char *actor_name, const char *key) {
    char path[256];
    if (build_path(rt, actor_name, key, path, sizeof(path)) < 0)
        return -1;

    return (unlink(path) == 0) ? 0 : -1;
}
