#define _DEFAULT_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/transport_tcp.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

#define TEST_PORT 19895
#define NODE_A 1
#define NODE_B 2
#define MSG_PING  200
#define MSG_PONG  201
#define MSG_INIT  202

/* ══════════════════════════════════════════════════════════════════════
 *  Shared noop behavior
 * ══════════════════════════════════════════════════════════════════════ */

static bool noop_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 1: Path sync on mount
 *
 *  Node A: ns_actor_init + register /test/svc1 + mount listener
 *  Node B: ns_actor_init + ns_mount_connect, poll until /test/svc1 visible
 * ══════════════════════════════════════════════════════════════════════ */

/* Node A manager: register /test/svc1 after a short delay, stop after 2s */
typedef struct {
    actor_id_t svc_id;
    int step;
} path_mgr_state_t;

static bool path_mgr_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    path_mgr_state_t *s = state;

    if (msg->type == MSG_INIT) {
        /* Wait for child to connect */
        actor_set_timer(rt, 100, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            actor_register_name(rt, "/test/svc1", s->svc_id);
            actor_set_timer(rt, 2000, false);
            s->step = 2;
        } else if (s->step == 2) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: poll for /test/svc1 to appear */
typedef struct {
    bool found;
    actor_id_t resolved_id;
    int polls;
} path_wait_state_t;

static bool path_wait_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    path_wait_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        actor_id_t id = actor_lookup(rt, "/test/svc1");
        s->polls++;
        if (id != ACTOR_ID_INVALID) {
            s->found = true;
            s->resolved_id = id;
            runtime_stop(rt);
            return false;
        }
        if (s->polls < 200) {
            actor_set_timer(rt, 10, false);
        } else {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

static int run_node_b_path_sync(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 32);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    ns_actor_init(rt);

    mount_result_t mresult;
    if (ns_mount_connect(rt, "127.0.0.1", TEST_PORT, &mresult) != 0) {
        runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    path_wait_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, path_wait_behavior, state, NULL, 64);
    if (my_id == ACTOR_ID_INVALID) {
        free(state); runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    actor_send(rt, my_id, MSG_INIT, NULL, 0);
    runtime_run(rt);

    char result = 'F';
    if (state->found && actor_id_node(state->resolved_id) == NODE_A) {
        result = 'P';
    }
    write(result_fd, &result, 1);

    free(state);
    runtime_destroy(rt);
    return (result == 'P') ? 0 : 1;
}

static int test_path_sync_on_mount(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    runtime_t *rt = runtime_init(NODE_A, 32);
    ASSERT_NOT_NULL(rt);

    ns_actor_init(rt);

    /* Spawn a service actor */
    actor_id_t svc_id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    ASSERT_NE(svc_id, ACTOR_ID_INVALID);

    /* Start mount listener */
    actor_id_t listener = ns_mount_listen(rt, TEST_PORT);
    ASSERT_NE(listener, ACTOR_ID_INVALID);

    /* Manager registers the path after delay */
    path_mgr_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->svc_id = svc_id;

    actor_id_t mgr_id = actor_spawn(rt, path_mgr_behavior, mgr_state, NULL, 16);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        int rc = run_node_b_path_sync(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_run(rt);

    char child_result = 'F';
    read(pipefd[0], &child_result, 1);
    close(pipefd[0]);

    int wstatus;
    waitpid(child, &wstatus, 0);
    alarm(0);

    ASSERT_EQ(child_result, 'P');
    ASSERT(WIFEXITED(wstatus));
    ASSERT_EQ(WEXITSTATUS(wstatus), 0);

    free(mgr_state);
    runtime_destroy(rt);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 2: Remote send via path
 *
 *  Node A: register /test/echo with echo actor, mount listener
 *  Node B: connect, lookup /test/echo, send MSG_PING, wait for MSG_PONG
 * ══════════════════════════════════════════════════════════════════════ */

static bool echo_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self; (void)state;
    if (msg->type == MSG_PING) {
        uint32_t val = 1;
        actor_send(rt, msg->source, MSG_PONG, &val, sizeof(val));
    }
    return true;
}

/* Node A manager: register /test/echo, wait, stop */
typedef struct {
    actor_id_t echo_id;
    int step;
} echo_mgr_state_t;

static bool echo_mgr_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    echo_mgr_state_t *s = state;

    if (msg->type == MSG_INIT) {
        actor_set_timer(rt, 100, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            actor_register_name(rt, "/test/echo", s->echo_id);
            actor_set_timer(rt, 2000, false);
            s->step = 2;
        } else if (s->step == 2) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: lookup /test/echo, send PING, wait for PONG */
typedef struct {
    int pong_count;
    int polls;
} remote_ping_state_t;

static bool remote_ping_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    remote_ping_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        actor_id_t id = actor_lookup(rt, "/test/echo");
        s->polls++;
        if (id != ACTOR_ID_INVALID) {
            actor_send(rt, id, MSG_PING, NULL, 0);
        } else if (s->polls < 200) {
            actor_set_timer(rt, 10, false);
        } else {
            runtime_stop(rt);
            return false;
        }
    } else if (msg->type == MSG_PONG) {
        s->pong_count++;
        runtime_stop(rt);
        return false;
    }
    return true;
}

static int run_node_b_remote_send(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 32);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    ns_actor_init(rt);

    mount_result_t mresult;
    if (ns_mount_connect(rt, "127.0.0.1", TEST_PORT + 1, &mresult) != 0) {
        runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    remote_ping_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, remote_ping_behavior, state, NULL, 64);
    if (my_id == ACTOR_ID_INVALID) {
        free(state); runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    actor_send(rt, my_id, MSG_INIT, NULL, 0);
    runtime_run(rt);

    char result = (state->pong_count >= 1) ? 'P' : 'F';
    write(result_fd, &result, 1);

    free(state);
    runtime_destroy(rt);
    return (result == 'P') ? 0 : 1;
}

static int test_remote_send_via_path(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    runtime_t *rt = runtime_init(NODE_A, 32);
    ASSERT_NOT_NULL(rt);

    ns_actor_init(rt);

    /* Spawn echo actor */
    actor_id_t echo_id = actor_spawn(rt, echo_behavior, NULL, NULL, 64);
    ASSERT_NE(echo_id, ACTOR_ID_INVALID);

    /* Start mount listener */
    actor_id_t listener = ns_mount_listen(rt, TEST_PORT + 1);
    ASSERT_NE(listener, ACTOR_ID_INVALID);

    /* Manager registers /test/echo after delay */
    echo_mgr_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->echo_id = echo_id;

    actor_id_t mgr_id = actor_spawn(rt, echo_mgr_behavior, mgr_state, NULL, 16);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        int rc = run_node_b_remote_send(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_run(rt);

    char child_result = 'F';
    read(pipefd[0], &child_result, 1);
    close(pipefd[0]);

    int wstatus;
    waitpid(child, &wstatus, 0);
    alarm(0);

    ASSERT_EQ(child_result, 'P');
    ASSERT(WIFEXITED(wstatus));
    ASSERT_EQ(WEXITSTATUS(wstatus), 0);

    free(mgr_state);
    runtime_destroy(rt);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 3: Bidirectional path sync
 *
 *  Node A: register /test/a_svc, mount listener
 *  Node B: connect, register /test/b_svc
 *  Node A: verify /test/b_svc appears
 *  Node B: verify /test/a_svc appears
 * ══════════════════════════════════════════════════════════════════════ */

/* Node A manager: register /test/a_svc, then poll for /test/b_svc */
typedef struct {
    actor_id_t svc_id;
    bool found_b;
    int step;
    int polls;
} bidir_mgr_state_t;

static bool bidir_mgr_behavior(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    bidir_mgr_state_t *s = state;

    if (msg->type == MSG_INIT) {
        /* Register our path immediately (before connect) so it syncs on connect */
        actor_register_name(rt, "/test/a_svc", s->svc_id);
        /* Wait for child to connect */
        actor_set_timer(rt, 200, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            /* Poll for /test/b_svc */
            actor_id_t id = actor_lookup(rt, "/test/b_svc");
            s->polls++;
            if (id != ACTOR_ID_INVALID) {
                s->found_b = true;
                /* Give child time to verify, then stop */
                actor_set_timer(rt, 500, false);
                s->step = 2;
            } else if (s->polls < 200) {
                actor_set_timer(rt, 10, false);
            } else {
                runtime_stop(rt);
                return false;
            }
        } else if (s->step == 2) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: connect, register /test/b_svc, poll for /test/a_svc */
typedef struct {
    bool found_a;
    int polls;
} bidir_b_state_t;

static bool bidir_b_behavior(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    bidir_b_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        actor_id_t id = actor_lookup(rt, "/test/a_svc");
        s->polls++;
        if (id != ACTOR_ID_INVALID) {
            s->found_a = true;
            runtime_stop(rt);
            return false;
        }
        if (s->polls < 200) {
            actor_set_timer(rt, 10, false);
        } else {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

static int run_node_b_bidir(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 32);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    ns_actor_init(rt);

    mount_result_t mresult;
    if (ns_mount_connect(rt, "127.0.0.1", TEST_PORT + 2, &mresult) != 0) {
        runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    /* Register our own path */
    actor_id_t b_svc = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    if (b_svc == ACTOR_ID_INVALID) {
        runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }
    actor_register_name(rt, "/test/b_svc", b_svc);

    bidir_b_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, bidir_b_behavior, state, NULL, 64);
    if (my_id == ACTOR_ID_INVALID) {
        free(state); runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    actor_send(rt, my_id, MSG_INIT, NULL, 0);
    runtime_run(rt);

    char result = state->found_a ? 'P' : 'F';
    write(result_fd, &result, 1);

    free(state);
    runtime_destroy(rt);
    return (result == 'P') ? 0 : 1;
}

static int test_bidirectional_path_sync(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    runtime_t *rt = runtime_init(NODE_A, 32);
    ASSERT_NOT_NULL(rt);

    ns_actor_init(rt);

    /* Spawn service actor */
    actor_id_t a_svc = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    ASSERT_NE(a_svc, ACTOR_ID_INVALID);

    /* Start mount listener */
    actor_id_t listener = ns_mount_listen(rt, TEST_PORT + 2);
    ASSERT_NE(listener, ACTOR_ID_INVALID);

    /* Manager registers /test/a_svc and polls for /test/b_svc */
    bidir_mgr_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->svc_id = a_svc;

    actor_id_t mgr_id = actor_spawn(rt, bidir_mgr_behavior, mgr_state, NULL, 64);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        int rc = run_node_b_bidir(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_run(rt);

    char child_result = 'F';
    read(pipefd[0], &child_result, 1);
    close(pipefd[0]);

    int wstatus;
    waitpid(child, &wstatus, 0);
    alarm(0);

    ASSERT_EQ(child_result, 'P');
    ASSERT(WIFEXITED(wstatus));
    ASSERT_EQ(WEXITSTATUS(wstatus), 0);

    /* Verify Node A also found /test/b_svc */
    ASSERT(mgr_state->found_b);

    free(mgr_state);
    runtime_destroy(rt);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_ns_remote:\n");
    RUN_TEST(test_path_sync_on_mount);
    RUN_TEST(test_remote_send_via_path);
    RUN_TEST(test_bidirectional_path_sync);
    TEST_REPORT();
}
