#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_spiffs.h>
#include <esp_vfs_eventfd.h>
#include <soc/soc_caps.h>
#include <pthread.h>
#include <esp_pthread.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/wasm_actor.h"
#include "microkernel/namespace.h"
#include <errno.h>
#include <lwip/sockets.h>

#if SOC_WIFI_SUPPORTED
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "microkernel/transport_tcp.h"
#include "microkernel/http.h"
#include <lwip/sockets.h>
#include "wifi_config.h"
#endif

/* Embedded WASM binaries */
extern const uint8_t echo_wasm_start[] asm("_binary_echo_wasm_start");
extern const uint8_t echo_wasm_end[]   asm("_binary_echo_wasm_end");
extern const uint8_t shell_wasm_start[] asm("_binary_shell_wasm_start");
extern const uint8_t shell_wasm_end[]   asm("_binary_shell_wasm_end");

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

#if SOC_WIFI_SUPPORTED

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

/* ── Test 4: HTTP GET ──────────────────────────────────────────────── */

typedef struct {
    int status_code;
    char body[1024];
    size_t body_size;
    bool got_response;
    bool got_error;
} http_test_state_t;

static bool http_get_behavior(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    http_test_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        char url[128];
        snprintf(url, sizeof(url), "http://%s:8080/hello", TEST_SERVER_IP);
        ESP_LOGI(TAG, "HTTP GET %s", url);
        actor_http_get(rt, url);
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->status_code = p->status_code;
        s->body_size = p->body_size < sizeof(s->body) - 1 ?
                       p->body_size : sizeof(s->body) - 1;
        memcpy(s->body, http_response_body(p), s->body_size);
        s->body[s->body_size] = '\0';
        s->got_response = true;
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "HTTP GET error: %s", p->message);
        s->got_error = true;
        return false;
    }

    return true;
}

static bool test_http_get(void) {
    ESP_LOGI(TAG, "=== Test 4: HTTP GET ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    http_test_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, http_get_behavior, s, NULL, 16);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_response && s->status_code == 200 &&
              strstr(s->body, "hello") != NULL;

    ESP_LOGI(TAG, "HTTP GET test %s (status=%d body=%s)",
             ok ? "passed" : "FAILED", s->status_code, s->body);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 5: HTTP POST ─────────────────────────────────────────────── */

static bool http_post_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    http_test_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        char url[128];
        snprintf(url, sizeof(url), "http://%s:8080/echo", TEST_SERVER_IP);
        ESP_LOGI(TAG, "HTTP POST %s", url);
        const char *hdrs[] = {"Content-Type: text/plain"};
        const char *body = "esp32 post test";
        actor_http_fetch(rt, "POST", url, hdrs, 1, body, strlen(body));
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->status_code = p->status_code;
        s->body_size = p->body_size < sizeof(s->body) - 1 ?
                       p->body_size : sizeof(s->body) - 1;
        memcpy(s->body, http_response_body(p), s->body_size);
        s->body[s->body_size] = '\0';
        s->got_response = true;
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "HTTP POST error: %s", p->message);
        s->got_error = true;
        return false;
    }

    return true;
}

static bool test_http_post(void) {
    ESP_LOGI(TAG, "=== Test 5: HTTP POST ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    http_test_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, http_post_behavior, s, NULL, 16);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_response && s->status_code == 200 &&
              strstr(s->body, "esp32 post test") != NULL;

    ESP_LOGI(TAG, "HTTP POST test %s (status=%d body=%s)",
             ok ? "passed" : "FAILED", s->status_code, s->body);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 6: WebSocket echo ────────────────────────────────────────── */

typedef struct {
    http_conn_id_t conn_id;
    bool opened;
    bool got_echo;
    bool got_error;
    char echo_msg[256];
} ws_test_state_t;

static bool ws_echo_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    ws_test_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        char url[128];
        snprintf(url, sizeof(url), "ws://%s:8081/ws", TEST_SERVER_IP);
        ESP_LOGI(TAG, "WS connect %s", url);
        s->conn_id = actor_ws_connect(rt, url);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->opened = true;
        ESP_LOGI(TAG, "WS opened, sending message");
        actor_ws_send_text(rt, s->conn_id, "hello from esp32", 16);
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        const char *data = ws_message_data(p);
        size_t len = p->data_size < sizeof(s->echo_msg) - 1 ?
                     p->data_size : sizeof(s->echo_msg) - 1;
        memcpy(s->echo_msg, data, len);
        s->echo_msg[len] = '\0';
        ESP_LOGI(TAG, "WS received: %s", s->echo_msg);

        if (strstr(s->echo_msg, "hello from esp32")) {
            s->got_echo = true;
            actor_ws_close(rt, s->conn_id, 1000, NULL);
            return false;
        }
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        return false;
    }

    if (msg->type == MSG_WS_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "WS error: %s", p->message);
        s->got_error = true;
        return false;
    }

    return true;
}

static bool test_ws_echo(void) {
    ESP_LOGI(TAG, "=== Test 6: WebSocket echo ===");

    runtime_t *rt = runtime_init(1, 32);
    if (!rt) return false;

    ws_test_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, ws_echo_behavior, s, NULL, 32);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->opened && s->got_echo && !s->got_error;

    ESP_LOGI(TAG, "WS echo test %s (opened=%d echo=%s)",
             ok ? "passed" : "FAILED", s->opened, s->echo_msg);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 7: HTTPS GET ─────────────────────────────────────────────── */

static bool https_get_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    http_test_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        ESP_LOGI(TAG, "HTTPS GET https://httpbin.org/get");
        actor_http_get(rt, "https://httpbin.org/get");
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->status_code = p->status_code;
        s->body_size = p->body_size < sizeof(s->body) - 1 ?
                       p->body_size : sizeof(s->body) - 1;
        memcpy(s->body, http_response_body(p), s->body_size);
        s->body[s->body_size] = '\0';
        s->got_response = true;
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "HTTPS GET error: %s", p->message);
        s->got_error = true;
        return false;
    }

    return true;
}

