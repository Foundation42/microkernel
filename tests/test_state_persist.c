#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/supervision.h"
#include "microkernel/wasm_actor.h"
#include "microkernel/state_persist.h"
#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#ifndef WASM_MODULE_DIR
#error "WASM_MODULE_DIR must be defined"
#endif

#define TEST_STATE_DIR "/tmp/mk_test_state"

/* Message types matching stateful.c */
#define MSG_SAVE_COUNT  210
#define MSG_GET_COUNT   211
#define MSG_COUNT_REPLY 212
#define MSG_INCREMENT   213

/* ── Helpers ──────────────────────────────────────────────────────── */

static void drain(runtime_t *rt, int max_steps) {
    for (int i = 0; i < max_steps; i++)
        runtime_step(rt);
}

/* Recursively remove a directory (like rm -rf) */
static void rmrf(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        rmrf(child);
    }
    closedir(d);
    rmdir(path);
}

/* Read stateful.wasm into a buffer (caller frees) */
static uint8_t *load_stateful_wasm(size_t *out_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/stateful.wasm", WASM_MODULE_DIR);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

/* Trigger message pattern: sends msg_type + payload to target actor */
#define MSG_TRIGGER 100

typedef struct {
    msg_type_t  got_type;
    uint8_t     got_payload[64];
    size_t      got_size;
    actor_id_t  got_source;
    int         reply_count;
    actor_id_t  target;
} tester_state_t;

static bool tester_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    tester_state_t *ts = state;

    if (msg->type == MSG_TRIGGER && msg->payload_size >= 4) {
        uint32_t send_type;
        memcpy(&send_type, msg->payload, 4);
        const uint8_t *payload = (const uint8_t *)msg->payload + 4;
        size_t payload_size = msg->payload_size - 4;
        actor_send(rt, ts->target, (msg_type_t)send_type, payload, payload_size);
        return true;
    }

    ts->got_type = msg->type;
    ts->got_source = msg->source;
    ts->reply_count++;
    ts->got_size = 0;
    if (msg->payload && msg->payload_size > 0) {
        size_t n = msg->payload_size < sizeof(ts->got_payload)
                   ? msg->payload_size : sizeof(ts->got_payload);
        memcpy(ts->got_payload, msg->payload, n);
        ts->got_size = msg->payload_size;
    }
    return true;
}

static void trigger_send(runtime_t *rt, actor_id_t tester,
                          msg_type_t type, const void *payload, size_t size) {
    uint8_t buf[256];
    uint32_t t = (uint32_t)type;
    memcpy(buf, &t, 4);
    if (payload && size > 0)
        memcpy(buf + 4, payload, size);
    actor_send(rt, tester, MSG_TRIGGER, buf, 4 + size);
}

/* ── Test 1: C API save/load roundtrip ────────────────────────────── */

static int test_c_save_load(void) {
    rmrf(TEST_STATE_DIR);

    runtime_t *rt = runtime_init(0, 16);
    state_persist_init(rt, TEST_STATE_DIR);

    /* Save a binary blob with embedded nulls and 0x0A bytes */
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;

    ASSERT_EQ(state_save(rt, "test_actor", "blob", data, 256), 0);

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    int n = state_load(rt, "test_actor", "blob", buf, 256);
    ASSERT_EQ(n, 256);
    ASSERT(memcmp(buf, data, 256) == 0);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 2: C API not-found ──────────────────────────────────────── */

static int test_c_not_found(void) {
    rmrf(TEST_STATE_DIR);

    runtime_t *rt = runtime_init(0, 16);
    state_persist_init(rt, TEST_STATE_DIR);

    uint8_t buf[64];
    int n = state_load(rt, "test_actor", "nonexistent", buf, 64);
    ASSERT_EQ(n, -1);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 3: C API delete ─────────────────────────────────────────── */

static int test_c_delete(void) {
    rmrf(TEST_STATE_DIR);

    runtime_t *rt = runtime_init(0, 16);
    state_persist_init(rt, TEST_STATE_DIR);

    uint8_t data[] = { 1, 2, 3, 4 };
    ASSERT_EQ(state_save(rt, "test_actor", "key", data, 4), 0);

    /* Verify it exists */
    uint8_t buf[4];
    ASSERT_EQ(state_load(rt, "test_actor", "key", buf, 4), 4);

    /* Delete and verify gone */
    ASSERT_EQ(state_delete(rt, "test_actor", "key"), 0);
    ASSERT_EQ(state_load(rt, "test_actor", "key", buf, 4), -1);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 4: WASM save/load roundtrip ─────────────────────────────── */

static int test_wasm_save_load(void) {
    rmrf(TEST_STATE_DIR);

    size_t wasm_size;
    uint8_t *wasm_buf = load_stateful_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 64);
    state_persist_init(rt, TEST_STATE_DIR);

    /* Spawn stateful.wasm and register as "counter" */
    actor_id_t wasm_id = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE,
                                           FIBER_STACK_NONE);
    ASSERT_NE(wasm_id, ACTOR_ID_INVALID);
    actor_register_name(rt, "counter", wasm_id);

    tester_state_t ts = { .target = wasm_id };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Increment 3 times */
    trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    drain(rt, 50);

    /* Save count */
    trigger_send(rt, tester, MSG_SAVE_COUNT, NULL, 0);
    drain(rt, 30);

    /* Verify count is 3 */
    ts.got_type = 0;
    ts.reply_count = 0;
    trigger_send(rt, tester, MSG_GET_COUNT, NULL, 0);
    drain(rt, 30);
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_COUNT_REPLY);
    ASSERT_EQ(ts.got_size, (size_t)4);
    int32_t count;
    memcpy(&count, ts.got_payload, 4);
    ASSERT_EQ(count, 3);

    /* Stop actor */
    actor_send(rt, wasm_id, 0, NULL, 0);
    drain(rt, 20);

    /* Spawn NEW instance with same name */
    actor_id_t wasm_id2 = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                            WASM_DEFAULT_STACK_SIZE,
                                            WASM_DEFAULT_HEAP_SIZE,
                                            FIBER_STACK_NONE);
    ASSERT_NE(wasm_id2, ACTOR_ID_INVALID);
    actor_register_name(rt, "counter", wasm_id2);

    /* Update tester target */
    ts.target = wasm_id2;
    ts.got_type = 0;
    ts.reply_count = 0;

    /* Get count — should be 3 (loaded from disk) */
    trigger_send(rt, tester, MSG_GET_COUNT, NULL, 0);
    drain(rt, 30);
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_COUNT_REPLY);
    ASSERT_EQ(ts.got_size, (size_t)4);
    memcpy(&count, ts.got_payload, 4);
    ASSERT_EQ(count, 3);

    (void)tester;
    runtime_destroy(rt);
    free(wasm_buf);
    return 0;
}

