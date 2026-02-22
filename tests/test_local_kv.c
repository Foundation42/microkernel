#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/cf_proxy.h"
#include "microkernel/local_kv.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ── Tester actor: state machine that sends KV requests sequentially ── */

typedef struct {
    actor_id_t kv_id;
    int        step;
    bool       done;

    /* Results */
    bool       init_ok;
    bool       put_ok;
    bool       get_ok;
    char       get_value[256];
    size_t     get_value_size;
    bool       not_found_ok;
    bool       delete_ok;
    bool       list_ok;
    char       list_keys[1024];
    size_t     list_keys_size;
    bool       overwrite_ok;
    char       overwrite_value[256];
    bool       slash_ok;
    char       slash_value[256];
    msg_type_t last_reply_type;
} kv_tester_state_t;

/* ── test_init: just verify paths are registered ──────────────────── */

static bool init_tester_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1) {
        /* Give init a scheduler step to complete registration */
        s->init_ok = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }
    return true;
}

static int test_init(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);
    ASSERT_NE(kv_id, ACTOR_ID_INVALID);

    /* Check path registration */
    ASSERT_EQ(actor_lookup(rt, "/node/local/storage/kv"), kv_id);
    ASSERT_EQ(actor_lookup(rt, "/node/storage/kv"), kv_id);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, init_tester_behavior, &ts, NULL, 8);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.init_ok);

    runtime_destroy(rt);
    /* Clean up temp dir */
    rmdir(tmpdir);
    return 0;
}

/* ── test_put_get: PUT then GET, verify value matches ─────────────── */