static bool test_https_get(void) {
    ESP_LOGI(TAG, "=== Test 7: HTTPS GET ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    http_test_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, https_get_behavior, s, NULL, 16);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_response && s->status_code == 200 &&
              strstr(s->body, "httpbin.org") != NULL;

    ESP_LOGI(TAG, "HTTPS GET test %s (status=%d body_size=%zu)",
             ok ? "passed" : "FAILED", s->status_code, s->body_size);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 8: WSS echo ──────────────────────────────────────────────── */

static bool wss_echo_behavior(runtime_t *rt, actor_t *self,
                               message_t *msg, void *state) {
    (void)self;
    ws_test_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        ESP_LOGI(TAG, "WSS connect wss://ws.postman-echo.com/raw");
        s->conn_id = actor_ws_connect(rt, "wss://ws.postman-echo.com/raw");
        if (s->conn_id == HTTP_CONN_ID_INVALID) {
            ESP_LOGE(TAG, "WSS connect returned invalid");
            s->got_error = true;
            return false;
        }
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        s->opened = true;
        ESP_LOGI(TAG, "WSS opened, sending message");
        actor_ws_send_text(rt, s->conn_id, "tls from esp32", 14);
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        const char *data = ws_message_data(p);
        size_t len = p->data_size < sizeof(s->echo_msg) - 1 ?
                     p->data_size : sizeof(s->echo_msg) - 1;
        memcpy(s->echo_msg, data, len);
        s->echo_msg[len] = '\0';
        ESP_LOGI(TAG, "WSS received: %s", s->echo_msg);

        if (strstr(s->echo_msg, "tls from esp32")) {
            s->got_echo = true;
            actor_ws_close(rt, s->conn_id, 1000, NULL);
            return false;
        }
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        return false;
    }

    if (msg->type == MSG_WS_ERROR) {
        ESP_LOGE(TAG, "WSS error");
        s->got_error = true;
        return false;
    }

    return true;
}

static bool test_wss_echo(void) {
    ESP_LOGI(TAG, "=== Test 8: WSS echo ===");

    runtime_t *rt = runtime_init(1, 32);
    if (!rt) return false;

    ws_test_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, wss_echo_behavior, s, NULL, 32);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->opened && s->got_echo && !s->got_error;

    ESP_LOGI(TAG, "WSS echo test %s (opened=%d echo=%s)",
             ok ? "passed" : "FAILED", s->opened, s->echo_msg);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 9: Multi-node distributed actors over TCP loopback ───────── */

#define MN_PORT   19901
#define MN_NODE_A 1
#define MN_NODE_B 2
#define MSG_PING  100
#define MSG_PONG  101
#define MN_ROUNDS 5

typedef struct {
    actor_id_t peer;
    int count;
    int target;
} mn_state_t;

/* Node A: receives MSG_PING, replies MSG_PONG, stops at target */
static bool mn_node_a_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    mn_state_t *s = state;

    if (msg->type == MSG_PING) {
        s->count++;
        uint32_t val = (uint32_t)s->count;
        actor_send(rt, s->peer, MSG_PONG, &val, sizeof(val));
        if (s->count >= s->target) {
            runtime_stop(rt);
            return false;
        }
    }
    return true;
}

/* Node B: sends initial MSG_PING, receives MSG_PONG, replies MSG_PING, stops at target */
static bool mn_node_b_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    mn_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        uint32_t val = 0;
        actor_send(rt, s->peer, MSG_PING, &val, sizeof(val));
        return true;
    }

    if (msg->type == MSG_PONG) {
        s->count++;
        if (s->count >= s->target) {
            runtime_stop(rt);
            return false;
        }
        uint32_t val = (uint32_t)s->count;
        actor_send(rt, s->peer, MSG_PING, &val, sizeof(val));
    }
    return true;
}

static EventGroupHandle_t s_mn_event_group;
#define MN_NODE_A_DONE BIT0
#define MN_NODE_B_DONE BIT1

typedef struct {
    transport_t *tp;
    mn_state_t  *state;
} mn_task_arg_t;

static void mn_node_a_task(void *arg) {
    mn_task_arg_t *a = arg;

    runtime_t *rt = runtime_init(MN_NODE_A, 8);
    if (!rt) {
        ESP_LOGE(TAG, "Node A: runtime_init failed");
        a->state->count = -1;
        xEventGroupSetBits(s_mn_event_group, MN_NODE_A_DONE);
        vTaskDelete(NULL);
        return;
    }

    runtime_add_transport(rt, a->tp);

    a->state->peer = actor_id_make(MN_NODE_B, 1);
    a->state->target = MN_ROUNDS;
    actor_spawn(rt, mn_node_a_behavior, a->state, NULL, 16);

    ESP_LOGI(TAG, "Node A: running (waiting for ping)");
    runtime_run(rt);

    ESP_LOGI(TAG, "Node A: done (count=%d)", a->state->count);
    runtime_destroy(rt);
    xEventGroupSetBits(s_mn_event_group, MN_NODE_A_DONE);
    vTaskDelete(NULL);
}

static void mn_node_b_task(void *arg) {
    mn_task_arg_t *a = arg;

    /* Let node A's listen socket be ready */
    vTaskDelay(pdMS_TO_TICKS(100));

    transport_t *tp = transport_tcp_connect("127.0.0.1", MN_PORT, MN_NODE_A);
    if (!tp) {
        ESP_LOGE(TAG, "Node B: transport_tcp_connect failed");
        a->state->count = -1;
        xEventGroupSetBits(s_mn_event_group, MN_NODE_B_DONE);
        vTaskDelete(NULL);
        return;
    }

    runtime_t *rt = runtime_init(MN_NODE_B, 8);
    if (!rt) {
        ESP_LOGE(TAG, "Node B: runtime_init failed");
        tp->destroy(tp);
        a->state->count = -1;
        xEventGroupSetBits(s_mn_event_group, MN_NODE_B_DONE);
        vTaskDelete(NULL);
        return;
    }

    runtime_add_transport(rt, tp);

    a->state->peer = actor_id_make(MN_NODE_A, 1);
    a->state->target = MN_ROUNDS;
    actor_id_t id = actor_spawn(rt, mn_node_b_behavior, a->state, NULL, 16);

    /* Bootstrap triggers initial MSG_PING */
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);

    ESP_LOGI(TAG, "Node B: running (sending first ping)");
    runtime_run(rt);

    ESP_LOGI(TAG, "Node B: done (count=%d)", a->state->count);
    runtime_destroy(rt);
    xEventGroupSetBits(s_mn_event_group, MN_NODE_B_DONE);
    vTaskDelete(NULL);
}

