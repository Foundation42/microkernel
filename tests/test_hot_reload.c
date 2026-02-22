#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/mailbox.h"
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

#define TEST_STATE_DIR "/tmp/mk_test_reload_state"

/* Message types matching counter_v1.c / counter_v2.c / echo.c */
#define MSG_SAVE_COUNT    210
#define MSG_GET_COUNT     211
#define MSG_COUNT_REPLY   212
#define MSG_INCREMENT     213
#define MSG_GET_VERSION   214
#define MSG_VERSION_REPLY 215

/* echo.c message types */
#define MSG_PING       200
#define MSG_PONG       201
#define MSG_RECV_TEST  205
#define MSG_RECV_REPLY 206

/* ── Helpers ──────────────────────────────────────────────────────── */

static void drain(runtime_t *rt, int max_steps) {
    for (int i = 0; i < max_steps; i++)
        runtime_step(rt);
}

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

static uint8_t *load_wasm_file(const char *name, size_t *out_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", WASM_MODULE_DIR, name);

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

/* Tester actor: trigger pattern + records replies */
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

static int32_t get_version(runtime_t *rt, actor_id_t tester,
                            tester_state_t *ts) {
    ts->got_type = 0;
    ts->reply_count = 0;
    trigger_send(rt, tester, MSG_GET_VERSION, NULL, 0);
    drain(rt, 50);
    if (ts->got_type != (msg_type_t)MSG_VERSION_REPLY || ts->got_size != 4)
        return -1;
    int32_t version;
    memcpy(&version, ts->got_payload, 4);
    return version;
}

static int32_t get_count(runtime_t *rt, actor_id_t tester,
                           tester_state_t *ts) {
    ts->got_type = 0;
    ts->reply_count = 0;
    trigger_send(rt, tester, MSG_GET_COUNT, NULL, 0);
    drain(rt, 50);
    if (ts->got_type != (msg_type_t)MSG_COUNT_REPLY || ts->got_size != 4)
        return -1;
    int32_t count;
    memcpy(&count, ts->got_payload, 4);
    return count;
}

/* ── Test 1: Basic reload ─────────────────────────────────────────── */

static int test_basic_reload(void) {
    size_t v1_size, v2_size;
    uint8_t *v1_buf = load_wasm_file("counter_v1.wasm", &v1_size);
    uint8_t *v2_buf = load_wasm_file("counter_v2.wasm", &v2_size);
    ASSERT_NOT_NULL(v1_buf);
    ASSERT_NOT_NULL(v2_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t ctr = actor_spawn_wasm(rt, v1_buf, v1_size, 16,
                                       WASM_DEFAULT_STACK_SIZE,
                                       WASM_DEFAULT_HEAP_SIZE,
                                       FIBER_STACK_NONE);
    ASSERT_NE(ctr, ACTOR_ID_INVALID);

    tester_state_t ts = { .target = ctr };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Verify version 1 */
    ASSERT_EQ(get_version(rt, tester, &ts), 1);

    /* Reload with v2 */
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, ctr, v2_buf, v2_size, &new_id);
    ASSERT_EQ(rc, RELOAD_OK);
    ASSERT_NE(new_id, ACTOR_ID_INVALID);
    ASSERT_NE(new_id, ctr);

    /* Verify version 2 */
    ts.target = new_id;
    ASSERT_EQ(get_version(rt, tester, &ts), 2);

    (void)tester;
    runtime_destroy(rt);
    free(v1_buf);
    free(v2_buf);
    return 0;
}

/* ── Test 2: Name preservation ────────────────────────────────────── */

static int test_name_preservation(void) {
    size_t v1_size, v2_size;
    uint8_t *v1_buf = load_wasm_file("counter_v1.wasm", &v1_size);
    uint8_t *v2_buf = load_wasm_file("counter_v2.wasm", &v2_size);
    ASSERT_NOT_NULL(v1_buf);
    ASSERT_NOT_NULL(v2_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t ctr = actor_spawn_wasm(rt, v1_buf, v1_size, 16,
                                       WASM_DEFAULT_STACK_SIZE,
                                       WASM_DEFAULT_HEAP_SIZE,
                                       FIBER_STACK_NONE);
    ASSERT_NE(ctr, ACTOR_ID_INVALID);
    ASSERT(actor_register_name(rt, "myactor", ctr));
    ASSERT_EQ(actor_lookup(rt, "myactor"), ctr);

    /* Reload */
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, ctr, v2_buf, v2_size, &new_id);
    ASSERT_EQ(rc, RELOAD_OK);

    /* Name should point to new actor */
    ASSERT_EQ(actor_lookup(rt, "myactor"), new_id);
    ASSERT_NE(new_id, ctr);

    runtime_destroy(rt);
    free(v1_buf);
    free(v2_buf);
    return 0;
}

/* ── Test 3: Mailbox forwarding ───────────────────────────────────── */

static int test_mailbox_forwarding(void) {
    size_t v1_size, v2_size;
    uint8_t *v1_buf = load_wasm_file("counter_v1.wasm", &v1_size);
    uint8_t *v2_buf = load_wasm_file("counter_v2.wasm", &v2_size);
    ASSERT_NOT_NULL(v1_buf);
    ASSERT_NOT_NULL(v2_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t ctr = actor_spawn_wasm(rt, v1_buf, v1_size, 16,
                                       WASM_DEFAULT_STACK_SIZE,
                                       WASM_DEFAULT_HEAP_SIZE,
                                       FIBER_STACK_NONE);
    ASSERT_NE(ctr, ACTOR_ID_INVALID);

    /* Enqueue 3 MSG_INCREMENT directly (don't drain yet) */
    actor_send(rt, ctr, MSG_INCREMENT, NULL, 0);
    actor_send(rt, ctr, MSG_INCREMENT, NULL, 0);
    actor_send(rt, ctr, MSG_INCREMENT, NULL, 0);

    /* Reload with v2 — messages should be forwarded */
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, ctr, v2_buf, v2_size, &new_id);
    ASSERT_EQ(rc, RELOAD_OK);

    /* Drain to process forwarded messages */
    drain(rt, 50);

    /* Get count: should be 3 (3 messages processed by v2 after forward,
       but v2 increments by 10... wait, the 3 messages were enqueued for v1.
       After reload to v2, v2 processes them: 3 * 10 = 30.
       Actually, let's check: the messages are MSG_INCREMENT. v2 adds 10 each.
       So 3 * 10 = 30. But the counter_v2.c starts at count=0 (new instance). */
    tester_state_t ts = { .target = new_id };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);
    int32_t count = get_count(rt, tester, &ts);
    /* 3 increments * 10 (v2) = 30 */
    ASSERT_EQ(count, 30);

    (void)tester;
    runtime_destroy(rt);
    free(v1_buf);
    free(v2_buf);
    return 0;
}

