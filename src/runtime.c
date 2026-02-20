#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/mailbox.h"
#include "microkernel/message.h"
#include "microkernel/scheduler.h"
#include "microkernel/transport.h"
#include "microkernel/supervision.h"
#include "runtime_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/poll.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef MAX_TRANSPORTS
#define MAX_TRANSPORTS 16
#endif
#ifndef MAX_TIMERS
#define MAX_TIMERS      32
#endif
#ifndef MAX_FD_WATCHES
#define MAX_FD_WATCHES  32
#endif
#define MAX_POLL_FDS    (MAX_TRANSPORTS + MAX_TIMERS + MAX_FD_WATCHES + MAX_HTTP_CONNS + MAX_HTTP_LISTENERS)

/* ── Internal types ────────────────────────────────────────────────── */

typedef enum {
    POLL_SOURCE_TRANSPORT,
    POLL_SOURCE_TIMER,
    POLL_SOURCE_FD_WATCH,
    POLL_SOURCE_HTTP,
    POLL_SOURCE_HTTP_LISTEN
} poll_source_type_t;

typedef struct {
    poll_source_type_t type;
    size_t idx;
} poll_source_t;

/* timer_entry_t and name_entry_t are in runtime_internal.h */

typedef struct {
    int         fd;       /* -1 = unused */
    uint32_t    events;   /* POLLIN | POLLOUT */
    actor_id_t  owner;
} fd_watch_entry_t;

#ifndef NAME_REGISTRY_SIZE
#define NAME_REGISTRY_SIZE 128
#endif
#define NAME_MAX_LEN 64

struct runtime {
    node_id_t    node_id;
    actor_t    **actors;         /* flat array indexed by local sequence */
    size_t       max_actors;
    uint32_t     next_actor_seq; /* monotonic, starts at 1 (0 = invalid) */
    size_t       actor_count;
    scheduler_t  scheduler;      /* embedded by value */
    actor_t     *current_actor;  /* set during behavior dispatch */
    bool         running;
    /* Phase 2: transport table (sparse array indexed by node_id) */
    transport_t *transports[MAX_TRANSPORTS];
    size_t       transport_count;
    /* Phase 2.5: timers */
    timer_entry_t    timers[MAX_TIMERS];
    uint32_t         next_timer_id;       /* monotonic, starts at 1 */
    /* Phase 2.5: FD watches */
    fd_watch_entry_t fd_watches[MAX_FD_WATCHES];
    /* Phase 2.5: name registry */
    name_entry_t     name_registry[NAME_REGISTRY_SIZE];
    /* Phase 2.5: logging */
    actor_id_t       log_actor_id;        /* ACTOR_ID_INVALID until enabled */
    int              min_log_level;
    /* Phase 3.5: HTTP connections */
    http_conn_t      http_conns[MAX_HTTP_CONNS];
    uint32_t         next_http_conn_id;   /* monotonic, starts at 1 */
    /* Phase 5: HTTP listeners */
    http_listener_t  http_listeners[MAX_HTTP_LISTENERS];
};

/* ── Initialization / teardown ──────────────────────────────────────── */

runtime_t *runtime_init(node_id_t node_id, size_t max_actors) {
    runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return NULL;

    rt->actors = calloc(max_actors, sizeof(actor_t *));
    if (!rt->actors) {
        free(rt);
        return NULL;
    }

    rt->node_id = node_id;
    rt->max_actors = max_actors;
    rt->next_actor_seq = 1;
    scheduler_init(&rt->scheduler);

    /* Phase 2.5: initialize service state */
    rt->next_timer_id = 1;
    for (size_t i = 0; i < MAX_FD_WATCHES; i++) {
        rt->fd_watches[i].fd = -1;
    }
    rt->log_actor_id = ACTOR_ID_INVALID;
    rt->min_log_level = LOG_INFO;

    /* Phase 3.5: HTTP connections */
    rt->next_http_conn_id = 1;

    /* Phase 5: HTTP listeners */
    for (size_t i = 0; i < MAX_HTTP_LISTENERS; i++) {
        rt->http_listeners[i].listen_fd = -1;
    }

    return rt;
}

