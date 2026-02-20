#define _DEFAULT_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/transport_tcp.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include <unistd.h>
#include <sys/wait.h>

#define TEST_PORT 19891
#define NODE_A 1
#define NODE_B 2
#define MSG_PING  200
#define MSG_PONG  201
#define MSG_INIT  202

/* ══════════════════════════════════════════════════════════════════════
 *  Test 1: actor_send_named local — register name, send by name
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int received;
} local_state_t;

static bool local_recv_behavior(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    local_state_t *s = state;
    if (msg->type == MSG_PING) {
        s->received++;
        runtime_stop(rt);
        return false;
    }
    return true;
}

static int test_send_named_local(void) {
    runtime_t *rt = runtime_init(1, 16);
    ASSERT_NOT_NULL(rt);

    local_state_t *state = calloc(1, sizeof(*state));
    ASSERT_NOT_NULL(state);

    actor_id_t id = actor_spawn(rt, local_recv_behavior, state, NULL, 16);
    ASSERT_NE(id, ACTOR_ID_INVALID);

    ASSERT(actor_register_name(rt, "local_svc", id));

    uint32_t payload = 42;
    ASSERT(actor_send_named(rt, "local_svc", MSG_PING, &payload, sizeof(payload)));

    runtime_run(rt);

    ASSERT_EQ(state->received, 1);

    free(state);
    runtime_destroy(rt);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 2: lookup unknown name returns ACTOR_ID_INVALID
 * ══════════════════════════════════════════════════════════════════════ */

static int test_lookup_unknown_name(void) {
    runtime_t *rt = runtime_init(1, 16);
    ASSERT_NOT_NULL(rt);

    ASSERT_EQ(actor_lookup(rt, "nonexistent"), ACTOR_ID_INVALID);
    ASSERT(!actor_send_named(rt, "nonexistent", MSG_PING, NULL, 0));

    runtime_destroy(rt);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Shared noop behavior for placeholder actors
 * ══════════════════════════════════════════════════════════════════════ */

static bool noop_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)rt; (void)self; (void)msg; (void)state;
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Test 3: register broadcast — node A registers, node B sees it
 *
 *  Node A: waits 100ms (timer) for child to connect, then registers
 *          "service", waits another 500ms, then stops.
 *  Node B: polls actor_lookup("service") via timers until found.
 * ══════════════════════════════════════════════════════════════════════ */

/* Node A: register-then-stop manager */
typedef struct {
    actor_id_t svc_id;
    int step;
} register_mgr_state_t;

