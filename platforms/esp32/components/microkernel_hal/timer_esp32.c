#include "runtime_internal.h"
#include <unistd.h>
#include <string.h>
#include <esp_vfs_eventfd.h>
#include <esp_timer.h>
#include <fcntl.h>

/* Static array of esp_timer handles indexed by timer slot */
#define MAX_TIMERS 32
static esp_timer_handle_t esp_timers[MAX_TIMERS];

/* Callback: write to eventfd to wake the poll loop */
static void timer_callback(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint64_t val = 1;
    write(fd, &val, sizeof(val));
}

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

    int fd = eventfd(0, 0);
    if (fd < 0) return TIMER_ID_INVALID;

    /* Set non-blocking (ESP-IDF eventfd doesn't support EFD_NONBLOCK) */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    esp_timer_create_args_t args = {
        .callback = timer_callback,
        .arg = (void *)(intptr_t)fd,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mk_timer",
    };

    esp_timer_handle_t handle;
    if (esp_timer_create(&args, &handle) != ESP_OK) {
        close(fd);
        return TIMER_ID_INVALID;
    }

    uint64_t interval_us = interval_ms * 1000;
    esp_err_t err;
    if (periodic) {
        err = esp_timer_start_periodic(handle, interval_us);
    } else {
        err = esp_timer_start_once(handle, interval_us);
    }

    if (err != ESP_OK) {
        esp_timer_delete(handle);
        close(fd);
        return TIMER_ID_INVALID;
    }

    timer_id_t id = runtime_alloc_timer_id(rt);
    timers[slot].id = id;
    timers[slot].owner = owner;
    timers[slot].fd = fd;
    timers[slot].periodic = periodic;
    esp_timers[slot] = handle;
    return id;
}

bool actor_cancel_timer(runtime_t *rt, timer_id_t id) {
    actor_id_t owner = runtime_current_actor_id(rt);
    if (owner == ACTOR_ID_INVALID) return false;

    timer_entry_t *timers = runtime_get_timers(rt);
    size_t max = runtime_get_max_timers();

    for (size_t i = 0; i < max; i++) {
        if (timers[i].id == id && timers[i].owner == owner) {
            esp_timer_stop(esp_timers[i]);
            esp_timer_delete(esp_timers[i]);
            esp_timers[i] = NULL;
            close(timers[i].fd);
            memset(&timers[i], 0, sizeof(timer_entry_t));
            return true;
        }
    }
    return false;
}

void timer_platform_close(size_t slot, int fd) {
    if (slot < MAX_TIMERS && esp_timers[slot]) {
        esp_timer_stop(esp_timers[slot]);
        esp_timer_delete(esp_timers[slot]);
        esp_timers[slot] = NULL;
    }
    close(fd);
}