/* Forward declaration for HTTP cleanup */
static void http_conn_free(http_conn_t *conn);

void runtime_destroy(runtime_t *rt) {
    if (!rt) return;
    for (size_t i = 0; i < rt->max_actors; i++) {
        if (rt->actors[i]) {
            actor_destroy(rt->actors[i]);
        }
    }
    free(rt->actors);
    for (size_t i = 0; i < MAX_TRANSPORTS; i++) {
        if (rt->transports[i]) {
            rt->transports[i]->destroy(rt->transports[i]);
        }
    }
    /* Close any active timerfds */
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (rt->timers[i].id != TIMER_ID_INVALID) {
            timer_platform_close(i, rt->timers[i].fd);
        }
    }
    /* Clean up HTTP connections */
    for (size_t i = 0; i < MAX_HTTP_CONNS; i++) {
        if (rt->http_conns[i].id != HTTP_CONN_ID_INVALID) {
            http_conn_free(&rt->http_conns[i]);
        }
    }
    /* Clean up HTTP listeners */
    for (size_t i = 0; i < MAX_HTTP_LISTENERS; i++) {
        if (rt->http_listeners[i].listen_fd >= 0) {
            close(rt->http_listeners[i].listen_fd);
            rt->http_listeners[i].listen_fd = -1;
        }
    }
    free(rt);
}

/* ── Actor lifecycle ────────────────────────────────────────────────── */

actor_id_t actor_spawn(runtime_t *rt, actor_behavior_fn behavior,
                       void *initial_state, void (*free_state)(void *),
                       size_t mailbox_size) {
    if (rt->actor_count >= rt->max_actors) return ACTOR_ID_INVALID;

    uint32_t seq = rt->next_actor_seq++;
    actor_id_t id = actor_id_make(rt->node_id, seq);

    actor_t *a = actor_create(id, rt->node_id, behavior,
                              initial_state, free_state, mailbox_size);
    if (!a) return ACTOR_ID_INVALID;

    /* Store at index = seq (slot 0 is unused since seq starts at 1) */
    if (seq >= rt->max_actors) {
        actor_destroy(a);
        return ACTOR_ID_INVALID;
    }
    rt->actors[seq] = a;
    rt->actor_count++;
    return id;
}

void actor_stop(runtime_t *rt, actor_id_t id) {
    uint32_t seq = actor_id_seq(id);
    if (seq == 0 || seq >= rt->max_actors) return;
    actor_t *a = rt->actors[seq];
    if (a) {
        a->exit_reason = EXIT_KILLED;
        a->status = ACTOR_STOPPED;
    }
}

/* ── Internal: look up a local actor by id ─────────────────────────── */

static actor_t *lookup(runtime_t *rt, actor_id_t id) {
    uint32_t seq = actor_id_seq(id);
    if (seq == 0 || seq >= rt->max_actors) return NULL;
    actor_t *a = rt->actors[seq];
    if (!a || a->status == ACTOR_STOPPED) return NULL;
    return a;
}

/* ── Internal: deliver a message to a local actor ──────────────────── */

static bool deliver_local(runtime_t *rt, actor_id_t dest, message_t *msg) {
    actor_t *target = lookup(rt, dest);
    if (!target) return false;

    if (!mailbox_enqueue(target->mailbox, msg)) return false;

    if (target->status == ACTOR_IDLE) {
        scheduler_enqueue(&rt->scheduler, target);
    }
    return true;
}

