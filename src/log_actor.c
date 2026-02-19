#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime_internal.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static const char *level_str(int level) {
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "???";
    }
}

static bool log_behavior(runtime_t *rt, actor_t *self,
                         message_t *msg, void *state) {
    (void)rt; (void)self; (void)state;
    if (msg->type != MSG_LOG) return true;
    if (msg->payload_size < sizeof(log_payload_t)) return true;

    const log_payload_t *lp = (const log_payload_t *)msg->payload;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%ld.%03ld] [%s] actor=%lu: %s\n",
            (long)ts.tv_sec, ts.tv_nsec / 1000000,
            level_str(lp->level),
            (unsigned long)lp->source,
            lp->text);
    return true;
}

void runtime_enable_logging(runtime_t *rt) {
    if (runtime_get_log_actor(rt) != ACTOR_ID_INVALID) return;
    actor_id_t id = actor_spawn(rt, log_behavior, NULL, NULL, 64);
    if (id != ACTOR_ID_INVALID) {
        runtime_set_log_actor(rt, id);
    }
}

void actor_log(runtime_t *rt, int level, const char *fmt, ...) {
    actor_id_t log_id = runtime_get_log_actor(rt);
    if (log_id == ACTOR_ID_INVALID) return;
    if (level < runtime_get_min_log_level(rt)) return;

    log_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.level = level;
    payload.source = runtime_current_actor_id(rt);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload.text, sizeof(payload.text), fmt, ap);
    va_end(ap);

    actor_send(rt, log_id, MSG_LOG, &payload, sizeof(payload));
}