static bool test_multinode(void) {
    ESP_LOGI(TAG, "=== Test 9: Multi-node distributed actors ===");

    s_mn_event_group = xEventGroupCreate();

    mn_state_t *state_a = calloc(1, sizeof(*state_a));
    mn_state_t *state_b = calloc(1, sizeof(*state_b));

    /* Bind listen socket before spawning tasks */
    transport_t *srv_tp = transport_tcp_listen("0.0.0.0", MN_PORT, MN_NODE_B);
    if (!srv_tp) {
        ESP_LOGE(TAG, "transport_tcp_listen failed");
        free(state_a);
        free(state_b);
        vEventGroupDelete(s_mn_event_group);
        return false;
    }

    mn_task_arg_t arg_a = { .tp = srv_tp, .state = state_a };
    mn_task_arg_t arg_b = { .tp = NULL,   .state = state_b };

    xTaskCreate(mn_node_a_task, "mn_node_a", 4096, &arg_a, 5, NULL);
    xTaskCreate(mn_node_b_task, "mn_node_b", 4096, &arg_b, 5, NULL);

    EventBits_t bits = xEventGroupWaitBits(s_mn_event_group,
        MN_NODE_A_DONE | MN_NODE_B_DONE, pdTRUE, pdTRUE,
        pdMS_TO_TICKS(30000));

    bool ok = (bits & (MN_NODE_A_DONE | MN_NODE_B_DONE)) ==
              (MN_NODE_A_DONE | MN_NODE_B_DONE) &&
              state_a->count >= MN_ROUNDS &&
              state_b->count >= MN_ROUNDS;

    ESP_LOGI(TAG, "Multi-node test %s (A=%d B=%d rounds)",
             ok ? "passed" : "FAILED", state_a->count, state_b->count);

    free(state_a);
    free(state_b);
    vEventGroupDelete(s_mn_event_group);
    return ok;
}

/* ── Test 10: Cross-device distributed actors over WiFi ────────────── */

#define DISCOVER_PORT  19902
#define CROSS_TCP_PORT 19903

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;        /* network byte order */
    bool     is_server; /* true = I am node A (server, lower MAC) */
} peer_info_t;

static bool discover_peer(peer_info_t *peer, uint8_t my_mac[6], uint32_t my_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Discovery: socket() failed");
        return false;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Discovery: bind() failed");
        close(sock);
        return false;
    }

    /* 200ms receive timeout */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bcast_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    /* Announcement: "MK" (2) + MAC (6) + IP (4) + port (2) = 14 bytes */
    uint8_t pkt[14];
    pkt[0] = 'M'; pkt[1] = 'K';
    memcpy(&pkt[2], my_mac, 6);
    memcpy(&pkt[8], &my_ip, 4);
    uint16_t port_net = htons(CROSS_TCP_PORT);
    memcpy(&pkt[12], &port_net, 2);

    bool found = false;
    TickType_t found_at = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(15000);

    while (xTaskGetTickCount() < deadline) {
        /* Keep broadcasting even after finding peer, so peer discovers us too */
        sendto(sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));

        if (found && (xTaskGetTickCount() - found_at) >= pdMS_TO_TICKS(2000))
            break;

        uint8_t recv_buf[14];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                         (struct sockaddr *)&from, &from_len);

        if (!found && n == 14 && recv_buf[0] == 'M' && recv_buf[1] == 'K') {
            /* Ignore our own broadcasts */
            if (memcmp(&recv_buf[2], my_mac, 6) != 0) {
                memcpy(peer->mac, &recv_buf[2], 6);
                memcpy(&peer->ip, &recv_buf[8], 4);
                peer->is_server = (memcmp(my_mac, peer->mac, 6) < 0);

                struct in_addr addr = { .s_addr = peer->ip };
                ESP_LOGI(TAG, "Discovered peer: %s (I am %s)",
                         inet_ntoa(addr),
                         peer->is_server ? "server/node A" : "client/node B");
                found = true;
                found_at = xTaskGetTickCount();
            }
        }
    }

    close(sock);
    return found;
}

static bool run_cross_server(peer_info_t *peer) {
    (void)peer;
    ESP_LOGI(TAG, "Cross-device: running as server (node A) on port %d", CROSS_TCP_PORT);

    transport_t *tp = transport_tcp_listen("0.0.0.0", CROSS_TCP_PORT, MN_NODE_B);
    if (!tp) {
        ESP_LOGE(TAG, "Cross-device: transport_tcp_listen failed");
        return false;
    }

    runtime_t *rt = runtime_init(MN_NODE_A, 8);
    if (!rt) {
        tp->destroy(tp);
        return false;
    }

    runtime_add_transport(rt, tp);

    mn_state_t *state = calloc(1, sizeof(*state));
    state->peer = actor_id_make(MN_NODE_B, 1);
    state->target = MN_ROUNDS;
    actor_spawn(rt, mn_node_a_behavior, state, NULL, 16);

    ESP_LOGI(TAG, "Cross-device server: waiting for ping from peer...");
    runtime_run(rt);

    bool ok = (state->count >= MN_ROUNDS);
    ESP_LOGI(TAG, "Cross-device server: done (count=%d)", state->count);

    runtime_destroy(rt);
    free(state);
    return ok;
}

static bool run_cross_client(peer_info_t *peer) {
    /* Give server time to set up listen socket */
    vTaskDelay(pdMS_TO_TICKS(200));

    struct in_addr addr = { .s_addr = peer->ip };
    char *peer_ip = inet_ntoa(addr);
    ESP_LOGI(TAG, "Cross-device: connecting to %s:%d", peer_ip, CROSS_TCP_PORT);

    transport_t *tp = transport_tcp_connect(peer_ip, CROSS_TCP_PORT, MN_NODE_A);
    if (!tp) {
        ESP_LOGE(TAG, "Cross-device: transport_tcp_connect failed");
        return false;
    }

    runtime_t *rt = runtime_init(MN_NODE_B, 8);
    if (!rt) {
        tp->destroy(tp);
        return false;
    }

    runtime_add_transport(rt, tp);

    mn_state_t *state = calloc(1, sizeof(*state));
    state->peer = actor_id_make(MN_NODE_A, 1);
    state->target = MN_ROUNDS;
    actor_id_t id = actor_spawn(rt, mn_node_b_behavior, state, NULL, 16);

    /* Bootstrap triggers initial MSG_PING */
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);

    ESP_LOGI(TAG, "Cross-device client: sending first ping...");
    runtime_run(rt);

    bool ok = (state->count >= MN_ROUNDS);
    ESP_LOGI(TAG, "Cross-device client: done (count=%d)", state->count);

    runtime_destroy(rt);
    free(state);
    return ok;
}