/* Public wrapper for deliver_local (used by http_conn.c) */
bool runtime_deliver_msg(runtime_t *rt, actor_id_t dest, msg_type_t type,
                         const void *payload, size_t payload_size) {
    message_t *msg = message_create(ACTOR_ID_INVALID, dest, type,
                                    payload, payload_size);
    if (!msg) return false;
    if (!deliver_local(rt, dest, msg)) {
        message_destroy(msg);
        return false;
    }
    return true;
}

/* ── Messaging ──────────────────────────────────────────────────────── */

bool actor_send(runtime_t *rt, actor_id_t dest, msg_type_t type,
                const void *payload, size_t payload_size) {
    actor_id_t source = rt->current_actor ? rt->current_actor->id
                                          : ACTOR_ID_INVALID;
    node_id_t dest_node = actor_id_node(dest);

    if (dest_node == rt->node_id) {
        /* Local delivery */
        actor_t *target = lookup(rt, dest);
        if (!target) return false;

        message_t *msg = message_create(source, dest, type,
                                        payload, payload_size);
        if (!msg) return false;

        if (!mailbox_enqueue(target->mailbox, msg)) {
            message_destroy(msg);
            return false;
        }

        if (target->status == ACTOR_IDLE) {
            scheduler_enqueue(&rt->scheduler, target);
        }
        return true;
    }

    /* Remote delivery via transport */
    if (dest_node >= MAX_TRANSPORTS || !rt->transports[dest_node])
        return false;

    transport_t *tp = rt->transports[dest_node];
    message_t *msg = message_create(source, dest, type,
                                    payload, payload_size);
    if (!msg) return false;

    bool ok = tp->send(tp, msg);
    message_destroy(msg);
    return ok;
}

/* ── Transport ─────────────────────────────────────────────────────── */

