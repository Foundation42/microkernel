#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include <unistd.h>
#include <poll.h>

/* ── Test state ────────────────────────────────────────────────────── */

typedef struct {
    int  pipe_read_fd;
    int  pipe_write_fd;
    int  event_count;
    int  last_fd;
    bool should_unwatch;
} fd_watch_state_t;

/* ── Behaviors ─────────────────────────────────────────────────────── */

static bool watch_pipe_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    fd_watch_state_t *s = (fd_watch_state_t *)state;

    if (msg->type == 1) {
        /* Setup: watch the read end of a pipe */
        actor_watch_fd(rt, s->pipe_read_fd, POLLIN);
        /* Write data to make it readable */
        (void)write(s->pipe_write_fd, "x", 1);
        return true;
    }
    if (msg->type == MSG_FD_EVENT) {
        const fd_event_payload_t *ev = (const fd_event_payload_t *)msg->payload;
        s->last_fd = ev->fd;
        s->event_count++;
        /* Drain the byte */
        char buf;
        (void)read(s->pipe_read_fd, &buf, 1);
        runtime_stop(rt);
        return true;
    }
    return true;
}

static bool unwatch_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    fd_watch_state_t *s = (fd_watch_state_t *)state;

    if (msg->type == 1) {
        actor_watch_fd(rt, s->pipe_read_fd, POLLIN);
        /* Immediately unwatch */
        actor_unwatch_fd(rt, s->pipe_read_fd);
        /* Write data (should NOT trigger event since unwatched) */
        (void)write(s->pipe_write_fd, "x", 1);
        /* Set a short timer to verify no FD event arrives */
        actor_set_timer(rt, 30, false);
        return true;
    }
    if (msg->type == MSG_FD_EVENT) {
        s->event_count++;
        return true;
    }
    if (msg->type == MSG_TIMER) {
        /* Timer fired, meaning we waited and no FD event came */
        runtime_stop(rt);
        return true;
    }
    return true;
}

static bool watch_stop_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    fd_watch_state_t *s = (fd_watch_state_t *)state;

    if (msg->type == 1) {
        actor_watch_fd(rt, s->pipe_read_fd, POLLIN);
        return false; /* stop - watch should be cleaned up */
    }
    return true;
}

/* ── Multi-actor test: two actors watching different FDs ────────────── */

typedef struct {
    int pipe_fds[2]; /* read, write */
    int event_count;
} multi_actor_state_t;

static bool multi_watch_behavior(runtime_t *rt, actor_t *self,
                                 message_t *msg, void *state) {
    (void)self;
    multi_actor_state_t *s = (multi_actor_state_t *)state;

    if (msg->type == 1) {
        actor_watch_fd(rt, s->pipe_fds[0], POLLIN);
        return true;
    }
    if (msg->type == MSG_FD_EVENT) {
        s->event_count++;
        char buf;
        (void)read(s->pipe_fds[0], &buf, 1);
        runtime_stop(rt);
        return true;
    }
    return true;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int test_pipe_readable(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    runtime_t *rt = runtime_init(0, 64);
    fd_watch_state_t s = {
        .pipe_read_fd = pipefd[0],
        .pipe_write_fd = pipefd[1],
    };
    actor_id_t id = actor_spawn(rt, watch_pipe_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(s.event_count, 1);
    ASSERT_EQ(s.last_fd, pipefd[0]);

    runtime_destroy(rt);
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
}

static int test_unwatch_stops_events(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    runtime_t *rt = runtime_init(0, 64);
    fd_watch_state_t s = {
        .pipe_read_fd = pipefd[0],
        .pipe_write_fd = pipefd[1],
    };
    actor_id_t id = actor_spawn(rt, unwatch_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);

    ASSERT_EQ(s.event_count, 0);

    runtime_destroy(rt);
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
}

static int test_cleanup_on_stop(void) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    runtime_t *rt = runtime_init(0, 64);
    fd_watch_state_t s = {
        .pipe_read_fd = pipefd[0],
        .pipe_write_fd = pipefd[1],
    };
    actor_id_t id = actor_spawn(rt, watch_stop_behavior, &s, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);
    runtime_run(rt);
    /* No crash, no leaked watches */

    runtime_destroy(rt);
    close(pipefd[0]);
    close(pipefd[1]);
    return 0;
}

static int test_multi_actor_watch(void) {
    int pipe1[2], pipe2[2];
    ASSERT_EQ(pipe(pipe1), 0);
    ASSERT_EQ(pipe(pipe2), 0);

    runtime_t *rt = runtime_init(0, 64);
    multi_actor_state_t s1 = {.pipe_fds = {pipe1[0], pipe1[1]}};
    multi_actor_state_t s2 = {.pipe_fds = {pipe2[0], pipe2[1]}};

    actor_id_t a1 = actor_spawn(rt, multi_watch_behavior, &s1, NULL, 16);
    /* s2 won't fire, just registers */
    actor_id_t a2 = actor_spawn(rt, multi_watch_behavior, &s2, NULL, 16);

    /* Tell both to watch */
    actor_send(rt, a1, 1, NULL, 0);
    actor_send(rt, a2, 1, NULL, 0);

    /* Only write to pipe1 */
    (void)write(pipe1[1], "x", 1);

    runtime_run(rt);

    ASSERT_EQ(s1.event_count, 1);
    ASSERT_EQ(s2.event_count, 0);

    runtime_destroy(rt);
    close(pipe1[0]); close(pipe1[1]);
    close(pipe2[0]); close(pipe2[1]);
    return 0;
}

int main(void) {
    printf("test_fd_watcher:\n");
    RUN_TEST(test_pipe_readable);
    RUN_TEST(test_unwatch_stops_events);
    RUN_TEST(test_cleanup_on_stop);
    RUN_TEST(test_multi_actor_watch);
    TEST_REPORT();
}