static bool register_mgr_behavior(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state) {
    (void)self;
    register_mgr_state_t *s = state;

    if (msg->type == MSG_INIT) {
        /* Wait for child to connect */
        actor_set_timer(rt, 100, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            /* Register the service name — broadcasts to peers */
            actor_register_name(rt, "service", s->svc_id);
            /* Wait for propagation then stop */
            actor_set_timer(rt, 500, false);
            s->step = 2;
        } else if (s->step == 2) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: poll for name to appear */
typedef struct {
    bool found;
    actor_id_t resolved_id;
    int polls;
} lookup_wait_state_t;

static bool lookup_wait_behavior(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state) {
    (void)self;
    lookup_wait_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        actor_id_t id = actor_lookup(rt, "service");
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

static int run_node_b_lookup(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 16);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    transport_t *tp = transport_tcp_connect("127.0.0.1", TEST_PORT, NODE_A);
    if (!tp) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }
    if (!runtime_add_transport(rt, tp)) {
        tp->destroy(tp);
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }

    lookup_wait_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, lookup_wait_behavior, state, NULL, 64);
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

static int test_register_broadcast(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    transport_t *server_tp = transport_tcp_listen("127.0.0.1", TEST_PORT, NODE_B);
    ASSERT_NOT_NULL(server_tp);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        close(server_tp->fd);
        free(server_tp->impl);
        free(server_tp);
        int rc = run_node_b_lookup(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_t *rt = runtime_init(NODE_A, 16);
    ASSERT_NOT_NULL(rt);
    ASSERT(runtime_add_transport(rt, server_tp));

    /* Spawn the service actor */
    actor_id_t svc_id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    ASSERT_NE(svc_id, ACTOR_ID_INVALID);

    /* Spawn manager that will register the name after a delay */
    register_mgr_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->svc_id = svc_id;

    actor_id_t mgr_id = actor_spawn(rt, register_mgr_behavior, mgr_state, NULL, 16);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

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
 *  Test 4: send_named remote — node A registers "pong_service",
 *          node B does actor_send_named("pong_service", MSG_PING)
 * ══════════════════════════════════════════════════════════════════════ */

/* Node A: pong service — receives PING, replies PONG */
static bool pong_service_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self; (void)state;
    if (msg->type == MSG_PING) {
        uint32_t val = 1;
        actor_send(rt, msg->source, MSG_PONG, &val, sizeof(val));
    }
    return true;
}

/* Node A: register pong_service after delay, then wait for exchange to complete */
typedef struct {
    actor_id_t svc_id;
    int step;
} pong_mgr_state_t;

static bool pong_mgr_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    pong_mgr_state_t *s = state;

    if (msg->type == MSG_INIT) {
        actor_set_timer(rt, 100, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            actor_register_name(rt, "pong_service", s->svc_id);
            /* Wait for exchange then stop */
            actor_set_timer(rt, 2000, false);
            s->step = 2;
        } else if (s->step == 2) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: sends PING by name, receives PONG, stops */
typedef struct {
    int pong_count;
} pong_client_state_t;

static bool ping_client_behavior(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state) {
    (void)self;
    pong_client_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        if (actor_send_named(rt, "pong_service", MSG_PING, NULL, 0)) {
            /* Sent, wait for PONG */
        } else {
            /* Name not yet propagated, retry */
            actor_set_timer(rt, 10, false);
        }
    } else if (msg->type == MSG_PONG) {
        s->pong_count++;
        runtime_stop(rt);
        return false;
    }
    return true;
}

static int run_node_b_ping(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 16);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    transport_t *tp = transport_tcp_connect("127.0.0.1", TEST_PORT + 1, NODE_A);
    if (!tp) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }
    if (!runtime_add_transport(rt, tp)) {
        tp->destroy(tp);
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }

    pong_client_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, ping_client_behavior, state, NULL, 64);
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

static int test_send_named_remote(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    transport_t *server_tp = transport_tcp_listen("127.0.0.1", TEST_PORT + 1, NODE_B);
    ASSERT_NOT_NULL(server_tp);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        close(server_tp->fd);
        free(server_tp->impl);
        free(server_tp);
        int rc = run_node_b_ping(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_t *rt = runtime_init(NODE_A, 16);
    ASSERT_NOT_NULL(rt);
    ASSERT(runtime_add_transport(rt, server_tp));

    actor_id_t svc_id = actor_spawn(rt, pong_service_behavior, NULL, NULL, 64);
    ASSERT_NE(svc_id, ACTOR_ID_INVALID);

    /* Manager registers the name after delay */
    pong_mgr_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->svc_id = svc_id;

    actor_id_t mgr_id = actor_spawn(rt, pong_mgr_behavior, mgr_state, NULL, 16);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

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
 *  Test 5: deregister on death — actor dies, MSG_NAME_UNREGISTER
 *          broadcast, node B's lookup returns ACTOR_ID_INVALID
 * ══════════════════════════════════════════════════════════════════════ */

/* Node A: register named actor via timer, kill it after propagation */
typedef struct {
    actor_id_t mortal_id;
    int step;
} kill_manager_state_t;

static bool kill_manager_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    kill_manager_state_t *s = state;

    if (msg->type == MSG_INIT) {
        /* Wait for child to connect */
        actor_set_timer(rt, 100, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            /* Register mortal name */
            actor_register_name(rt, "mortal", s->mortal_id);
            /* Wait for broadcast to propagate */
            actor_set_timer(rt, 200, false);
            s->step = 2;
        } else if (s->step == 2) {
            /* Kill the mortal actor — triggers deregister broadcast */
            actor_stop(rt, s->mortal_id);
            /* Wait for cleanup propagation then stop */
            actor_set_timer(rt, 500, false);
            s->step = 3;
        } else if (s->step == 3) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: wait for "mortal" to appear, then wait for it to disappear */
typedef struct {
    bool appeared;
    bool disappeared;
    int polls;
} appear_disappear_state_t;

static bool appear_disappear_behavior(runtime_t *rt, actor_t *self,
                                       message_t *msg, void *state) {
    (void)self;
    appear_disappear_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        actor_id_t id = actor_lookup(rt, "mortal");
        s->polls++;

        if (!s->appeared) {
            if (id != ACTOR_ID_INVALID) {
                s->appeared = true;
                actor_set_timer(rt, 10, false);
            } else if (s->polls < 300) {
                actor_set_timer(rt, 10, false);
            } else {
                runtime_stop(rt);
                return false;
            }
        } else {
            if (id == ACTOR_ID_INVALID) {
                s->disappeared = true;
                runtime_stop(rt);
                return false;
            } else if (s->polls < 300) {
                actor_set_timer(rt, 10, false);
            } else {
                runtime_stop(rt);
                return false;
            }
        }
    }
    return true;
}

static int run_node_b_deregister(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 16);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    transport_t *tp = transport_tcp_connect("127.0.0.1", TEST_PORT + 2, NODE_A);
    if (!tp) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }
    if (!runtime_add_transport(rt, tp)) {
        tp->destroy(tp);
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }

    appear_disappear_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, appear_disappear_behavior, state, NULL, 64);
    if (my_id == ACTOR_ID_INVALID) {
        free(state); runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    actor_send(rt, my_id, MSG_INIT, NULL, 0);
    runtime_run(rt);

    char result = (state->appeared && state->disappeared) ? 'P' : 'F';
    write(result_fd, &result, 1);

    free(state);
    runtime_destroy(rt);
    return (result == 'P') ? 0 : 1;
}