bool runtime_add_transport(runtime_t *rt, transport_t *transport) {
    if (!rt || !transport) return false;
    if (transport->peer_node >= MAX_TRANSPORTS) return false;
    rt->transports[transport->peer_node] = transport;
    rt->transport_count++;
    return true;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

actor_id_t actor_self(runtime_t *rt) {
    return rt->current_actor ? rt->current_actor->id : ACTOR_ID_INVALID;
}

void *actor_state(runtime_t *rt) {
    return rt->current_actor ? rt->current_actor->state : NULL;
}

/* ── HTTP connection cleanup ───────────────────────────────────────── */

static void http_conn_free(http_conn_t *conn) {
    if (conn->sock) {
        conn->sock->close(conn->sock);
        conn->sock = NULL;
    }
    free(conn->send_buf);
    free(conn->headers_buf);
    free(conn->body_buf);
    free(conn->sse_data);
    free(conn->request_method);
    free(conn->request_path);
    memset(conn, 0, sizeof(*conn));
}

/* ── Execution ──────────────────────────────────────────────────────── */

/* Forward declarations for service cleanup */
void name_registry_deregister_actor(runtime_t *rt, actor_id_t id);

static void cleanup_stopped(runtime_t *rt) {
    for (size_t i = 1; i < rt->max_actors; i++) {
        actor_t *a = rt->actors[i];
        if (a && a->status == ACTOR_STOPPED) {
            actor_id_t id = a->id;
            /* Notify parent of child death */
            if (a->parent != ACTOR_ID_INVALID) {
                child_exit_payload_t exit_payload = {
                    .child_id = id,
                    .exit_reason = a->exit_reason
                };
                runtime_deliver_msg(rt, a->parent, MSG_CHILD_EXIT,
                                    &exit_payload, sizeof(exit_payload));
            }
            /* Clean up timers owned by this actor */
            for (size_t t = 0; t < MAX_TIMERS; t++) {
                if (rt->timers[t].id != TIMER_ID_INVALID &&
                    rt->timers[t].owner == id) {
                    timer_platform_close(t, rt->timers[t].fd);
                    memset(&rt->timers[t], 0, sizeof(timer_entry_t));
                }
            }
            /* Clean up FD watches owned by this actor */
            for (size_t w = 0; w < MAX_FD_WATCHES; w++) {
                if (rt->fd_watches[w].fd >= 0 &&
                    rt->fd_watches[w].owner == id) {
                    rt->fd_watches[w].fd = -1;
                    rt->fd_watches[w].events = 0;
                    rt->fd_watches[w].owner = ACTOR_ID_INVALID;
                }
            }
            /* Clean up HTTP connections owned by this actor */
            for (size_t h = 0; h < MAX_HTTP_CONNS; h++) {
                if (rt->http_conns[h].id != HTTP_CONN_ID_INVALID &&
                    rt->http_conns[h].owner == id) {
                    http_conn_free(&rt->http_conns[h]);
                }
            }
            /* Clean up HTTP listeners owned by this actor */
            for (size_t l = 0; l < MAX_HTTP_LISTENERS; l++) {
                if (rt->http_listeners[l].listen_fd >= 0 &&
                    rt->http_listeners[l].owner == id) {
                    close(rt->http_listeners[l].listen_fd);
                    rt->http_listeners[l].listen_fd = -1;
                }
            }
            /* Clean up name registry entries */
            name_registry_deregister_actor(rt, id);

            actor_destroy(a);
            rt->actors[i] = NULL;
            rt->actor_count--;
        }
    }
}

void runtime_step(runtime_t *rt) {
    actor_t *actor = scheduler_dequeue(&rt->scheduler);
    if (!actor) {
        cleanup_stopped(rt);
        return;
    }

    if (actor->status == ACTOR_STOPPED) return;

    actor->status = ACTOR_RUNNING;
    rt->current_actor = actor;

    /* Process one message per turn for fairness */
    message_t *msg = mailbox_dequeue(actor->mailbox);
    if (msg) {
        bool keep = actor->behavior(rt, actor, msg, actor->state);
        message_destroy(msg);
        if (!keep) {
            actor->exit_reason = EXIT_NORMAL;
            actor->status = ACTOR_STOPPED;
        }
    }

    rt->current_actor = NULL;

    /* Re-enqueue if still alive and has more messages */
    if (actor->status == ACTOR_RUNNING) {
        if (!mailbox_is_empty(actor->mailbox)) {
            actor->status = ACTOR_IDLE; /* reset before enqueue sets READY */
            scheduler_enqueue(&rt->scheduler, actor);
        } else {
            actor->status = ACTOR_IDLE;
        }
    }

    cleanup_stopped(rt);
}

/* ── Internal: count active IO sources ─────────────────────────────── */

static size_t count_active_timers(runtime_t *rt) {
    size_t n = 0;
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (rt->timers[i].id != TIMER_ID_INVALID) n++;
    }
    return n;
}

static size_t count_active_watches(runtime_t *rt) {
    size_t n = 0;
    for (size_t i = 0; i < MAX_FD_WATCHES; i++) {
        if (rt->fd_watches[i].fd >= 0) n++;
    }
    return n;
}

static size_t count_active_listeners(runtime_t *rt) {
    size_t n = 0;
    for (size_t i = 0; i < MAX_HTTP_LISTENERS; i++) {
        if (rt->http_listeners[i].listen_fd >= 0) n++;
    }
    return n;
}

static size_t count_active_http_conns(runtime_t *rt) {
    size_t n = 0;
    for (size_t i = 0; i < MAX_HTTP_CONNS; i++) {
        if (rt->http_conns[i].id != HTTP_CONN_ID_INVALID &&
            rt->http_conns[i].state != HTTP_STATE_IDLE &&
            rt->http_conns[i].state != HTTP_STATE_DONE &&
            rt->http_conns[i].state != HTTP_STATE_ERROR) {
            n++;
        }
    }
    return n;
}

/* ── Unified poll and dispatch ─────────────────────────────────────── */

