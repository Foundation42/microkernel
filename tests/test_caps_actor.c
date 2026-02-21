#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"

/* ── Tester actor: sends caps request, captures reply ──────────────── */

typedef struct {
    actor_id_t caps_id;
    bool       got_reply;
    char       reply[512];
    size_t     reply_size;
} caps_tester_state_t;

static bool caps_tester_behavior(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state) {
    (void)self;
    caps_tester_state_t *s = state;

    if (msg->type == 1) {
        /* Trigger: send caps request */
        actor_send(rt, s->caps_id, MSG_CAPS_REQUEST, NULL, 0);
        return true;
    }

    if (msg->type == MSG_CAPS_REPLY) {
        s->got_reply = true;
        s->reply_size = msg->payload_size < sizeof(s->reply) - 1
                        ? msg->payload_size : sizeof(s->reply) - 1;
        memcpy(s->reply, msg->payload, s->reply_size);
        s->reply[s->reply_size] = '\0';
        return false;
    }

    return true;
}

/* ── Helper: find value for key in "key=value\n" text ──────────────── */

static const char *find_value(const char *text, const char *key) {
    size_t klen = strlen(key);
    const char *p = text;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return p + klen + 1;
        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}

static int parse_int_value(const char *text, const char *key) {
    const char *v = find_value(text, key);
    return v ? atoi(v) : -1;
}

/* ── Test 1: caps_actor_init + basic request/reply ─────────────────── */

static int test_caps_basic(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t caps_id = caps_actor_init(rt);
    ASSERT_NE(caps_id, ACTOR_ID_INVALID);

    /* Verify caps actor is registered at /node/<identity>/caps */
    char path[128];
    snprintf(path, sizeof(path), "/node/%s/caps", mk_node_identity());
    actor_id_t looked_up = actor_lookup(rt, path);
    ASSERT_EQ(looked_up, caps_id);

    /* Spawn tester actor to send request and receive reply */
    caps_tester_state_t ts = { .caps_id = caps_id };
    actor_id_t tester = actor_spawn(rt, caps_tester_behavior, &ts, NULL, 16);
    actor_send(rt, tester, 1, NULL, 0);

    /* Pump scheduler until tester stops */
    for (int i = 0; i < 100 && !ts.got_reply; i++)
        runtime_step(rt);

    ASSERT(ts.got_reply);
    ASSERT(ts.reply_size > 0);
    ASSERT(strstr(ts.reply, "platform=linux") != NULL);
    ASSERT(strstr(ts.reply, "identity=") != NULL);
    ASSERT(strstr(ts.reply, "max_actors=") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 2: validate parsed key=value pairs ───────────────────────── */

static int test_caps_values(void) {
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);
    actor_id_t caps_id = caps_actor_init(rt);

    caps_tester_state_t ts = { .caps_id = caps_id };
    actor_id_t tester = actor_spawn(rt, caps_tester_behavior, &ts, NULL, 16);
    actor_send(rt, tester, 1, NULL, 0);

    for (int i = 0; i < 100 && !ts.got_reply; i++)
        runtime_step(rt);

    ASSERT(ts.got_reply);

    /* node_id should match what we passed to runtime_init */
    int node_id = parse_int_value(ts.reply, "node_id");
    ASSERT_EQ(node_id, 1);

    /* max_actors should be 64 */
    int max_actors = parse_int_value(ts.reply, "max_actors");
    ASSERT_EQ(max_actors, 64);

    /* actor_count > 0 (at least ns, caps, tester) */
    int actor_count = parse_int_value(ts.reply, "actor_count");
    ASSERT(actor_count > 0);

    /* http=true */
    ASSERT(strstr(ts.reply, "http=true") != NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_caps_actor\n");

    RUN_TEST(test_caps_basic);
    RUN_TEST(test_caps_values);

    TEST_REPORT();
}