/* ── Test 4: State persistence across reload ──────────────────────── */

static int test_state_persistence(void) {
    rmrf(TEST_STATE_DIR);

    size_t v1_size, v2_size;
    uint8_t *v1_buf = load_wasm_file("counter_v1.wasm", &v1_size);
    uint8_t *v2_buf = load_wasm_file("counter_v2.wasm", &v2_size);
    ASSERT_NOT_NULL(v1_buf);
    ASSERT_NOT_NULL(v2_buf);

    runtime_t *rt = runtime_init(0, 64);
    state_persist_init(rt, TEST_STATE_DIR);

    actor_id_t ctr = actor_spawn_wasm(rt, v1_buf, v1_size, 16,
                                       WASM_DEFAULT_STACK_SIZE,
                                       WASM_DEFAULT_HEAP_SIZE,
                                       FIBER_STACK_NONE);
    ASSERT_NE(ctr, ACTOR_ID_INVALID);
    /* Register with a name so state persists under a stable path */
    ASSERT(actor_register_name(rt, "ctr", ctr));

    tester_state_t ts = { .target = ctr };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Increment 3 times and save (v1 adds 1 each) */
    trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    trigger_send(rt, tester, MSG_INCREMENT, NULL, 0);
    drain(rt, 60);
    trigger_send(rt, tester, MSG_SAVE_COUNT, NULL, 0);
    drain(rt, 30);

    /* Verify count = 3 */
    ASSERT_EQ(get_count(rt, tester, &ts), 3);

    /* Reload with v2 */
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, ctr, v2_buf, v2_size, &new_id);
    ASSERT_EQ(rc, RELOAD_OK);
    drain(rt, 30);

    /* v2 loads persisted state on first message */
    ts.target = new_id;
    ASSERT_EQ(get_count(rt, tester, &ts), 3);
    ASSERT_EQ(get_version(rt, tester, &ts), 2);

    (void)tester;
    runtime_destroy(rt);
    free(v1_buf);
    free(v2_buf);
    return 0;
}

/* ── Test 5: Supervised reload ────────────────────────────────────── */