static bool poll_and_dispatch(runtime_t *rt, int timeout_ms) {
    struct pollfd  fds[MAX_POLL_FDS];
    poll_source_t  sources[MAX_POLL_FDS];
    nfds_t nfds = 0;

    /* Add transport FDs */
    for (size_t i = 0; i < MAX_TRANSPORTS; i++) {
        transport_t *tp = rt->transports[i];
        if (!tp || tp->fd < 0) continue;
        fds[nfds].fd = tp->fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        sources[nfds].type = POLL_SOURCE_TRANSPORT;
        sources[nfds].idx = i;
        nfds++;
    }

    /* Add timer FDs */
    for (size_t i = 0; i < MAX_TIMERS; i++) {
        if (rt->timers[i].id == TIMER_ID_INVALID) continue;
        fds[nfds].fd = rt->timers[i].fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        sources[nfds].type = POLL_SOURCE_TIMER;
        sources[nfds].idx = i;
        nfds++;
    }

    /* Add FD watch entries */
    for (size_t i = 0; i < MAX_FD_WATCHES; i++) {
        if (rt->fd_watches[i].fd < 0) continue;
        fds[nfds].fd = rt->fd_watches[i].fd;
        fds[nfds].events = (short)rt->fd_watches[i].events;
        fds[nfds].revents = 0;
        sources[nfds].type = POLL_SOURCE_FD_WATCH;
        sources[nfds].idx = i;
        nfds++;
    }

    /* Add HTTP connection FDs */
    for (size_t i = 0; i < MAX_HTTP_CONNS; i++) {
        http_conn_t *hc = &rt->http_conns[i];
        if (hc->id == HTTP_CONN_ID_INVALID || !hc->sock) continue;
        if (hc->state == HTTP_STATE_IDLE || hc->state == HTTP_STATE_DONE ||
            hc->state == HTTP_STATE_ERROR) continue;

        int sock_fd = hc->sock->get_fd(hc->sock);
        fds[nfds].fd = sock_fd;
        short events;
        if (hc->state == HTTP_STATE_SENDING ||
            hc->state == HTTP_STATE_SRV_SENDING) {
            events = POLLOUT;
        } else {
            events = POLLIN;
        }
        fds[nfds].events = events;
        fds[nfds].revents = 0;
        sources[nfds].type = POLL_SOURCE_HTTP;
        sources[nfds].idx = i;
        nfds++;
    }

    /* Add HTTP listener FDs */
    for (size_t i = 0; i < MAX_HTTP_LISTENERS; i++) {
        if (rt->http_listeners[i].listen_fd < 0) continue;
        fds[nfds].fd = rt->http_listeners[i].listen_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        sources[nfds].type = POLL_SOURCE_HTTP_LISTEN;
        sources[nfds].idx = i;
        nfds++;
    }

    if (nfds == 0) return false;

    int ret = poll(fds, nfds, timeout_ms);
    if (ret <= 0) return false;

    bool dispatched = false;

    for (nfds_t n = 0; n < nfds; n++) {
        if (fds[n].revents == 0) continue;

        switch (sources[n].type) {
        case POLL_SOURCE_TRANSPORT: {
            transport_t *tp = rt->transports[sources[n].idx];
            if (!tp) break;
            message_t *msg;
            while ((msg = tp->recv(tp)) != NULL) {
                if (!deliver_local(rt, msg->dest, msg)) {
                    message_destroy(msg);
                }
                dispatched = true;
            }
            break;
        }
        case POLL_SOURCE_TIMER: {
            size_t idx = sources[n].idx;
            timer_entry_t *te = &rt->timers[idx];
            if (te->id == TIMER_ID_INVALID) break;
            uint64_t expirations = 0;
            ssize_t r = read(te->fd, &expirations, sizeof(expirations));
            if (r != (ssize_t)sizeof(expirations)) break;

            timer_payload_t payload = {
                .id = te->id,
                .expirations = expirations
            };
            message_t *msg = message_create(
                ACTOR_ID_INVALID, te->owner, MSG_TIMER,
                &payload, sizeof(payload));
            if (msg) {
                if (!deliver_local(rt, te->owner, msg)) {
                    message_destroy(msg);
                }
                dispatched = true;
            }
            /* One-shot: auto-clean after fire */
            if (!te->periodic) {
                timer_platform_close(te - rt->timers, te->fd);
                memset(te, 0, sizeof(timer_entry_t));
            }
            break;
        }
        case POLL_SOURCE_FD_WATCH: {
            size_t idx = sources[n].idx;
            fd_watch_entry_t *we = &rt->fd_watches[idx];
            if (we->fd < 0) break;

            fd_event_payload_t payload = {
                .fd = we->fd,
                .events = (uint32_t)fds[n].revents
            };
            message_t *msg = message_create(
                ACTOR_ID_INVALID, we->owner, MSG_FD_EVENT,
                &payload, sizeof(payload));
            if (msg) {
                if (!deliver_local(rt, we->owner, msg)) {
                    message_destroy(msg);
                }
                dispatched = true;
            }
            break;
        }
        case POLL_SOURCE_HTTP: {
            http_conn_t *hc = &rt->http_conns[sources[n].idx];
            if (hc->id == HTTP_CONN_ID_INVALID) break;
            http_conn_drive(hc, fds[n].revents, rt);
            dispatched = true;
            break;
        }
        case POLL_SOURCE_HTTP_LISTEN: {
            http_listener_t *lis = &rt->http_listeners[sources[n].idx];
            if (lis->listen_fd < 0) break;

            int client_fd = accept(lis->listen_fd, NULL, NULL);
            if (client_fd < 0) break;

            /* Set non-blocking */
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags >= 0) fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            mk_socket_t *sock = mk_socket_tcp_wrap(client_fd);
            if (!sock) { close(client_fd); break; }

            /* Allocate connection from shared pool */
            http_conn_t *hc = NULL;
            for (size_t ci = 0; ci < MAX_HTTP_CONNS; ci++) {
                if (rt->http_conns[ci].id == HTTP_CONN_ID_INVALID) {
                    hc = &rt->http_conns[ci];
                    break;
                }
            }
            if (!hc) {
                sock->close(sock);
                break;
            }

            memset(hc, 0, sizeof(*hc));
            hc->id = rt->next_http_conn_id++;
            hc->state = HTTP_STATE_SRV_RECV_REQUEST;
            hc->conn_type = HTTP_CONN_SERVER;
            hc->owner = lis->owner;
            hc->sock = sock;
            hc->is_server = true;
            hc->content_length = -1;

            dispatched = true;
            break;
        }
        }
    }

    return dispatched;
}

