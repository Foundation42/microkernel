#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/mailbox.h"
#include "microkernel/message.h"
#include "microkernel/scheduler.h"
#include "microkernel/transport.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#define MAX_TRANSPORTS 16
#define MAX_TIMERS      32
#define MAX_FD_WATCHES  32
#define MAX_POLL_FDS    (MAX_TRANSPORTS + MAX_TIMERS + MAX_FD_WATCHES)

/* ── Internal types ────────────────────────────────────────────────── */

typedef enum {
    POLL_SOURCE_TRANSPORT,
    POLL_SOURCE_TIMER,
    POLL_SOURCE_FD_WATCH
} poll_source_type_t;

typedef struct {
    poll_source_type_t type;
    size_t idx;
} poll_source_t;

typedef struct {
    timer_id_t  id;       /* 0 = unused */
    actor_id_t  owner;
    int         fd;       /* timerfd (Linux) */
    bool        periodic;
} timer_entry_t;

typedef struct {
    int         fd;       /* -1 = unused */
    uint32_t    events;   /* POLLIN | POLLOUT */
    actor_id_t  owner;
} fd_watch_entry_t;

#define NAME_REGISTRY_SIZE 128
#define NAME_MAX_LEN 64

typedef struct {
    char       name[NAME_MAX_LEN];
    actor_id_t actor_id;
    bool       occupied;
} name_entry_t;

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

    return rt;
}

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
            close(rt->timers[i].fd);
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
    if (a) a->status = ACTOR_STOPPED;
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

/* ── Execution ──────────────────────────────────────────────────────── */

/* Forward declarations for service cleanup */
void name_registry_deregister_actor(runtime_t *rt, actor_id_t id);

static void cleanup_stopped(runtime_t *rt) {
    for (size_t i = 1; i < rt->max_actors; i++) {
        actor_t *a = rt->actors[i];
        if (a && a->status == ACTOR_STOPPED) {
            actor_id_t id = a->id;
            /* Clean up timers owned by this actor */
            for (size_t t = 0; t < MAX_TIMERS; t++) {
                if (rt->timers[t].id != TIMER_ID_INVALID &&
                    rt->timers[t].owner == id) {
                    close(rt->timers[t].fd);
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
    if (!actor) return;

    if (actor->status == ACTOR_STOPPED) return;

    actor->status = ACTOR_RUNNING;
    rt->current_actor = actor;

    /* Process one message per turn for fairness */
    message_t *msg = mailbox_dequeue(actor->mailbox);
    if (msg) {
        bool keep = actor->behavior(rt, actor, msg, actor->state);
        message_destroy(msg);
        if (!keep) {
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
                close(te->fd);
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
                      (count_active_watches(rt) > 0);

        if (has_io) {
            /* Non-blocking poll for events */
            bool received = poll_and_dispatch(rt, 0);

            if (!received && scheduler_is_empty(&rt->scheduler)) {
                if (rt->actor_count == 0) break;
                /* Block with timeout, then poll again */
                poll_and_dispatch(rt, 100);
            }
        } else {
            /* No IO sources → exit when scheduler empty */
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
