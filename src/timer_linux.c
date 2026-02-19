#define _GNU_SOURCE
#include "runtime_internal.h"
#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>

timer_id_t actor_set_timer(runtime_t *rt, uint64_t interval_ms, bool periodic) {
    actor_id_t owner = runtime_current_actor_id(rt);
    if (owner == ACTOR_ID_INVALID) return TIMER_ID_INVALID;

    timer_entry_t *timers = runtime_get_timers(rt);
    size_t max = runtime_get_max_timers();

    /* Find free slot */
    size_t slot = max;
    for (size_t i = 0; i < max; i++) {
        if (timers[i].id == TIMER_ID_INVALID) {
            slot = i;
            break;
        }
    }
    if (slot == max) return TIMER_ID_INVALID;

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return TIMER_ID_INVALID;

    struct itimerspec its = {0};
    its.it_value.tv_sec = (time_t)(interval_ms / 1000);
    its.it_value.tv_nsec = (long)((interval_ms % 1000) * 1000000);
    if (periodic) {
        its.it_interval = its.it_value;
    }

    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return TIMER_ID_INVALID;
    }

    timer_id_t id = runtime_alloc_timer_id(rt);
    timers[slot].id = id;
    timers[slot].owner = owner;
    timers[slot].fd = fd;
    timers[slot].periodic = periodic;
    return id;
}

bool actor_cancel_timer(runtime_t *rt, timer_id_t id) {
    actor_id_t owner = runtime_current_actor_id(rt);
    if (owner == ACTOR_ID_INVALID) return false;

    timer_entry_t *timers = runtime_get_timers(rt);
    size_t max = runtime_get_max_timers();

    for (size_t i = 0; i < max; i++) {
        if (timers[i].id == id && timers[i].owner == owner) {
            close(timers[i].fd);
            memset(&timers[i], 0, sizeof(timer_entry_t));
            return true;
        }
    }
    return false;
}

void timer_platform_close(size_t slot, int fd) {
    (void)slot;
    close(fd);
}
