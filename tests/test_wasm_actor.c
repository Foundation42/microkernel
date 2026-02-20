#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/supervision.h"
#include "microkernel/wasm_actor.h"
#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef WASM_MODULE_DIR
#error "WASM_MODULE_DIR must be defined"
#endif

/* ── Helpers ──────────────────────────────────────────────────────── */

static void drain(runtime_t *rt, int max_steps) {
    for (int i = 0; i < max_steps; i++)
        runtime_step(rt);
}

/* Read echo.wasm into a buffer (caller frees) */
static uint8_t *load_echo_wasm(size_t *out_size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/echo.wasm", WASM_MODULE_DIR);

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

/* Message types matching echo.c */
#define MSG_PING       200
#define MSG_PONG       201
#define MSG_GET_SELF   202
#define MSG_SELF_REPLY 203

/* Trigger message: tells tester to send a message to target */
#define MSG_TRIGGER    100

/* Native tester actor: on MSG_TRIGGER, sends payload to target with
   the msg_type stored in first byte of trigger payload.
   Records any other incoming messages for later assertion. */
typedef struct {
    msg_type_t  got_type;
    uint8_t     got_payload[64];
    size_t      got_size;
    actor_id_t  got_source;
    int         reply_count;
    actor_id_t  target;      /* WASM actor to talk to */
} tester_state_t;

static bool tester_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    tester_state_t *ts = state;

    if (msg->type == MSG_TRIGGER && msg->payload_size >= 4) {
        /* First 4 bytes = msg_type to send, rest = payload */
        uint32_t send_type;
        memcpy(&send_type, msg->payload, 4);
        const uint8_t *payload = (const uint8_t *)msg->payload + 4;
        size_t payload_size = msg->payload_size - 4;
        actor_send(rt, ts->target, (msg_type_t)send_type, payload, payload_size);
        return true;
    }

    /* Record reply */
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

/* Helper: send a trigger message to the tester actor */
static void trigger_send(runtime_t *rt, actor_id_t tester,
                          msg_type_t type, const void *payload, size_t size) {
    uint8_t buf[256];
    uint32_t t = (uint32_t)type;
    memcpy(buf, &t, 4);
    if (payload && size > 0)
        memcpy(buf + 4, payload, size);
    actor_send(rt, tester, MSG_TRIGGER, buf, 4 + size);
}

/* ── Test 1: Spawn WASM actor ─────────────────────────────────────── */

static int test_wasm_spawn(void) {
    size_t wasm_size;
    uint8_t *wasm_buf = load_echo_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t wasm_id = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE);
    ASSERT_NE(wasm_id, ACTOR_ID_INVALID);

    runtime_destroy(rt);
    free(wasm_buf);
    return 0;
}

/* ── Test 2: Echo (PING -> PONG) ──────────────────────────────────── */

static int test_wasm_echo(void) {
    size_t wasm_size;
    uint8_t *wasm_buf = load_echo_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t wasm_id = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE);
    ASSERT_NE(wasm_id, ACTOR_ID_INVALID);

    tester_state_t ts = { .target = wasm_id };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    /* Trigger tester to send PING with "hello" to WASM actor */
    trigger_send(rt, tester, MSG_PING, "hello", 5);
    drain(rt, 30);

    /* Tester should have received PONG with same payload */
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_PONG);
    ASSERT_EQ(ts.got_size, (size_t)5);
    ASSERT(memcmp(ts.got_payload, "hello", 5) == 0);
    ASSERT_EQ(ts.got_source, wasm_id);
    (void)tester;

    runtime_destroy(rt);
    free(wasm_buf);
    return 0;
}

/* ── Test 3: mk_self() ────────────────────────────────────────────── */

static int test_wasm_self(void) {
    size_t wasm_size;
    uint8_t *wasm_buf = load_echo_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t wasm_id = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE);
    ASSERT_NE(wasm_id, ACTOR_ID_INVALID);

    tester_state_t ts = { .target = wasm_id };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    trigger_send(rt, tester, MSG_GET_SELF, NULL, 0);
    drain(rt, 30);

    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_SELF_REPLY);
    ASSERT_EQ(ts.got_size, (size_t)8);

    /* The payload is the wasm actor's own id as int64 */
    actor_id_t reported_id;
    memcpy(&reported_id, ts.got_payload, 8);
    ASSERT_EQ(reported_id, wasm_id);
    (void)tester;

    runtime_destroy(rt);
    free(wasm_buf);
    return 0;
}