static int test_supervised_reload(void) {
    size_t v1_size, v2_size;
    uint8_t *v1_buf = load_wasm_file("counter_v1.wasm", &v1_size);
    uint8_t *v2_buf = load_wasm_file("counter_v2.wasm", &v2_size);
    ASSERT_NOT_NULL(v1_buf);
    ASSERT_NOT_NULL(v2_buf);

    runtime_t *rt = runtime_init(0, 128);

    wasm_factory_arg_t *farg = wasm_factory_arg_create(v1_buf, v1_size,
                                                        WASM_DEFAULT_STACK_SIZE,
                                                        WASM_DEFAULT_HEAP_SIZE,
                                                        FIBER_STACK_NONE);
    ASSERT_NOT_NULL(farg);

    child_spec_t specs[] = {
        {
            .name = "ctr",
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

    tester_state_t ts = { .target = child1 };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Verify initial version is 1 */
    ASSERT_EQ(get_version(rt, tester, &ts), 1);

    /* Reload child with v2 */
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, child1, v2_buf, v2_size, &new_id);
    ASSERT_EQ(rc, RELOAD_OK);
    drain(rt, 30);

    /* Verify new child is version 2 */
    ts.target = new_id;
    ASSERT_EQ(get_version(rt, tester, &ts), 2);

    /* Supervisor should know about new child */
    ASSERT_EQ(supervisor_get_child(rt, sup, 0), new_id);

    /* Kill the new child — supervisor should restart with v2 factory */
    actor_send(rt, new_id, 0, NULL, 0);
    drain(rt, 60);

    actor_id_t child3 = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child3, ACTOR_ID_INVALID);
    ASSERT_NE(child3, new_id);

    /* Verify restarted child is still version 2 (NOT 1) */
    ts.target = child3;
    ASSERT_EQ(get_version(rt, tester, &ts), 2);

    /* Get the current factory_arg (v2) from supervisor before destroying runtime.
       The old farg (v1) was already destroyed by actor_reload_wasm. */
    wasm_factory_arg_t *v2_farg = supervisor_get_factory_arg(rt, sup, 0);

    (void)tester;
    runtime_destroy(rt);
    wasm_factory_arg_destroy(v2_farg);
    free(v1_buf);
    free(v2_buf);
    return 0;
}

/* ── Test 6: Fiber active rejection ───────────────────────────────── */

static int test_fiber_active_rejection(void) {
    size_t echo_size, v2_size;
    uint8_t *echo_buf = load_wasm_file("echo.wasm", &echo_size);
    uint8_t *v2_buf = load_wasm_file("counter_v2.wasm", &v2_size);
    ASSERT_NOT_NULL(echo_buf);
    ASSERT_NOT_NULL(v2_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t echo = actor_spawn_wasm(rt, echo_buf, echo_size, 16,
                                        WASM_DEFAULT_STACK_SIZE,
                                        WASM_DEFAULT_HEAP_SIZE,
                                        FIBER_STACK_SMALL);
    ASSERT_NE(echo, ACTOR_ID_INVALID);

    tester_state_t ts = { .target = echo };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Send RECV_TEST — echo.wasm calls mk_recv, fiber yields */
    trigger_send(rt, tester, MSG_RECV_TEST, NULL, 0);
    drain(rt, 20);

    /* Try to reload while fiber is yielded */
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, echo, v2_buf, v2_size, &new_id);
    ASSERT_EQ(rc, RELOAD_ERR_FIBER_ACTIVE);
    ASSERT_EQ(new_id, ACTOR_ID_INVALID);

    /* Resume fiber by sending a message */
    trigger_send(rt, tester, MSG_PING, "hello", 5);
    drain(rt, 30);

    /* Actor should still work */
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_RECV_REPLY);

    (void)tester;
    runtime_destroy(rt);
    free(echo_buf);
    free(v2_buf);
    return 0;
}

/* ── Test 7: Bad module rejection ─────────────────────────────────── */

static int test_bad_module_rejection(void) {
    size_t v1_size;
    uint8_t *v1_buf = load_wasm_file("counter_v1.wasm", &v1_size);
    ASSERT_NOT_NULL(v1_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t ctr = actor_spawn_wasm(rt, v1_buf, v1_size, 16,
                                       WASM_DEFAULT_STACK_SIZE,
                                       WASM_DEFAULT_HEAP_SIZE,
                                       FIBER_STACK_NONE);
    ASSERT_NE(ctr, ACTOR_ID_INVALID);

    tester_state_t ts = { .target = ctr };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Try to reload with garbage bytes */
    uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, ctr, garbage, sizeof(garbage), &new_id);
    ASSERT_EQ(rc, RELOAD_ERR_MODULE_LOAD);
    ASSERT_EQ(new_id, ACTOR_ID_INVALID);

    /* Original actor should still be alive and working */
    ASSERT_EQ(get_version(rt, tester, &ts), 1);

    (void)tester;
    runtime_destroy(rt);
    free(v1_buf);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    if (!wasm_actors_init()) {
        fprintf(stderr, "Failed to initialize WAMR\n");
        return 1;
    }

    printf("test_hot_reload:\n");
    RUN_TEST(test_basic_reload);
    RUN_TEST(test_name_preservation);
    RUN_TEST(test_mailbox_forwarding);
    RUN_TEST(test_state_persistence);
    RUN_TEST(test_supervised_reload);
    RUN_TEST(test_fiber_active_rejection);
    RUN_TEST(test_bad_module_rejection);

    /* Cleanup */
    rmrf(TEST_STATE_DIR);

    wasm_actors_cleanup();
    TEST_REPORT();
}
