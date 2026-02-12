#define _DEFAULT_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/transport_tcp.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <unistd.h>
#include <sys/wait.h>

#define TEST_PORT 19877
#define NODE1 1
#define NODE2 2
#define TARGET_ROUNDS 50
#define MSG_PING 100
#define MSG_PONG 101

/* ── Node 1 actor: receives MSG_PING, replies with MSG_PONG ───────── */

typedef struct {
    actor_id_t peer;
    int count;
} node1_state_t;

static bool node1_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    node1_state_t *s = state;
    (void)self;

    if (msg->type == MSG_PING) {
        s->count++;
        uint32_t val = (uint32_t)s->count;
        actor_send(rt, s->peer, MSG_PONG, &val, sizeof(val));
        if (s->count >= TARGET_ROUNDS) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* ── Node 2 actor: sends initial MSG_PING, receives MSG_PONG, replies MSG_PING */

typedef struct {
    actor_id_t peer;
    int count;
} node2_state_t;

static bool node2_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    node2_state_t *s = state;
    (void)self;

    if (msg->type == MSG_PONG) {
        s->count++;
        if (s->count >= TARGET_ROUNDS) {
            runtime_stop(rt);
            return false;
        }
        uint32_t val = (uint32_t)s->count;
        actor_send(rt, s->peer, MSG_PING, &val, sizeof(val));
    }
    return true;
}

/* ── Child process (node 2) ────────────────────────────────────────── */

static int run_node2(int result_fd) {
    alarm(5);

    /* Small delay to ensure server socket is ready */
    usleep(50000);

    runtime_t *rt = runtime_init(NODE2, 16);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    transport_t *tp = transport_tcp_connect("127.0.0.1", TEST_PORT, NODE1);
    if (!tp) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    if (!runtime_add_transport(rt, tp)) {
        tp->destroy(tp);
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }

    actor_id_t remote_peer = actor_id_make(NODE1, 1);
    node2_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }
    state->peer = remote_peer;
    state->count = 0;

    actor_id_t my_id = actor_spawn(rt, node2_behavior, state, NULL, 64);
    if (my_id == ACTOR_ID_INVALID) {
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }
    (void)my_id;

    /* Send initial PING to kick off the exchange */
    uint32_t init_val = 0;
    if (!actor_send(rt, remote_peer, MSG_PING, &init_val, sizeof(init_val))) {
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }

    runtime_run(rt);

    char result = (state->count >= TARGET_ROUNDS) ? 'P' : 'F';
    write(result_fd, &result, 1);

    free(state);
    runtime_destroy(rt);
    return (result == 'P') ? 0 : 1;
}

/* ── Test ──────────────────────────────────────────────────────────── */

static int test_ping_pong_tcp(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    /* Create listening socket BEFORE fork */
    transport_t *server_tp = transport_tcp_listen("127.0.0.1", TEST_PORT, NODE2);
    ASSERT_NOT_NULL(server_tp);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        /* Child inherited the listen fd — close it.
           TCP has no path to unlink, so just free the struct. */
        close(server_tp->fd);
        free(server_tp->impl);
        free(server_tp);
        int rc = run_node2(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    /* Parent process — node 1 (server) */
    close(pipefd[1]);
    alarm(5);

    runtime_t *rt = runtime_init(NODE1, 16);
    ASSERT_NOT_NULL(rt);

    ASSERT(runtime_add_transport(rt, server_tp));

    actor_id_t remote_peer = actor_id_make(NODE2, 1);
    node1_state_t *state = calloc(1, sizeof(*state));
    ASSERT_NOT_NULL(state);
    state->peer = remote_peer;
    state->count = 0;

    actor_id_t my_id = actor_spawn(rt, node1_behavior, state, NULL, 64);
    ASSERT_NE(my_id, ACTOR_ID_INVALID);
    (void)my_id;

    runtime_run(rt);

    /* Read child result */
    char child_result = 'F';
    read(pipefd[0], &child_result, 1);
    close(pipefd[0]);

    int wstatus;
    waitpid(child, &wstatus, 0);
    alarm(0);

    ASSERT_EQ(child_result, 'P');
    ASSERT(WIFEXITED(wstatus));
    ASSERT_EQ(WEXITSTATUS(wstatus), 0);
    ASSERT(state->count >= TARGET_ROUNDS);

    free(state);
    runtime_destroy(rt);
    return 0;
}

int main(void) {
    printf("test_multinode_tcp:\n");
    RUN_TEST(test_ping_pong_tcp);
    TEST_REPORT();
}