/* ── Test 4: Stop signal ──────────────────────────────────────────── */

static int test_wasm_stop(void) {
    size_t wasm_size;
    uint8_t *wasm_buf = load_echo_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t wasm_id = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE);
    ASSERT_NE(wasm_id, ACTOR_ID_INVALID);

    /* Send type 0 from outside — source=ACTOR_ID_INVALID is fine,
       echo.c just returns 0 on type 0 regardless of source */
    actor_send(rt, wasm_id, 0, NULL, 0);
    drain(rt, 20);

    /* Actor should be stopped: sending should fail */
    ASSERT(!actor_send(rt, wasm_id, MSG_PING, "x", 1));

    runtime_destroy(rt);
    free(wasm_buf);
    return 0;
}

/* ── Test 5: Supervision ──────────────────────────────────────────── */

static int test_wasm_supervision(void) {
    size_t wasm_size;
    uint8_t *wasm_buf = load_echo_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 128);

    wasm_factory_arg_t *farg = wasm_factory_arg_create(wasm_buf, wasm_size,
                                                        WASM_DEFAULT_STACK_SIZE,
                                                        WASM_DEFAULT_HEAP_SIZE);
    ASSERT_NOT_NULL(farg);

    child_spec_t specs[] = {
        {
            .name = "wasm-echo",
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

    /* Verify child works via tester */
    tester_state_t ts = { .target = child1 };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);
    trigger_send(rt, tester, MSG_PING, "hi", 2);
    drain(rt, 30);
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_PONG);

    /* Kill child via stop signal (returns 0 from handle_message) */
    actor_send(rt, child1, 0, NULL, 0);
    drain(rt, 40);

    /* Child should have been restarted with a new ID */
    actor_id_t child2 = supervisor_get_child(rt, sup, 0);
    ASSERT_NE(child2, ACTOR_ID_INVALID);
    ASSERT_NE(child2, child1);

    /* Verify new child works */
    ts.target = child2;
    ts.reply_count = 0;
    ts.got_type = 0;
    trigger_send(rt, tester, MSG_PING, "yo", 2);
    drain(rt, 30);
    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_PONG);
    (void)tester;

    runtime_destroy(rt);
    wasm_factory_arg_destroy(farg);
    free(wasm_buf);
    return 0;
}

/* ── Test 6: Named WASM actor ─────────────────────────────────────── */

static int test_wasm_named(void) {
    size_t wasm_size;
    uint8_t *wasm_buf = load_echo_wasm(&wasm_size);
    ASSERT_NOT_NULL(wasm_buf);

    runtime_t *rt = runtime_init(0, 64);
    actor_id_t wasm_id = actor_spawn_wasm(rt, wasm_buf, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE);
    ASSERT_NE(wasm_id, ACTOR_ID_INVALID);

    /* Register the WASM actor with a name */
    ASSERT(actor_register_name(rt, "echo", wasm_id));
    ASSERT_EQ(actor_lookup(rt, "echo"), wasm_id);

    /* Spawn tester and have it send via the tester behavior
       (so source is correctly set to tester's id).
       We use the WASM actor ID as target for trigger_send,
       then verify the reply comes back. */
    tester_state_t ts = { .target = wasm_id };
    actor_id_t tester = actor_spawn(rt, tester_behavior, &ts, NULL, 16);

    trigger_send(rt, tester, MSG_PING, "named", 5);
    drain(rt, 30);

    ASSERT_EQ(ts.got_type, (msg_type_t)MSG_PONG);
    ASSERT_EQ(ts.got_size, (size_t)5);
    ASSERT(memcmp(ts.got_payload, "named", 5) == 0);
    (void)tester;

    runtime_destroy(rt);
    free(wasm_buf);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    if (!wasm_actors_init()) {
        fprintf(stderr, "Failed to initialize WAMR\n");
        return 1;
    }

    printf("test_wasm_actor:\n");
    RUN_TEST(test_wasm_spawn);
    RUN_TEST(test_wasm_echo);
    RUN_TEST(test_wasm_self);
    RUN_TEST(test_wasm_stop);
    RUN_TEST(test_wasm_supervision);
    RUN_TEST(test_wasm_named);

    wasm_actors_cleanup();
    TEST_REPORT();
}