static int test_deregister_on_death(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    transport_t *server_tp = transport_tcp_listen("127.0.0.1", TEST_PORT + 2, NODE_B);
    ASSERT_NOT_NULL(server_tp);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        close(server_tp->fd);
        free(server_tp->impl);
        free(server_tp);
        int rc = run_node_b_deregister(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_t *rt = runtime_init(NODE_A, 16);
    ASSERT_NOT_NULL(rt);
    ASSERT(runtime_add_transport(rt, server_tp));

    /* Spawn the mortal actor */
    actor_id_t mortal_id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    ASSERT_NE(mortal_id, ACTOR_ID_INVALID);

    /* Spawn manager to orchestrate register → kill sequence */
    kill_manager_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->mortal_id = mortal_id;

    actor_id_t mgr_id = actor_spawn(rt, kill_manager_behavior, mgr_state, NULL, 16);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

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
 *  Test 6: reregister after death — actor dies, new actor re-registers
 *          same name, node B sees updated actor_id
 * ══════════════════════════════════════════════════════════════════════ */

/* Node A manager: register → kill → re-register with new actor */
typedef struct {
    actor_id_t phoenix_id;
    int step;
} reregister_mgr_state_t;

static bool reregister_mgr_behavior(runtime_t *rt, actor_t *self,
                                     message_t *msg, void *state) {
    (void)self;
    reregister_mgr_state_t *s = state;

    if (msg->type == MSG_INIT) {
        actor_set_timer(rt, 100, false);
        s->step = 1;
    } else if (msg->type == MSG_TIMER) {
        if (s->step == 1) {
            /* Register initial phoenix */
            actor_register_name(rt, "phoenix", s->phoenix_id);
            actor_set_timer(rt, 200, false);
            s->step = 2;
        } else if (s->step == 2) {
            /* Kill the phoenix */
            actor_stop(rt, s->phoenix_id);
            /* Wait for cleanup (deregister broadcast) */
            actor_set_timer(rt, 200, false);
            s->step = 3;
        } else if (s->step == 3) {
            /* Spawn new actor, re-register same name */
            actor_id_t new_id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
            if (new_id != ACTOR_ID_INVALID) {
                actor_register_name(rt, "phoenix", new_id);
            }
            /* Wait for propagation then stop */
            actor_set_timer(rt, 500, false);
            s->step = 4;
        } else if (s->step == 4) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: wait for "phoenix" to appear, record id, wait for different id */
typedef struct {
    actor_id_t first_id;
    actor_id_t second_id;
    bool got_first;
    bool got_second;
    int polls;
} reregister_state_t;

static bool reregister_watch_behavior(runtime_t *rt, actor_t *self,
                                       message_t *msg, void *state) {
    (void)self;
    reregister_state_t *s = state;

    if (msg->type == MSG_INIT || msg->type == MSG_TIMER) {
        actor_id_t id = actor_lookup(rt, "phoenix");
        s->polls++;

        if (!s->got_first) {
            if (id != ACTOR_ID_INVALID) {
                s->first_id = id;
                s->got_first = true;
                actor_set_timer(rt, 10, false);
            } else if (s->polls < 400) {
                actor_set_timer(rt, 10, false);
            } else {
                runtime_stop(rt);
                return false;
            }
        } else if (!s->got_second) {
            if (id != ACTOR_ID_INVALID && id != s->first_id) {
                s->second_id = id;
                s->got_second = true;
                runtime_stop(rt);
                return false;
            } else if (s->polls < 400) {
                actor_set_timer(rt, 10, false);
            } else {
                runtime_stop(rt);
                return false;
            }
        }
    }
    return true;
}

static int run_node_b_reregister(int result_fd) {
    alarm(5);
    usleep(50000);

    runtime_t *rt = runtime_init(NODE_B, 16);
    if (!rt) { write(result_fd, "F", 1); return 1; }

    transport_t *tp = transport_tcp_connect("127.0.0.1", TEST_PORT + 3, NODE_A);
    if (!tp) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }
    if (!runtime_add_transport(rt, tp)) {
        tp->destroy(tp);
        runtime_destroy(rt);
        write(result_fd, "F", 1);
        return 1;
    }

    reregister_state_t *state = calloc(1, sizeof(*state));
    if (!state) { runtime_destroy(rt); write(result_fd, "F", 1); return 1; }

    actor_id_t my_id = actor_spawn(rt, reregister_watch_behavior, state, NULL, 64);
    if (my_id == ACTOR_ID_INVALID) {
        free(state); runtime_destroy(rt); write(result_fd, "F", 1); return 1;
    }

    actor_send(rt, my_id, MSG_INIT, NULL, 0);
    runtime_run(rt);

    char result = (state->got_first && state->got_second &&
                   state->first_id != state->second_id) ? 'P' : 'F';
    write(result_fd, &result, 1);

    free(state);
    runtime_destroy(rt);
    return (result == 'P') ? 0 : 1;
}

static int test_reregister_after_death(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    transport_t *server_tp = transport_tcp_listen("127.0.0.1", TEST_PORT + 3, NODE_B);
    ASSERT_NOT_NULL(server_tp);

    pid_t child = fork();
    ASSERT_NE(child, -1);

    if (child == 0) {
        close(pipefd[0]);
        close(server_tp->fd);
        free(server_tp->impl);
        free(server_tp);
        int rc = run_node_b_reregister(pipefd[1]);
        close(pipefd[1]);
        _exit(rc);
    }

    close(pipefd[1]);
    alarm(5);

    runtime_t *rt = runtime_init(NODE_A, 16);
    ASSERT_NOT_NULL(rt);
    ASSERT(runtime_add_transport(rt, server_tp));

    /* Spawn first phoenix */
    actor_id_t phoenix_id = actor_spawn(rt, noop_behavior, NULL, NULL, 16);
    ASSERT_NE(phoenix_id, ACTOR_ID_INVALID);

    /* Manager orchestrates register → kill → re-register */
    reregister_mgr_state_t *mgr_state = calloc(1, sizeof(*mgr_state));
    ASSERT_NOT_NULL(mgr_state);
    mgr_state->phoenix_id = phoenix_id;

    actor_id_t mgr_id = actor_spawn(rt, reregister_mgr_behavior, mgr_state, NULL, 16);
    ASSERT_NE(mgr_id, ACTOR_ID_INVALID);
    actor_send(rt, mgr_id, MSG_INIT, NULL, 0);

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

/* ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_distributed_registry:\n");
    RUN_TEST(test_send_named_local);
    RUN_TEST(test_lookup_unknown_name);
    RUN_TEST(test_register_broadcast);
    RUN_TEST(test_send_named_remote);
    RUN_TEST(test_deregister_on_death);
    RUN_TEST(test_reregister_after_death);
    TEST_REPORT();
}