void runtime_run(runtime_t *rt) {
    rt->running = true;

    while (rt->running) {
        /* Drain the scheduler */
        while (rt->running && !scheduler_is_empty(&rt->scheduler)) {
            runtime_step(rt);
        }

        if (!rt->running) break;

        bool has_io = (rt->transport_count > 0) ||
                      (count_active_timers(rt) > 0) ||
                      (count_active_watches(rt) > 0) ||
                      (count_active_http_conns(rt) > 0) ||
                      (count_active_listeners(rt) > 0);

        if (has_io) {
            /* Non-blocking poll for events */
            bool received = poll_and_dispatch(rt, 0);

            if (!received && scheduler_is_empty(&rt->scheduler)) {
                if (rt->actor_count == 0) break;
                /* Block with timeout, then poll again */
                poll_and_dispatch(rt, 100);
            }
        } else {
            /* No IO sources -> exit when scheduler empty */
            if (scheduler_is_empty(&rt->scheduler) || rt->actor_count == 0) {
                break;
            }
        }
    }
    rt->running = false;
}

void runtime_stop(runtime_t *rt) {
    rt->running = false;
}

/* ── FD watcher service ────────────────────────────────────────────── */

bool actor_watch_fd(runtime_t *rt, int fd, uint32_t events) {
    if (!rt->current_actor) return false;
    actor_id_t owner = rt->current_actor->id;

    /* Check for existing watch on same fd by same owner, update it */
    for (size_t i = 0; i < MAX_FD_WATCHES; i++) {
        if (rt->fd_watches[i].fd == fd &&
            rt->fd_watches[i].owner == owner) {
            rt->fd_watches[i].events = events;
            return true;
        }
    }

    /* Find free slot */
    for (size_t i = 0; i < MAX_FD_WATCHES; i++) {
        if (rt->fd_watches[i].fd < 0) {
            rt->fd_watches[i].fd = fd;
            rt->fd_watches[i].events = events;
            rt->fd_watches[i].owner = owner;
            return true;
        }
    }
    return false; /* no free slots */
}

