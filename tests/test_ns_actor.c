#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static bool noop_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

/* Echo behavior: counts messages of a given type */
typedef struct { int count; } echo_state_t;

static bool echo_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)rt; (void)self;
    echo_state_t *es = state;
    if (msg->type == 42) es->count++;
    return true;
}

/* ── Test 1: ns_actor_init registers "ns" and is discoverable ──────── */

static int test_ns_init(void) {
    runtime_t *rt = runtime_init(0, 64);
    actor_id_t ns_id = ns_actor_init(rt);

    ASSERT_NE(ns_id, ACTOR_ID_INVALID);
    ASSERT_EQ(actor_lookup(rt, "ns"), ns_id);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 2: register and lookup hierarchical path ─────────────────── */

static int test_register_path(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t foo_id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Register "/test/foo" */
    ns_register_t reg;
    memset(&reg, 0, sizeof(reg));
    strncpy(reg.path, "/test/foo", NS_PATH_MAX - 1);
    reg.actor_id = foo_id;

    ns_reply_t reply;
    int rc = ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);

    /* Lookup "/test/foo" */
    ns_lookup_t look;
    memset(&look, 0, sizeof(look));
    strncpy(look.path, "/test/foo", NS_PATH_MAX - 1);

    rc = ns_call(rt, MSG_NS_LOOKUP, &look, sizeof(look), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);
    ASSERT_EQ(reply.actor_id, foo_id);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 3: lookup nonexistent path returns ENOENT ────────────────── */

static int test_lookup_missing(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    ns_lookup_t look;
    memset(&look, 0, sizeof(look));
    strncpy(look.path, "/nonexistent", NS_PATH_MAX - 1);

    ns_reply_t reply;
    int rc = ns_call(rt, MSG_NS_LOOKUP, &look, sizeof(look), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_ENOENT);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 4: list paths by prefix ──────────────────────────────────── */

static int test_list_prefix(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t a = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    actor_id_t b = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Register two paths under /test */
    ns_register_t reg;
    ns_reply_t reply;

    memset(&reg, 0, sizeof(reg));
    strncpy(reg.path, "/test/alpha", NS_PATH_MAX - 1);
    reg.actor_id = a;
    ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(reply.status, NS_OK);

    memset(&reg, 0, sizeof(reg));
    strncpy(reg.path, "/test/beta", NS_PATH_MAX - 1);
    reg.actor_id = b;
    ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(reply.status, NS_OK);

    /* Register one outside /test */
    memset(&reg, 0, sizeof(reg));
    strncpy(reg.path, "/other/gamma", NS_PATH_MAX - 1);
    reg.actor_id = a;
    ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(reply.status, NS_OK);

    /* List "/test" prefix — should find alpha and beta but not gamma */
    ns_list_req_t list_req;
    memset(&list_req, 0, sizeof(list_req));
    strncpy(list_req.prefix, "/test", NS_PATH_MAX - 1);

    int rc = ns_call(rt, MSG_NS_LIST, &list_req, sizeof(list_req), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);
    ASSERT(reply.data_len > 0);
    ASSERT(strstr(reply.data, "/test/alpha=") != NULL);
    ASSERT(strstr(reply.data, "/test/beta=") != NULL);
    ASSERT(strstr(reply.data, "/other/gamma=") == NULL);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 5: bare name via ns actor (delegates to flat registry) ───── */

static int test_bare_name(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t svc = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Register bare name "bar" via ns actor */
    ns_register_t reg;
    memset(&reg, 0, sizeof(reg));
    strncpy(reg.path, "bar", NS_PATH_MAX - 1);
    reg.actor_id = svc;

    ns_reply_t reply;
    int rc = ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);

    /* Lookup bare name "bar" via ns actor */
    ns_lookup_t look;
    memset(&look, 0, sizeof(look));
    strncpy(look.path, "bar", NS_PATH_MAX - 1);

    rc = ns_call(rt, MSG_NS_LOOKUP, &look, sizeof(look), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);
    ASSERT_EQ(reply.actor_id, svc);

    /* Also verify flat registry was updated directly */
    ASSERT_EQ(actor_lookup(rt, "bar"), svc);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 6: mount and lookup through mount point ──────────────────── */

static int test_mount_umount(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t proxy = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Mount /remote on proxy actor */
    ns_mount_t mnt;
    memset(&mnt, 0, sizeof(mnt));
    strncpy(mnt.mount_point, "/remote", NS_PATH_MAX - 1);
    mnt.target = proxy;

    ns_reply_t reply;
    int rc = ns_call(rt, MSG_NS_MOUNT, &mnt, sizeof(mnt), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);

    /* Lookup /remote/foo — should resolve to mount target */
    ns_lookup_t look;
    memset(&look, 0, sizeof(look));
    strncpy(look.path, "/remote/foo", NS_PATH_MAX - 1);

    rc = ns_call(rt, MSG_NS_LOOKUP, &look, sizeof(look), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);
    ASSERT_EQ(reply.actor_id, proxy);

    /* Umount /remote */
    ns_umount_t umnt;
    memset(&umnt, 0, sizeof(umnt));
    strncpy(umnt.mount_point, "/remote", NS_PATH_MAX - 1);

    rc = ns_call(rt, MSG_NS_UMOUNT, &umnt, sizeof(umnt), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);

    /* Lookup /remote/foo should now fail */
    rc = ns_call(rt, MSG_NS_LOOKUP, &look, sizeof(look), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_ENOENT);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 7: duplicate path registration fails ─────────────────────── */

static int test_duplicate_path(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t a = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    actor_id_t b = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    ns_register_t reg;
    ns_reply_t reply;

    memset(&reg, 0, sizeof(reg));
    strncpy(reg.path, "/svc/unique", NS_PATH_MAX - 1);
    reg.actor_id = a;
    ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(reply.status, NS_OK);

    /* Duplicate should fail */
    reg.actor_id = b;
    ns_call(rt, MSG_NS_REGISTER, &reg, sizeof(reg), &reply);
    ASSERT_EQ(reply.status, NS_EEXIST);

    /* Original still there */
    ns_lookup_t look;
    memset(&look, 0, sizeof(look));
    strncpy(look.path, "/svc/unique", NS_PATH_MAX - 1);
    ns_call(rt, MSG_NS_LOOKUP, &look, sizeof(look), &reply);
    ASSERT_EQ(reply.status, NS_OK);
    ASSERT_EQ(reply.actor_id, a);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 8: duplicate mount fails ─────────────────────────────────── */

static int test_duplicate_mount(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t p1 = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    actor_id_t p2 = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    ns_mount_t mnt;
    ns_reply_t reply;

    memset(&mnt, 0, sizeof(mnt));
    strncpy(mnt.mount_point, "/mnt/dev", NS_PATH_MAX - 1);
    mnt.target = p1;
    ns_call(rt, MSG_NS_MOUNT, &mnt, sizeof(mnt), &reply);
    ASSERT_EQ(reply.status, NS_OK);

    mnt.target = p2;
    ns_call(rt, MSG_NS_MOUNT, &mnt, sizeof(mnt), &reply);
    ASSERT_EQ(reply.status, NS_EEXIST);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 9: transparent register + lookup via actor_register_name ──── */

static int test_transparent_register_path(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t worker = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Register via public API — transparently routes to path table */
    bool ok = actor_register_name(rt, "/svc/worker", worker);
    ASSERT(ok);

    /* Lookup via public API */
    actor_id_t found = actor_lookup(rt, "/svc/worker");
    ASSERT_EQ(found, worker);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 10: transparent send_named via path ──────────────────────── */

static int test_transparent_send_named(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    echo_state_t es = {0};
    actor_id_t echo = actor_spawn(rt, echo_behavior, &es, NULL, 16);

    actor_register_name(rt, "/svc/echo", echo);

    /* Send via path name */
    bool ok = actor_send_named(rt, "/svc/echo", 42, "hi", 2);
    ASSERT(ok);

    /* Step to deliver */
    runtime_step(rt);
    ASSERT_EQ(es.count, 1);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 11: path ops before ns_actor_init gracefully fail ────────── */

static int test_path_before_ns_init(void) {
    runtime_t *rt = runtime_init(0, 64);
    /* No ns_actor_init() */

    actor_id_t worker = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Register should fail (ns_state is NULL) */
    bool ok = actor_register_name(rt, "/foo", worker);
    ASSERT(!ok);

    /* Lookup should return INVALID */
    actor_id_t found = actor_lookup(rt, "/foo");
    ASSERT_EQ(found, ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 12: transparent mount lookup via actor_lookup ─────────────── */

static int test_transparent_mount_lookup(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t proxy = actor_spawn(rt, noop_behavior, NULL, NULL, 16);

    /* Mount /mnt/proxy via ns_call (mount is still message-based) */
    ns_mount_t mnt;
    memset(&mnt, 0, sizeof(mnt));
    strncpy(mnt.mount_point, "/mnt/proxy", NS_PATH_MAX - 1);
    mnt.target = proxy;

    ns_reply_t reply;
    int rc = ns_call(rt, MSG_NS_MOUNT, &mnt, sizeof(mnt), &reply);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(reply.status, NS_OK);

    /* Lookup through mount via public API */
    actor_id_t found = actor_lookup(rt, "/mnt/proxy/sub");
    ASSERT_EQ(found, proxy);

    runtime_destroy(rt);
    return 0;
}

/* ── Test 13: path cleanup on actor death ──────────────────────────── */

static int test_path_cleanup_on_death(void) {
    runtime_t *rt = runtime_init(0, 64);
    ns_actor_init(rt);

    actor_id_t mortal = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    actor_register_name(rt, "/svc/mortal", mortal);

    /* Verify registered */
    ASSERT_EQ(actor_lookup(rt, "/svc/mortal"), mortal);

    /* Kill actor and step to clean up */
    actor_stop(rt, mortal);
    runtime_step(rt);

    /* Path should be gone */
    ASSERT_EQ(actor_lookup(rt, "/svc/mortal"), ACTOR_ID_INVALID);

    runtime_destroy(rt);
    return 0;
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_ns_actor:\n");
    RUN_TEST(test_ns_init);
    RUN_TEST(test_register_path);
    RUN_TEST(test_lookup_missing);
    RUN_TEST(test_list_prefix);
    RUN_TEST(test_bare_name);
    RUN_TEST(test_mount_umount);
    RUN_TEST(test_duplicate_path);
    RUN_TEST(test_duplicate_mount);
    RUN_TEST(test_transparent_register_path);
    RUN_TEST(test_transparent_send_named);
    RUN_TEST(test_path_before_ns_init);
    RUN_TEST(test_transparent_mount_lookup);
    RUN_TEST(test_path_cleanup_on_death);
    TEST_REPORT();
}
