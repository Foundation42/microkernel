#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_vfs_eventfd.h>
#include "microkernel/runtime.h"
#include "microkernel/message.h"

static const char *TAG = "mk_smoke";

/* ── Test 1: Ping-pong actors ──────────────────────────────────────── */

typedef struct {
    actor_id_t peer;
    int count;
    int target;
} pingpong_state_t;

static bool ping_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    pingpong_state_t *s = state;

    if (msg->type == 1) {
        s->count++;
        if (s->count < s->target) {
            actor_send(rt, s->peer, 1, NULL, 0);
        } else {
            actor_stop(rt, s->peer);
            return false;
        }
    }
    return true;
}

static bool pong_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    pingpong_state_t *s = state;

    if (msg->type == 1) {
        s->count++;
        actor_send(rt, s->peer, 1, NULL, 0);
    }
    return true;
}

static bool test_pingpong(void) {
    ESP_LOGI(TAG, "=== Test 1: Ping-pong actors ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    pingpong_state_t *ping_state = calloc(1, sizeof(*ping_state));
    pingpong_state_t *pong_state = calloc(1, sizeof(*pong_state));
    ping_state->target = 5;
    pong_state->target = 999;

    actor_id_t ping_id = actor_spawn(rt, ping_behavior, ping_state, NULL, 16);
    actor_id_t pong_id = actor_spawn(rt, pong_behavior, pong_state, NULL, 16);

    ping_state->peer = pong_id;
    pong_state->peer = ping_id;

    actor_send(rt, ping_id, 1, NULL, 0);
    runtime_run(rt);

    bool ok = (ping_state->count == 5);
    ESP_LOGI(TAG, "Ping-pong complete: %d round trips%s",
             ping_state->count, ok ? "" : " (UNEXPECTED)");

    runtime_destroy(rt);
    free(ping_state);
    free(pong_state);
    return ok;
}

/* ── Test 2: Timer ─────────────────────────────────────────────────── */

#define MSG_BOOTSTRAP ((msg_type_t)0xFE)

typedef struct {
    timer_id_t timer_id;
    int fire_count;
    int target;
} timer_state_t;

static bool timer_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state) {
    (void)self;
    timer_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        s->timer_id = actor_set_timer(rt, 100, true);
        ESP_LOGI(TAG, "Timer started (id=%" PRIu32 ")", (uint32_t)s->timer_id);
        return true;
    }
    if (msg->type == MSG_TIMER) {
        s->fire_count++;
        ESP_LOGI(TAG, "Timer fired %d times", s->fire_count);
        if (s->fire_count >= s->target) {
            actor_cancel_timer(rt, s->timer_id);
            return false;
        }
    }
    return true;
}

static bool test_timer(void) {
    ESP_LOGI(TAG, "=== Test 2: Timer ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    timer_state_t *ts = calloc(1, sizeof(*ts));
    ts->target = 3;

    actor_id_t id = actor_spawn(rt, timer_behavior, ts, NULL, 16);
    if (id == ACTOR_ID_INVALID) {
        runtime_destroy(rt);
        return false;
    }

    /* Bootstrap message triggers timer setup inside actor context */
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = (ts->fire_count == 3);
    ESP_LOGI(TAG, "Timer test %s (fire_count=%d)!", ok ? "passed" : "FAILED", ts->fire_count);

    runtime_destroy(rt);
    free(ts);
    return ok;
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void) {
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 8,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    bool pass = true;

    if (!test_pingpong()) pass = false;
    if (!test_timer()) pass = false;

    if (pass) {
        ESP_LOGI(TAG, "All smoke tests complete!");
    } else {
        ESP_LOGE(TAG, "Some smoke tests FAILED!");
    }
}