bool actor_unwatch_fd(runtime_t *rt, int fd) {
    if (!rt->current_actor) return false;
    actor_id_t owner = rt->current_actor->id;

    for (size_t i = 0; i < MAX_FD_WATCHES; i++) {
        if (rt->fd_watches[i].fd == fd &&
            rt->fd_watches[i].owner == owner) {
            rt->fd_watches[i].fd = -1;
            rt->fd_watches[i].events = 0;
            rt->fd_watches[i].owner = ACTOR_ID_INVALID;
            return true;
        }
    }
    return false;
}

/* ── Timer accessors (used by timer_linux.c) ───────────────────────── */

timer_entry_t *runtime_get_timers(runtime_t *rt) {
    return rt->timers;
}

size_t runtime_get_max_timers(void) {
    return MAX_TIMERS;
}

uint32_t runtime_alloc_timer_id(runtime_t *rt) {
    return rt->next_timer_id++;
}

actor_id_t runtime_current_actor_id(runtime_t *rt) {
    return rt->current_actor ? rt->current_actor->id : ACTOR_ID_INVALID;
}

/* ── Logging accessors (used by log_actor.c) ───────────────────────── */

actor_id_t runtime_get_log_actor(runtime_t *rt) {
    return rt->log_actor_id;
}

void runtime_set_log_actor(runtime_t *rt, actor_id_t id) {
    rt->log_actor_id = id;
}

int runtime_get_min_log_level(runtime_t *rt) {
    return rt->min_log_level;
}

void runtime_set_log_level(runtime_t *rt, int level) {
    rt->min_log_level = level;
}

/* ── Name registry accessors (used by name_registry.c) ─────────────── */

name_entry_t *runtime_get_name_registry(runtime_t *rt) {
    return rt->name_registry;
}

size_t runtime_get_name_registry_size(void) {
    return NAME_REGISTRY_SIZE;
}

/* ── HTTP connection accessors (used by http_conn.c) ───────────────── */

http_conn_t *runtime_get_http_conns(runtime_t *rt) {
    return rt->http_conns;
}

size_t runtime_get_max_http_conns(void) {
    return MAX_HTTP_CONNS;
}

uint32_t runtime_alloc_http_conn_id(runtime_t *rt) {
    return rt->next_http_conn_id++;
}

/* ── HTTP listener accessors (used by http_conn.c) ─────────────────── */

http_listener_t *runtime_get_http_listeners(runtime_t *rt) {
    return rt->http_listeners;
}

/* ── Supervision accessors ─────────────────────────────────────────── */

void runtime_set_actor_parent(runtime_t *rt, actor_id_t child_id,
                               actor_id_t parent_id) {
    uint32_t seq = actor_id_seq(child_id);
    if (seq == 0 || seq >= rt->max_actors) return;
    actor_t *a = rt->actors[seq];
    if (a) a->parent = parent_id;
}

void *runtime_get_actor_state(runtime_t *rt, actor_id_t id) {
    uint32_t seq = actor_id_seq(id);
    if (seq == 0 || seq >= rt->max_actors) return NULL;
    actor_t *a = rt->actors[seq];
    return a ? a->state : NULL;
}
