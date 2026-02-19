#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <esp_log.h>
#include <esp_vfs_eventfd.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "microkernel/runtime.h"
#include "microkernel/message.h"
#include "microkernel/transport_tcp.h"
#include "wifi_config.h"

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
    if (!rt) {
        ESP_LOGE(TAG, "runtime_init failed (out of memory?)");
        return false;
    }

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

/* ── WiFi initialization ──────────────────────────────────────────── */

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc =
            (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retrying...", disc->reason);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init(void) {
    ESP_LOGI(TAG, "Initializing WiFi — SSID=\"%s\" pass=\"%s\"", WIFI_SSID, WIFI_PASSWORD);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    /* Wait up to 60 seconds for IP (weak signal may need many auth retries) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection timed out");
        return false;
    }
    return true;
}

/* ── Test 3: TCP loopback transport ───────────────────────────────── */

#define TCP_TEST_PORT 19900

static bool test_tcp_loopback(void) {
    ESP_LOGI(TAG, "=== Test 3: TCP loopback ===");

    /* Listen on all interfaces */
    ESP_LOGI(TAG, "TCP listen on port %d", TCP_TEST_PORT);
    transport_t *server = transport_tcp_listen("0.0.0.0", TCP_TEST_PORT, 2);
    if (!server) {
        ESP_LOGE(TAG, "transport_tcp_listen failed");
        return false;
    }

    /* Connect to ourselves */
    ESP_LOGI(TAG, "TCP connect to 127.0.0.1:%d", TCP_TEST_PORT);
    transport_t *client = transport_tcp_connect("127.0.0.1", TCP_TEST_PORT, 1);
    if (!client) {
        ESP_LOGE(TAG, "transport_tcp_connect failed");
        server->destroy(server);
        return false;
    }

    /* Send a message from client to server */
    const char *payload = "hello";
    message_t *msg = message_create(
        0x200000001ULL,  /* source: node 2, actor 1 */
        0x100000001ULL,  /* dest:   node 1, actor 1 */
        42, (const uint8_t *)payload, strlen(payload));
    if (!msg) {
        ESP_LOGE(TAG, "message_create failed");
        client->destroy(client);
        server->destroy(server);
        return false;
    }

    bool sent = client->send(client, msg);
    message_destroy(msg);
    if (!sent) {
        ESP_LOGE(TAG, "transport send failed");
        client->destroy(client);
        server->destroy(server);
        return false;
    }
    ESP_LOGI(TAG, "Sent message via TCP transport");

    /* Give lwIP loopback a moment */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Server receives (triggers accept + recv) */
    message_t *recv_msg = server->recv(server);
    if (!recv_msg) {
        ESP_LOGE(TAG, "transport recv returned NULL");
        client->destroy(client);
        server->destroy(server);
        return false;
    }

    bool ok = (recv_msg->type == 42 &&
               recv_msg->payload_size == strlen(payload) &&
               memcmp(recv_msg->payload, payload, strlen(payload)) == 0);

    if (ok) {
        ESP_LOGI(TAG, "Received message via TCP transport, payload OK");
    } else {
        ESP_LOGE(TAG, "Received message but payload mismatch!");
    }
    message_destroy(recv_msg);

    client->destroy(client);
    server->destroy(server);

    ESP_LOGI(TAG, "TCP loopback test %s!", ok ? "passed" : "FAILED");
    return ok;
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void) {
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 4,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    bool pass = true;

    if (!test_pingpong()) pass = false;
    if (!test_timer()) pass = false;

    /* TCP test requires WiFi (for lwIP network stack) */
    bool wifi_ok = wifi_init();
    if (wifi_ok) {
        if (!test_tcp_loopback()) pass = false;
    } else {
        ESP_LOGW(TAG, "Skipping TCP test (no WiFi)");
    }

    if (pass && wifi_ok) {
        ESP_LOGI(TAG, "All smoke tests complete!");
    } else if (pass) {
        ESP_LOGI(TAG, "Core tests passed (TCP skipped — no WiFi)");
    } else {
        ESP_LOGE(TAG, "Some smoke tests FAILED!");
    }
}