static bool test_cross_device(void) {
    ESP_LOGI(TAG, "=== Test 10: Cross-device distributed actors ===");

    uint8_t my_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);

    ESP_LOGI(TAG, "My MAC: %02x:%02x:%02x:%02x:%02x:%02x  IP: " IPSTR,
             my_mac[0], my_mac[1], my_mac[2],
             my_mac[3], my_mac[4], my_mac[5],
             IP2STR(&ip_info.ip));

    peer_info_t peer = {0};
    if (!discover_peer(&peer, my_mac, ip_info.ip.addr)) {
        ESP_LOGW(TAG, "Test 10: no peer discovered, skipping cross-device test");
        return true; /* skip, not fail */
    }

    if (peer.is_server) {
        return run_cross_server(&peer);
    } else {
        return run_cross_client(&peer);
    }
}

/* ── Test 11: HTTP Server GET 200 (self-test via loopback) ─────────── */

#define HTTP_SRV_GET_PORT  19904
#define HTTP_SRV_POST_PORT 19905
#define SSE_SRV_PORT       19906
#define WS_SRV_PORT        19907

typedef struct {
    int status_code;
    bool got_request;
    bool got_response;
    char body[256];
} http_srv_state_t;

static bool http_srv_get_behavior(runtime_t *rt, actor_t *self,
                                   message_t *msg, void *state) {
    (void)self;
    http_srv_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        actor_http_listen(rt, HTTP_SRV_GET_PORT);
        actor_http_get(rt, "http://127.0.0.1:19904/hello");
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->got_request = true;
        ESP_LOGI(TAG, "Server got %s %s",
                 http_request_method(p), http_request_path(p));
        actor_http_respond(rt, p->conn_id, 200, NULL, 0,
                           "hello from esp32", 16);
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->got_response = true;
        s->status_code = p->status_code;
        size_t len = p->body_size < sizeof(s->body) - 1 ?
                     p->body_size : sizeof(s->body) - 1;
        memcpy(s->body, http_response_body(p), len);
        s->body[len] = '\0';
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "HTTP srv GET error: %s", p->message);
        return false;
    }

    return true;
}

static bool test_http_server_get(void) {
    ESP_LOGI(TAG, "=== Test 11: HTTP Server GET 200 ===");

    runtime_t *rt = runtime_init(1, 32);
    if (!rt) return false;

    http_srv_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, http_srv_get_behavior, s, NULL, 32);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_request && s->got_response &&
              s->status_code == 200 &&
              strstr(s->body, "hello from esp32") != NULL;

    ESP_LOGI(TAG, "HTTP Server GET test %s (req=%d resp=%d status=%d body=%s)",
             ok ? "passed" : "FAILED",
             s->got_request, s->got_response, s->status_code, s->body);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 12: HTTP Server POST Echo (self-test via loopback) ──────── */

static bool http_srv_post_behavior(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state) {
    (void)self;
    http_srv_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        actor_http_listen(rt, HTTP_SRV_POST_PORT);
        const char *hdrs[] = {"Content-Type: text/plain"};
        actor_http_fetch(rt, "POST", "http://127.0.0.1:19905/echo",
                         hdrs, 1, "ping", 4);
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->got_request = true;
        const void *body = http_request_body(p);
        ESP_LOGI(TAG, "Server got POST, body_size=%zu", p->body_size);
        actor_http_respond(rt, p->conn_id, 200, NULL, 0,
                           body, p->body_size);
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        s->got_response = true;
        s->status_code = p->status_code;
        size_t len = p->body_size < sizeof(s->body) - 1 ?
                     p->body_size : sizeof(s->body) - 1;
        memcpy(s->body, http_response_body(p), len);
        s->body[len] = '\0';
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "HTTP srv POST error: %s", p->message);
        return false;
    }

    return true;
}

static bool test_http_server_post(void) {
    ESP_LOGI(TAG, "=== Test 12: HTTP Server POST Echo ===");

    runtime_t *rt = runtime_init(1, 32);
    if (!rt) return false;

    http_srv_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, http_srv_post_behavior, s, NULL, 32);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_request && s->got_response &&
              s->status_code == 200 &&
              strcmp(s->body, "ping") == 0;

    ESP_LOGI(TAG, "HTTP Server POST test %s (req=%d resp=%d status=%d body=%s)",
             ok ? "passed" : "FAILED",
             s->got_request, s->got_response, s->status_code, s->body);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 13: SSE Server Push (self-test via loopback) ────────────── */

typedef struct {
    http_conn_id_t srv_conn;
    http_conn_id_t cli_conn;
    int event_count;
    int timer_count;
    bool got_request;
    char events[2][64];
} sse_srv_state_t;

static bool sse_srv_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self;
    sse_srv_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        actor_http_listen(rt, SSE_SRV_PORT);
        s->cli_conn = actor_sse_connect(rt, "http://127.0.0.1:19906/events");
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->srv_conn = p->conn_id;
        s->got_request = true;
        actor_sse_start(rt, p->conn_id);
        actor_set_timer(rt, 50, false);
        return true;
    }

    if (msg->type == MSG_SSE_OPEN) {
        return true;
    }

    if (msg->type == MSG_TIMER) {
        s->timer_count++;
        if (s->timer_count == 1) {
            actor_sse_push(rt, s->srv_conn, NULL, "event1", 6);
            actor_sse_push(rt, s->srv_conn, NULL, "event2", 6);
            actor_set_timer(rt, 500, false);
        } else {
            /* Safety timeout */
            return false;
        }
        return true;
    }

    if (msg->type == MSG_SSE_EVENT) {
        const sse_event_payload_t *p = msg->payload;
        if (s->event_count < 2) {
            const char *data = sse_event_data(p);
            size_t len = p->data_size < 63 ? p->data_size : 63;
            memcpy(s->events[s->event_count], data, len);
            s->events[s->event_count][len] = '\0';
            ESP_LOGI(TAG, "SSE client got event: %s",
                     s->events[s->event_count]);
        }
        s->event_count++;
        if (s->event_count >= 2) {
            return false;
        }
        return true;
    }

    return true;
}