/* ── Test 5: Supervision recovery ─────────────────────────────────── */

static int test_supervision_recovery(void) {
    rmrf(TEST_STATE_DIR);

    size_t wasm_size;
    uint8_t *wasm_buf = load_stateful_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 128);
    state_persist_init(rt, TEST_STATE_DIR);

    wasm_factory_arg_t *farg = wasm_factory_arg_create(wasm_buf, wasm_size,
                                                        WASM_DEFAULT_STACK_SIZE,
                                                        WASM_DEFAULT_HEAP_SIZE,
                                                        FIBER_STACK_NONE);
    ASSERT_NOT_NULL(farg);

    child_spec_t specs[] = {
        {
            .name = "counter",
            .behavior = wasm_actor_behavior,
            .factory = wasm_actor_factory,
            .factory_arg = farg,
            .free_state = wasm_actor_free,
            .mailbox_size = 16,
            .restart_type = RESTART_PERMANENT,
        },
    };

    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                       5, 10000, specs, 1);
    ASSERT_NE(sup, ACTOR_ID_INVALID);
    drain(rt, 20);

    actor_id_t child1 = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child1, ACTOR_ID_INVALID);

    /* Register child with name so state persists under "counter" */
    actor_register_name(rt, "counter", child1);

    tester_state_t ts = { .target = child1 };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Increment 5 times + save */
    for (int i = 0; i < 5; i++)
        trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    drain(rt, 80);

    trigger_send(rt, tester, MSG_SAVE_COUNT, NULL, 0);
    drain(rt, 30);

    /* Kill actor — supervisor will restart it */
    actor_send(rt, child1, 0, NULL, 0);
    drain(rt, 40);

    /* Get restarted child */
    actor_id_t child2 = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child2, ACTOR_ID_INVALID);
    ASSERT_NE(child2, child1);

    /* Register new child with same name */
    actor_register_name(rt, "counter", child2);

    /* Verify new instance loads count=5 */
    ts.target = child2;
    ts.got_type = 0;
    ts.reply_count = 0;
    trigger_send(rt, tester, MSG_GET_COUNT, NULL, 0);
    drain(rt, 30);
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_COUNT_REPLY);
    ASSERT_EQ(ts.got_size, (size_t)4);
    int32_t count;
    memcpy(&count, ts.got_payload, 4);
    ASSERT_EQ(count, 5);

    (void)tester;
    runtime_destroy(rt);
    wasm_factory_arg_destroy(farg);
    free(wasm_buf);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    if (!wasm_actors_init()) {
        fprintf(stderr, "Failed to initialize WAMR\n");
        return 1;
    }

    printf("test_state_persist:\n");
    RUN_TEST(test_c_save_load);
    RUN_TEST(test_c_not_found);
    RUN_TEST(test_c_delete);
    RUN_TEST(test_wasm_save_load);
    RUN_TEST(test_supervision_recovery);

    /* Cleanup */
    rmrf(TEST_STATE_DIR);

    wasm_actors_cleanup();
    TEST_REPORT();
}