static bool put_get_tester(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "key=testkey\nvalue=hello_world";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->put_ok = true;
        s->step = 2;
        const char *p = "key=testkey";
        actor_send(rt, s->kv_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_VALUE && s->step == 2) {
        s->get_ok = true;
        size_t copy = msg->payload_size < sizeof(s->get_value) - 1
                     ? msg->payload_size : sizeof(s->get_value) - 1;
        memcpy(s->get_value, msg->payload, copy);
        s->get_value[copy] = '\0';
        s->get_value_size = copy;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_put_get(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, put_get_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.put_ok);
    ASSERT(ts.get_ok);
    ASSERT(strcmp(ts.get_value, "hello_world") == 0);

    runtime_destroy(rt);

    /* Clean up */
    char path[512];
    snprintf(path, sizeof(path), "%s/testkey", tmpdir);
    unlink(path);
    rmdir(tmpdir);
    return 0;
}

/* ── test_get_not_found: GET nonexistent key ──────────────────────── */

static bool not_found_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1) {
        const char *p = "key=nonexistent";
        actor_send(rt, s->kv_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_NOT_FOUND) {
        s->not_found_ok = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_get_not_found(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, not_found_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.not_found_ok);

    runtime_destroy(rt);
    rmdir(tmpdir);
    return 0;
}

/* ── test_delete: PUT + DELETE + GET → not found ──────────────────── */

static bool delete_tester(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "key=delme\nvalue=temp";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->step = 2;
        const char *p = "key=delme";
        actor_send(rt, s->kv_id, MSG_CF_KV_DELETE, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 2) {
        s->delete_ok = true;
        s->step = 3;
        const char *p = "key=delme";
        actor_send(rt, s->kv_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_NOT_FOUND && s->step == 3) {
        s->not_found_ok = true;
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_delete(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, delete_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.delete_ok);
    ASSERT(ts.not_found_ok);

    runtime_destroy(rt);
    rmdir(tmpdir);
    return 0;
}

/* ── test_list_all: PUT 3 keys, LIST empty prefix → all 3 ────────── */

static bool list_all_tester(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "key=alpha\nvalue=a";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->step = 2;
        const char *p = "key=beta\nvalue=b";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 2) {
        s->step = 3;
        const char *p = "key=gamma\nvalue=c";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 3) {
        s->step = 4;
        const char *p = "prefix=\nlimit=50";
        actor_send(rt, s->kv_id, MSG_CF_KV_LIST, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_KEYS && s->step == 4) {
        s->list_ok = true;
        s->list_keys_size = msg->payload_size < sizeof(s->list_keys) - 1
                           ? msg->payload_size : sizeof(s->list_keys) - 1;
        memcpy(s->list_keys, msg->payload, s->list_keys_size);
        s->list_keys[s->list_keys_size] = '\0';
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_list_all(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, list_all_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.list_ok);
    ASSERT(strstr(ts.list_keys, "alpha") != NULL);
    ASSERT(strstr(ts.list_keys, "beta") != NULL);
    ASSERT(strstr(ts.list_keys, "gamma") != NULL);

    runtime_destroy(rt);

    /* Clean up */
    char path[512];
    snprintf(path, sizeof(path), "%s/alpha", tmpdir); unlink(path);
    snprintf(path, sizeof(path), "%s/beta", tmpdir); unlink(path);
    snprintf(path, sizeof(path), "%s/gamma", tmpdir); unlink(path);
    rmdir(tmpdir);
    return 0;
}

/* ── test_list_prefix: PUT keys with different prefixes, LIST with filter */

static bool list_prefix_tester(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "key=log_one\nvalue=a";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->step = 2;
        const char *p = "key=log_two\nvalue=b";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 2) {
        s->step = 3;
        const char *p = "key=other\nvalue=c";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 3) {
        s->step = 4;
        const char *p = "prefix=log_";
        actor_send(rt, s->kv_id, MSG_CF_KV_LIST, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_KEYS && s->step == 4) {
        s->list_ok = true;
        s->list_keys_size = msg->payload_size < sizeof(s->list_keys) - 1
                           ? msg->payload_size : sizeof(s->list_keys) - 1;
        memcpy(s->list_keys, msg->payload, s->list_keys_size);
        s->list_keys[s->list_keys_size] = '\0';
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_list_prefix(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, list_prefix_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.list_ok);
    ASSERT(strstr(ts.list_keys, "log_one") != NULL);
    ASSERT(strstr(ts.list_keys, "log_two") != NULL);
    ASSERT(strstr(ts.list_keys, "other") == NULL);

    runtime_destroy(rt);

    /* Clean up */
    char path[512];
    snprintf(path, sizeof(path), "%s/log_uone", tmpdir); unlink(path);
    snprintf(path, sizeof(path), "%s/log_utwo", tmpdir); unlink(path);
    snprintf(path, sizeof(path), "%s/other", tmpdir); unlink(path);
    rmdir(tmpdir);
    return 0;
}

/* ── test_overwrite: PUT same key twice, GET returns latest ──────── */

static bool overwrite_tester(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "key=mykey\nvalue=first";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->step = 2;
        const char *p = "key=mykey\nvalue=second";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 2) {
        s->step = 3;
        const char *p = "key=mykey";
        actor_send(rt, s->kv_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_VALUE && s->step == 3) {
        s->overwrite_ok = true;
        size_t copy = msg->payload_size < sizeof(s->overwrite_value) - 1
                     ? msg->payload_size : sizeof(s->overwrite_value) - 1;
        memcpy(s->overwrite_value, msg->payload, copy);
        s->overwrite_value[copy] = '\0';
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_overwrite(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, overwrite_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.overwrite_ok);
    ASSERT(strcmp(ts.overwrite_value, "second") == 0);

    runtime_destroy(rt);

    /* Clean up */
    char path[512];
    snprintf(path, sizeof(path), "%s/mykey", tmpdir); unlink(path);
    rmdir(tmpdir);
    return 0;
}

/* ── test_path_with_slash: PUT key containing /, verify round-trip ── */

static bool slash_tester(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)self;
    kv_tester_state_t *s = state;

    if (msg->type == 1 && s->step == 0) {
        s->step = 1;
        const char *p = "key=history/user1\nvalue=cmd1";
        actor_send(rt, s->kv_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->step = 2;
        const char *p = "key=history/user1";
        actor_send(rt, s->kv_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    if (msg->type == MSG_CF_VALUE && s->step == 2) {
        s->slash_ok = true;
        size_t copy = msg->payload_size < sizeof(s->slash_value) - 1
                     ? msg->payload_size : sizeof(s->slash_value) - 1;
        memcpy(s->slash_value, msg->payload, copy);
        s->slash_value[copy] = '\0';
        s->done = true;
        runtime_stop(rt);
        return false;
    }

    return true;
}

static int test_path_with_slash(void) {
    char tmpdir[] = "/tmp/mk_kv_test_XXXXXX";
    ASSERT(mkdtemp(tmpdir) != NULL);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    local_kv_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.base_path, tmpdir, sizeof(cfg.base_path) - 1);

    actor_id_t kv_id = local_kv_init(rt, &cfg);

    kv_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.kv_id = kv_id;

    actor_id_t tester = actor_spawn(rt, slash_tester, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.slash_ok);
    ASSERT(strcmp(ts.slash_value, "cmd1") == 0);

    runtime_destroy(rt);

    /* Clean up: encoded filename is history__user1 */
    char path[512];
    snprintf(path, sizeof(path), "%s/history__user1", tmpdir); unlink(path);
    rmdir(tmpdir);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_local_kv:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_put_get);
    RUN_TEST(test_get_not_found);
    RUN_TEST(test_delete);
    RUN_TEST(test_list_all);
    RUN_TEST(test_list_prefix);
    RUN_TEST(test_overwrite);
    RUN_TEST(test_path_with_slash);

    TEST_REPORT();
}