static bool test_sse_server(void) {
    ESP_LOGI(TAG, "=== Test 13: SSE Server Push ===");

    runtime_t *rt = runtime_init(1, 32);
    if (!rt) return false;

    sse_srv_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, sse_srv_behavior, s, NULL, 32);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_request && s->event_count >= 2 &&
              strcmp(s->events[0], "event1") == 0 &&
              strcmp(s->events[1], "event2") == 0;

    ESP_LOGI(TAG, "SSE Server test %s (req=%d events=%d e0=%s e1=%s)",
             ok ? "passed" : "FAILED",
             s->got_request, s->event_count,
             s->events[0], s->events[1]);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 14: WebSocket Server Echo (self-test via loopback) ──────── */

typedef struct {
    http_conn_id_t srv_conn;
    http_conn_id_t client_conn;
    bool got_echo;
    char echo_msg[256];
} ws_srv_state_t;

static bool ws_srv_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    ws_srv_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        actor_http_listen(rt, WS_SRV_PORT);
        s->client_conn = actor_ws_connect(rt, "ws://127.0.0.1:19907/ws");
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *p = msg->payload;
        s->srv_conn = p->conn_id;
        actor_ws_accept(rt, p->conn_id);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        const ws_status_payload_t *p = msg->payload;
        if (p->conn_id == s->client_conn) {
            ESP_LOGI(TAG, "WS client connected, sending message");
            actor_ws_send_text(rt, s->client_conn,
                               "hello ws server", 15);
        }
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        if (p->conn_id == s->srv_conn) {
            /* Server received message — echo back */
            const void *data = ws_message_data(p);
            actor_ws_send_text(rt, s->srv_conn, data, p->data_size);
        } else if (p->conn_id == s->client_conn) {
            /* Client received echo */
            const char *data = ws_message_data(p);
            size_t len = p->data_size < sizeof(s->echo_msg) - 1 ?
                         p->data_size : sizeof(s->echo_msg) - 1;
            memcpy(s->echo_msg, data, len);
            s->echo_msg[len] = '\0';
            s->got_echo = true;
            ESP_LOGI(TAG, "WS client got echo: %s", s->echo_msg);
            actor_ws_close(rt, s->client_conn, 1000, NULL);
        }
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        return false;
    }

    if (msg->type == MSG_WS_ERROR) {
        const http_error_payload_t *p = msg->payload;
        ESP_LOGE(TAG, "WS server echo error: %s", p->message);
        return false;
    }

    return true;
}

static bool test_ws_server(void) {
    ESP_LOGI(TAG, "=== Test 14: WebSocket Server Echo ===");

    runtime_t *rt = runtime_init(1, 32);
    if (!rt) return false;

    ws_srv_state_t *s = calloc(1, sizeof(*s));
    actor_id_t id = actor_spawn(rt, ws_srv_behavior, s, NULL, 32);
    actor_send(rt, id, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_echo &&
              strcmp(s->echo_msg, "hello ws server") == 0;

    ESP_LOGI(TAG, "WS Server echo test %s (echo=%d msg=%s)",
             ok ? "passed" : "FAILED", s->got_echo, s->echo_msg);

    runtime_destroy(rt);
    free(s);
    return ok;
}

#endif /* SOC_WIFI_SUPPORTED */

/* ── Test 15: WASM actor spawn ─────────────────────────────────────── */

static bool test_wasm_spawn(void) {
    ESP_LOGI(TAG, "=== Test 15: WASM actor spawn ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    size_t wasm_size = (size_t)(echo_wasm_end - echo_wasm_start);
    actor_id_t wasm_id = actor_spawn_wasm(rt, echo_wasm_start, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE,
                                           FIBER_STACK_NONE);
    bool ok = (wasm_id != ACTOR_ID_INVALID);
    ESP_LOGI(TAG, "WASM spawn test %s (id=%" PRIu64 ")",
             ok ? "passed" : "FAILED", (uint64_t)wasm_id);

    /* Send stop signal (msg_type 0) so runtime exits */
    if (ok) actor_send(rt, wasm_id, 0, NULL, 0);
    runtime_run(rt);
    runtime_destroy(rt);
    return ok;
}

/* ── Test 16: WASM echo (PING → PONG) ─────────────────────────────── */

#define WASM_MSG_PING       200
#define WASM_MSG_PONG       201
#define WASM_MSG_SLEEP_TEST 204
#define WASM_MSG_RECV_TEST  205
#define WASM_MSG_RECV_REPLY 206

typedef struct {
    actor_id_t wasm_id;
    msg_type_t trigger_type;
    const char *trigger_payload;
    size_t trigger_size;
    bool got_reply;
    msg_type_t reply_type;
    char reply[64];
    size_t reply_size;
    /* For recv test: send a second message after the first */
    msg_type_t second_type;
    const char *second_payload;
    size_t second_size;
} wasm_tester_state_t;

static bool wasm_tester_behavior(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state) {
    (void)self;
    wasm_tester_state_t *s = state;

    if (msg->type == MSG_BOOTSTRAP) {
        actor_send(rt, s->wasm_id, s->trigger_type,
                   s->trigger_payload, s->trigger_size);
        if (s->second_payload) {
            actor_send(rt, s->wasm_id, s->second_type,
                       s->second_payload, s->second_size);
        }
        return true;
    }

    /* Capture any reply from WASM actor */
    s->got_reply = true;
    s->reply_type = msg->type;
    s->reply_size = msg->payload_size < sizeof(s->reply) - 1 ?
                    msg->payload_size : sizeof(s->reply) - 1;
    if (s->reply_size > 0 && msg->payload)
        memcpy(s->reply, msg->payload, s->reply_size);
    s->reply[s->reply_size] = '\0';
    return false;
}

static bool test_wasm_echo(void) {
    ESP_LOGI(TAG, "=== Test 16: WASM echo ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    size_t wasm_size = (size_t)(echo_wasm_end - echo_wasm_start);
    actor_id_t wasm_id = actor_spawn_wasm(rt, echo_wasm_start, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE,
                                           FIBER_STACK_NONE);
    if (wasm_id == ACTOR_ID_INVALID) {
        ESP_LOGE(TAG, "WASM spawn failed");
        runtime_destroy(rt);
        return false;
    }

    wasm_tester_state_t *s = calloc(1, sizeof(*s));
    s->wasm_id = wasm_id;
    s->trigger_type = WASM_MSG_PING;
    s->trigger_payload = "hi";
    s->trigger_size = 2;

    actor_id_t tester = actor_spawn(rt, wasm_tester_behavior, s, NULL, 16);
    actor_send(rt, tester, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_reply && s->reply_type == WASM_MSG_PONG &&
              s->reply_size == 2 && memcmp(s->reply, "hi", 2) == 0;

    ESP_LOGI(TAG, "WASM echo test %s (reply_type=%u reply=%s)",
             ok ? "passed" : "FAILED",
             (unsigned)s->reply_type, s->reply);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 17: WASM fiber sleep ─────────────────────────────────────── */

static bool test_wasm_fiber_sleep(void) {
    ESP_LOGI(TAG, "=== Test 17: WASM fiber sleep ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    size_t wasm_size = (size_t)(echo_wasm_end - echo_wasm_start);
    actor_id_t wasm_id = actor_spawn_wasm(rt, echo_wasm_start, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE,
                                           FIBER_STACK_SMALL);
    if (wasm_id == ACTOR_ID_INVALID) {
        ESP_LOGE(TAG, "WASM spawn failed");
        runtime_destroy(rt);
        return false;
    }

    wasm_tester_state_t *s = calloc(1, sizeof(*s));
    s->wasm_id = wasm_id;
    s->trigger_type = WASM_MSG_SLEEP_TEST;
    s->trigger_payload = NULL;
    s->trigger_size = 0;

    actor_id_t tester = actor_spawn(rt, wasm_tester_behavior, s, NULL, 16);
    actor_send(rt, tester, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_reply && s->reply_type == WASM_MSG_PONG &&
              s->reply_size == 5 && memcmp(s->reply, "slept", 5) == 0;

    ESP_LOGI(TAG, "WASM fiber sleep test %s (reply_type=%u reply=%s)",
             ok ? "passed" : "FAILED",
             (unsigned)s->reply_type, s->reply);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── Test 18: WASM fiber recv ──────────────────────────────────────── */

static bool test_wasm_fiber_recv(void) {
    ESP_LOGI(TAG, "=== Test 18: WASM fiber recv ===");

    runtime_t *rt = runtime_init(1, 16);
    if (!rt) return false;

    size_t wasm_size = (size_t)(echo_wasm_end - echo_wasm_start);
    actor_id_t wasm_id = actor_spawn_wasm(rt, echo_wasm_start, wasm_size, 16,
                                           WASM_DEFAULT_STACK_SIZE,
                                           WASM_DEFAULT_HEAP_SIZE,
                                           FIBER_STACK_SMALL);
    if (wasm_id == ACTOR_ID_INVALID) {
        ESP_LOGE(TAG, "WASM spawn failed");
        runtime_destroy(rt);
        return false;
    }

    wasm_tester_state_t *s = calloc(1, sizeof(*s));
    s->wasm_id = wasm_id;
    s->trigger_type = WASM_MSG_RECV_TEST;
    s->trigger_payload = NULL;
    s->trigger_size = 0;
    /* Second message: the one mk_recv will pick up */
    s->second_type = WASM_MSG_PING;
    s->second_payload = "hello";
    s->second_size = 5;

    actor_id_t tester = actor_spawn(rt, wasm_tester_behavior, s, NULL, 16);
    actor_send(rt, tester, MSG_BOOTSTRAP, NULL, 0);
    runtime_run(rt);

    bool ok = s->got_reply && s->reply_type == WASM_MSG_RECV_REPLY &&
              s->reply_size == 5 && memcmp(s->reply, "hello", 5) == 0;

    ESP_LOGI(TAG, "WASM fiber recv test %s (reply_type=%u reply=%s)",
             ok ? "passed" : "FAILED",
             (unsigned)s->reply_type, s->reply);

    runtime_destroy(rt);
    free(s);
    return ok;
}

/* ── WASM test runner (must run in a pthread for WAMR compatibility) ── */

static void *wasm_test_runner(void *arg) {
    bool *pass = (bool *)arg;

    if (!wasm_actors_init()) {
        ESP_LOGE(TAG, "wasm_actors_init failed!");
        *pass = false;
        return NULL;
    }

    if (!test_wasm_spawn()) *pass = false;
    if (!test_wasm_echo()) *pass = false;
    if (!test_wasm_fiber_sleep()) *pass = false;
    if (!test_wasm_fiber_recv()) *pass = false;

    wasm_actors_cleanup();
    return NULL;
}

/* ── Interactive WASM Shell ─────────────────────────────────────────── */

#define MSG_SHELL_INPUT          100
#define MSG_SHELL_INIT           101
#define MSG_SPAWN_REQUEST        102
#define MSG_SPAWN_RESPONSE       103
#define MSG_SPAWN_REQUEST_NAMED  104
#define MSG_MOUNT_REQUEST        105
#define MSG_MOUNT_RESPONSE       106

typedef struct {
    actor_id_t shell;
    int        client_fd;  /* TCP client socket */
    bool       watching;
    bool       last_was_cr; /* for CRLF across read boundaries */
    char       line_buf[256];
    size_t     line_len;
} shell_console_state_t;

static bool shell_console_behavior(runtime_t *rt, actor_t *self,
                                    message_t *msg, void *state_ptr) {
    (void)self;
    shell_console_state_t *cs = state_ptr;

    if (msg->type == MSG_SHELL_INIT) {
        actor_register_name(rt, "console", actor_self(rt));
        actor_watch_fd(rt, cs->client_fd, POLLIN);
        cs->watching = true;
        return true;
    }

    if (msg->type == MSG_SPAWN_REQUEST) {
        /* Payload = raw WASM bytes; spawn a new WASM actor */
        actor_id_t new_id = ACTOR_ID_INVALID;
        if (msg->payload && msg->payload_size > 0) {
            new_id = actor_spawn_wasm(rt, msg->payload, msg->payload_size,
                                       32,
                                       WASM_DEFAULT_STACK_SIZE,
                                       4096,  /* small app heap */
                                       FIBER_STACK_NONE);
        }
        actor_send(rt, msg->source, MSG_SPAWN_RESPONSE,
                   &new_id, sizeof(new_id));
        return true;
    }

    if (msg->type == MSG_SPAWN_REQUEST_NAMED) {
        /* Payload: name_len(1) + name(name_len) + wasm_bytes */
        actor_id_t new_id = ACTOR_ID_INVALID;
        if (!msg->payload || msg->payload_size < 2) {
            actor_send(rt, msg->source, MSG_SPAWN_RESPONSE,
                       &new_id, sizeof(new_id));
            return true;
        }

        const uint8_t *p = msg->payload;
        uint8_t name_len = p[0];
        if (name_len > 63) name_len = 63;
        if (msg->payload_size < (size_t)(1 + name_len)) {
            actor_send(rt, msg->source, MSG_SPAWN_RESPONSE,
                       &new_id, sizeof(new_id));
            return true;
        }

        char name[64];
        memcpy(name, &p[1], name_len);
        name[name_len] = '\0';

        const uint8_t *wasm_bytes = &p[1 + name_len];
        size_t wasm_size = msg->payload_size - 1 - name_len;

        ESP_LOGI("shell", "SPAWN_NAMED: name='%s' wasm_size=%zu payload_size=%zu "
                 "magic=[%02x %02x %02x %02x]",
                 name, wasm_size, (size_t)msg->payload_size,
                 wasm_size >= 4 ? wasm_bytes[0] : 0,
                 wasm_size >= 4 ? wasm_bytes[1] : 0,
                 wasm_size >= 4 ? wasm_bytes[2] : 0,
                 wasm_size >= 4 ? wasm_bytes[3] : 0);

        if (wasm_size > 0) {
            new_id = actor_spawn_wasm(rt, wasm_bytes, wasm_size, 32,
                                       WASM_DEFAULT_STACK_SIZE,
                                       4096,  /* small app heap */
                                       FIBER_STACK_NONE);
        }

        if (new_id != ACTOR_ID_INVALID) {
            /* Register with base name, fall back to name_1, name_2, ... */
            char base[60];
            size_t bn = strlen(name);
            if (bn > sizeof(base) - 1) bn = sizeof(base) - 1;
            memcpy(base, name, bn);
            base[bn] = '\0';

            char reg_name[64];
            strncpy(reg_name, base, sizeof(reg_name) - 1);
            reg_name[sizeof(reg_name) - 1] = '\0';

            if (!actor_register_name(rt, reg_name, new_id)) {
                for (int i = 1; i <= 99; i++) {
                    snprintf(reg_name, sizeof(reg_name), "%s_%d", base, i);
                    if (actor_register_name(rt, reg_name, new_id))
                        break;
                }
            }

            /* Response: actor_id(8) + registered name */
            size_t rname_len = strlen(reg_name);
            uint8_t resp[8 + 64];
            memcpy(resp, &new_id, 8);
            memcpy(resp + 8, reg_name, rname_len);
            actor_send(rt, msg->source, MSG_SPAWN_RESPONSE,
                       resp, 8 + rname_len);
        } else {
            actor_send(rt, msg->source, MSG_SPAWN_RESPONSE,
                       &new_id, sizeof(new_id));
        }
        return true;
    }

    if (msg->type == MSG_MOUNT_REQUEST) {
        /* Payload: host_len(1) + host(host_len) + port_le(2) */
        uint8_t resp[64];
        if (!msg->payload || msg->payload_size < 4) {
            resp[0] = 1;
            actor_send(rt, msg->source, MSG_MOUNT_RESPONSE, resp, 1);
            return true;
        }
        const uint8_t *p = msg->payload;
        uint8_t host_len = p[0];
        if (msg->payload_size < (size_t)(1 + host_len + 2)) {
            resp[0] = 1;
            actor_send(rt, msg->source, MSG_MOUNT_RESPONSE, resp, 1);
            return true;
        }
        char host[256];
        memcpy(host, &p[1], host_len);
        host[host_len] = '\0';
        uint16_t port;
        memcpy(&port, &p[1 + host_len], 2);

        mount_result_t result;
        int rc = ns_mount_connect(rt, host, port, &result);
        if (rc == 0) {
            resp[0] = 0;
            size_t ilen = strlen(result.identity);
            memcpy(&resp[1], result.identity, ilen);
            actor_send(rt, msg->source, MSG_MOUNT_RESPONSE,
                       resp, 1 + ilen);
        } else {
            resp[0] = 1;
            actor_send(rt, msg->source, MSG_MOUNT_RESPONSE, resp, 1);
        }
        return true;
    }

    if (msg->type == MSG_FD_EVENT) {
        const fd_event_payload_t *ev = msg->payload;
        if (ev->fd != cs->client_fd) return true;

        /* Read available bytes from TCP client */
        char tmp[64];
        ssize_t n = read(cs->client_fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;  /* spurious wakeup, retry later */
            ESP_LOGE("shell", "TCP read error: %d", errno);
            runtime_stop(rt);
            return true;
        }
        if (n == 0) {
            /* Client disconnected (FIN) */
            ESP_LOGI("shell", "TCP client disconnected");
            runtime_stop(rt);
            return true;
        }

        /* Accumulate into line buffer, send complete lines */
        for (ssize_t i = 0; i < n; i++) {
            /* Skip \n that follows \r (CRLF), even across read boundaries */
            if (tmp[i] == '\n' && cs->last_was_cr) {
                cs->last_was_cr = false;
                continue;
            }
            cs->last_was_cr = (tmp[i] == '\r');

            if (tmp[i] == '\n' || tmp[i] == '\r') {
                cs->line_buf[cs->line_len] = '\0';
                actor_send(rt, cs->shell, MSG_SHELL_INPUT,
                           cs->line_buf, cs->line_len);
                cs->line_len = 0;
            } else if (cs->line_len < sizeof(cs->line_buf) - 1) {
                cs->line_buf[cs->line_len++] = tmp[i];
            }
        }
        return true;
    }

    return true;
}

#define SHELL_TCP_PORT 23

static void *shell_runner(void *arg) {
    (void)arg;

    /* Start TCP listener for shell access */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "shell: socket() failed");
        return NULL;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SHELL_TCP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "shell: bind() failed");
        close(listen_fd);
        return NULL;
    }
    listen(listen_fd, 1);
    ESP_LOGI(TAG, "shell: listening on port %d — connect with: nc <ip> %d",
             SHELL_TCP_PORT, SHELL_TCP_PORT);

    /* Block until a client connects */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    close(listen_fd);
    if (client_fd < 0) {
        ESP_LOGE(TAG, "shell: accept() failed");
        return NULL;
    }
    ESP_LOGI(TAG, "shell: TCP client connected from %s",
             inet_ntoa(client_addr.sin_addr));

    /* Set client socket non-blocking for poll-driven reads */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    /* Disable Nagle for interactive responsiveness */
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Redirect mk_print output to the TCP socket */
    wasm_actor_set_print_fd(client_fd);

    if (!wasm_actors_init()) {
        ESP_LOGE(TAG, "shell: wasm_actors_init failed");
        close(client_fd);
        return NULL;
    }

    runtime_t *rt = runtime_init(mk_node_id(), 32);
    if (!rt) {
        ESP_LOGE(TAG, "shell: runtime_init failed");
        wasm_actors_cleanup();
        close(client_fd);
        return NULL;
    }

    ns_actor_init(rt);
    ns_mount_listen(rt, MK_MOUNT_PORT);

    /* Spawn shell WASM actor from embedded binary */
    size_t shell_size = (size_t)(shell_wasm_end - shell_wasm_start);
    ESP_LOGI(TAG, "shell: loading shell.wasm (%zu bytes, 1 page)", shell_size);
    actor_id_t shell = actor_spawn_wasm(rt, shell_wasm_start, shell_size, 32,
                                         WASM_DEFAULT_STACK_SIZE,
                                         0,  /* no app heap — static buffers */
                                         FIBER_STACK_MEDIUM);
    if (shell == ACTOR_ID_INVALID) {
        ESP_LOGE(TAG, "shell: cannot spawn shell.wasm");
        runtime_destroy(rt);
        wasm_actors_cleanup();
        close(client_fd);
        return NULL;
    }

    /* Spawn console actor (watches TCP client socket) */
    static shell_console_state_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.shell = shell;
    cs.client_fd = client_fd;
    actor_id_t console = actor_spawn(rt, shell_console_behavior, &cs, NULL, 16);
    if (console == ACTOR_ID_INVALID) {
        ESP_LOGE(TAG, "shell: cannot spawn console actor");
        runtime_destroy(rt);
        wasm_actors_cleanup();
        close(client_fd);
        return NULL;
    }

    /* Send init messages */
    actor_send(rt, shell, MSG_SHELL_INIT, NULL, 0);
    actor_send(rt, console, MSG_SHELL_INIT, NULL, 0);

    ESP_LOGI(TAG, "shell: running (shell=%" PRIu64 " console=%" PRIu64 ")",
             (uint64_t)shell, (uint64_t)console);

    /* Run event loop until client disconnects or 'exit' */
    runtime_run(rt);

    runtime_destroy(rt);
    wasm_actors_cleanup();
    wasm_actor_set_print_fd(-1);  /* restore stdout */
    close(client_fd);
    ESP_LOGI(TAG, "shell: session ended");
    return NULL;
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

    /* WASM tests (no network needed).
       Run in a pthread because WAMR's ESP-IDF platform layer calls
       pthread_self() internally, which asserts if the calling task
       wasn't created via pthread_create(). */
    {
        bool wasm_pass = true;
        esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
        pcfg.stack_size = 16384;
        esp_pthread_set_cfg(&pcfg);

        pthread_t wasm_thread;
        pthread_create(&wasm_thread, NULL, wasm_test_runner, &wasm_pass);
        pthread_join(wasm_thread, NULL);
        if (!wasm_pass) pass = false;
    }

#if SOC_WIFI_SUPPORTED
    /* Network tests require WiFi (for lwIP network stack) */
    bool wifi_ok = wifi_init();
    if (wifi_ok) {
        if (!test_tcp_loopback()) pass = false;
        if (!test_http_get()) pass = false;
        if (!test_http_post()) pass = false;
        if (!test_ws_echo()) pass = false;
        if (!test_https_get()) pass = false;
        if (!test_wss_echo()) pass = false;
        if (!test_multinode()) pass = false;
        if (!test_cross_device()) pass = false;

        /* Server self-tests via loopback (no external dependencies) */
        if (!test_http_server_get()) pass = false;
        if (!test_http_server_post()) pass = false;
        if (!test_sse_server()) pass = false;
        if (!test_ws_server()) pass = false;
    } else {
        ESP_LOGW(TAG, "Skipping network tests (no WiFi)");
    }

    if (pass && wifi_ok) {
        ESP_LOGI(TAG, "All 18 smoke tests passed!");
    } else if (pass) {
        ESP_LOGI(TAG, "Core + WASM tests passed (network tests skipped — no WiFi)");
    } else {
        ESP_LOGE(TAG, "Some smoke tests FAILED!");
    }
#else
    if (pass) {
        ESP_LOGI(TAG, "All 6 smoke tests passed (no WiFi on this chip)!");
    } else {
        ESP_LOGE(TAG, "Some smoke tests FAILED!");
    }
#endif

    /* Mount SPIFFS for file storage (used by shell's "load" command) */
    {
        esp_vfs_spiffs_conf_t spiffs_cfg = {
            .base_path = "/storage",
            .partition_label = "storage",
            .max_files = 4,
            .format_if_mount_failed = true,
        };
        esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
        if (err == ESP_OK) {
            size_t total = 0, used = 0;
            esp_spiffs_info("storage", &total, &used);
            ESP_LOGI(TAG, "SPIFFS mounted: %zu/%zu bytes used", used, total);
        } else {
            ESP_LOGW(TAG, "SPIFFS mount failed (%s), file commands disabled",
                     esp_err_to_name(err));
        }
    }

#if SOC_WIFI_SUPPORTED
    /* Launch interactive WASM shell (TCP, port 23).
       Runs in a pthread (WAMR requirement). */
    ESP_LOGI(TAG, "Starting interactive shell (TCP)...");
    {
        esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
        pcfg.stack_size = 16384;
        esp_pthread_set_cfg(&pcfg);

        pthread_t shell_thread;
        pthread_create(&shell_thread, NULL, shell_runner, NULL);
        pthread_join(shell_thread, NULL);
    }
    ESP_LOGI(TAG, "Shell terminated, resetting in 5s...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
#endif
}
