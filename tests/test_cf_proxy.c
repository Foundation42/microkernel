#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/http.h"
#include "microkernel/cf_proxy.h"
#include "sha1.h"
#include "base64.h"
#include "json_util.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#define TEST_PORT 19896
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ── TCP + WS helpers (same pattern as test_websocket.c) ──────────── */

static int listen_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(fd, 1);
    return fd;
}

static void read_request(int fd, char *buf, size_t cap) {
    size_t pos = 0;
    while (pos < cap - 1) {
        ssize_t n = recv(fd, buf + pos, cap - 1 - pos, 0);
        if (n <= 0) break;
        pos += (size_t)n;
        buf[pos] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
}

static bool extract_ws_key(const char *req, char *key, size_t key_cap) {
    const char *p = strstr(req, "Sec-WebSocket-Key: ");
    if (!p) return false;
    p += 19;
    const char *end = strstr(p, "\r\n");
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= key_cap) return false;
    memcpy(key, p, len);
    key[len] = '\0';
    return true;
}

static void ws_accept_response(int fd, const char *client_key) {
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);
    uint8_t hash[20];
    sha1((const uint8_t *)concat, strlen(concat), hash);
    char accept_b64[32];
    base64_encode(hash, 20, accept_b64);
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n", accept_b64);
    send(fd, resp, (size_t)n, 0);
}

static ssize_t ws_read_frame(int fd, uint8_t *opcode, uint8_t *buf,
                             size_t cap) {
    uint8_t header[14];
    ssize_t n = recv(fd, header, 2, 0);
    if (n < 2) return -1;

    *opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t plen = header[1] & 0x7F;

    if (plen == 126) {
        n = recv(fd, header + 2, 2, 0);
        if (n < 2) return -1;
        plen = ((uint64_t)header[2] << 8) | header[3];
    } else if (plen == 127) {
        n = recv(fd, header + 2, 8, 0);
        if (n < 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | header[2 + i];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        n = recv(fd, mask, 4, 0);
        if (n < 4) return -1;
    }

    if (plen > cap) return -1;

    size_t pos = 0;
    while (pos < plen) {
        n = recv(fd, buf + pos, plen - pos, 0);
        if (n <= 0) return -1;
        pos += (size_t)n;
    }

    if (masked) {
        for (size_t i = 0; i < plen; i++)
            buf[i] ^= mask[i & 3];
    }

    return (ssize_t)plen;
}

static void ws_send_frame(int fd, uint8_t opcode, const void *data,
                          size_t len) {
    uint8_t header[10];
    size_t hlen = 0;
    header[hlen++] = 0x80 | opcode;
    if (len < 126) {
        header[hlen++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[hlen++] = 126;
        header[hlen++] = (uint8_t)(len >> 8);
        header[hlen++] = (uint8_t)(len & 0xFF);
    }
    send(fd, header, hlen, 0);
    if (len > 0) send(fd, data, len, 0);
}

static void ws_send_text(int fd, const char *text) {
    ws_send_frame(fd, 0x1, text, strlen(text));
}

/* ── Mock server ──────────────────────────────────────────────────── */

static void mock_handle_kv(int fd, const char *json) {
    char type[32] = "";
    json_get_str(json, "type", type, sizeof(type));
    int64_t req_id = json_get_int(json, "req_id", -1);

    char resp[1024];
    json_buf_t j;
    json_init(&j, resp, sizeof(resp));
    json_obj_open(&j);
    json_int(&j, "req_id", req_id);

    if (strcmp(type, "kv_put") == 0) {
        json_str(&j, "type", "ok");
    } else if (strcmp(type, "kv_get") == 0) {
        char key[128] = "";
        json_get_str(json, "key", key, sizeof(key));
        if (strcmp(key, "missing") == 0) {
            json_str(&j, "type", "not_found");
        } else {
            json_str(&j, "type", "value");
            json_str(&j, "value", "test_value");
        }
    } else if (strcmp(type, "kv_delete") == 0) {
        json_str(&j, "type", "ok");
    } else if (strcmp(type, "kv_list") == 0) {
        json_str(&j, "type", "keys");
        /* Inline array */
        size_t off = json_len(&j);
        const char *arr = ",\"keys\":[\"alpha\",\"beta\",\"gamma\"]";
        size_t alen = strlen(arr);
        if (off + alen < sizeof(resp)) {
            memcpy(resp + off, arr, alen);
            j.off += alen;
            resp[j.off] = '\0';
        }
    } else {
        json_str(&j, "type", "error");
        json_str(&j, "message", "unknown op");
    }

    json_obj_close(&j);
    ws_send_text(fd, resp);
}

static pid_t start_mock_server(int n_ops) {
    pid_t pid = fork();
    if (pid != 0) { usleep(50000); return pid; }

    int lfd = listen_tcp(TEST_PORT);
    /* Timeout on accept so the mock server doesn't hang forever */
    struct timeval atv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &atv, sizeof(atv));
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) { close(lfd); _exit(1); }

    char req[4096];
    read_request(cfd, req, sizeof(req));
    char key[64];
    if (!extract_ws_key(req, key, sizeof(key))) _exit(1);
    ws_accept_response(cfd, key);
    usleep(10000);

    uint8_t buf[4096];
    uint8_t opcode;
    ssize_t n = ws_read_frame(cfd, &opcode, buf, sizeof(buf));
    if (n <= 0) _exit(1);
    buf[n] = '\0';

    char auth_type[32] = "";
    json_get_str((const char *)buf, "type", auth_type, sizeof(auth_type));
    char token[128] = "";
    json_get_str((const char *)buf, "token", token, sizeof(token));

    if (strcmp(auth_type, "auth") != 0 || strcmp(token, "test-token") != 0)
        _exit(1);

    ws_send_text(cfd, "{\"type\":\"auth_ok\",\"tenant\":\"test\"}");
    usleep(10000);

    if (n_ops <= 0) {
        usleep(300000);
        close(cfd); close(lfd);
        _exit(0);
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (int i = 0; i < n_ops; i++) {
        n = ws_read_frame(cfd, &opcode, buf, sizeof(buf));
        if (n <= 0) break;
        buf[n] = '\0';
        if (opcode == 0x1)
            mock_handle_kv(cfd, (const char *)buf);
        else if (opcode == 0x8)
            break;
    }

    usleep(100000);
    close(cfd); close(lfd);
    _exit(0);
}

/* ── Stopper actor: sets timer, stops runtime on timer fire ───────── */

static bool stopper_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self; (void)state;
    if (msg->type == 1) {

        actor_set_timer(rt, 300, false); /* 300ms delay */
        return true;
    }
    if (msg->type == MSG_TIMER) {

        runtime_stop(rt);
        return false;
    }
    return true;
}

/* ── KV tester actor: timer-delayed, runs KV op sequence ──────────── */

typedef struct {
    actor_id_t cf_id;
    int        step;
    bool       done;
    int        stop_at_step;
    bool       put_ok;
    bool       get_ok;
    char       get_value[256];
    bool       delete_ok;
    bool       list_ok;
    char       list_keys[512];
    size_t     list_keys_size;
    bool       not_found_ok;
    char       error[128];
} cf_tester_state_t;

static bool cf_tester_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    cf_tester_state_t *s = state;

    /* Trigger (type=1): set 500ms one-shot timer to let WS connect+auth */
    if (msg->type == 1 && s->step == 0) {

        actor_set_timer(rt, 500, false);
        return true;
    }

    /* Timer: start KV PUT */
    if (msg->type == MSG_TIMER && s->step == 0) {

        s->step = 1;
        const char *p = "key=testkey\nvalue=testval\nttl=0";
        actor_send(rt, s->cf_id, MSG_CF_KV_PUT, p, strlen(p));
        return true;
    }

    /* PUT reply → GET */
    if (msg->type == MSG_CF_OK && s->step == 1) {
        s->put_ok = true;
        if (s->stop_at_step <= 1) goto done;
        s->step = 2;
        const char *p = "key=testkey";
        actor_send(rt, s->cf_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    /* GET reply → DELETE */
    if (msg->type == MSG_CF_VALUE && s->step == 2) {
        s->get_ok = true;
        size_t copy = msg->payload_size < sizeof(s->get_value) - 1
                     ? msg->payload_size : sizeof(s->get_value) - 1;
        memcpy(s->get_value, msg->payload, copy);
        s->get_value[copy] = '\0';
        if (s->stop_at_step <= 2) goto done;
        s->step = 3;
        const char *p = "key=testkey";
        actor_send(rt, s->cf_id, MSG_CF_KV_DELETE, p, strlen(p));
        return true;
    }

    /* DELETE reply → LIST */
    if (msg->type == MSG_CF_OK && s->step == 3) {
        s->delete_ok = true;
        if (s->stop_at_step <= 3) goto done;
        s->step = 4;
        const char *p = "prefix=\nlimit=50";
        actor_send(rt, s->cf_id, MSG_CF_KV_LIST, p, strlen(p));
        return true;
    }

    /* LIST reply → GET missing */
    if (msg->type == MSG_CF_KEYS && s->step == 4) {
        s->list_ok = true;
        s->list_keys_size = msg->payload_size < sizeof(s->list_keys) - 1
                           ? msg->payload_size : sizeof(s->list_keys) - 1;
        memcpy(s->list_keys, msg->payload, s->list_keys_size);
        s->list_keys[s->list_keys_size] = '\0';
        if (s->stop_at_step <= 4) goto done;
        s->step = 5;
        const char *p = "key=missing";
        actor_send(rt, s->cf_id, MSG_CF_KV_GET, p, strlen(p));
        return true;
    }

    /* NOT_FOUND reply → done */
    if (msg->type == MSG_CF_NOT_FOUND && s->step == 5) {
        s->not_found_ok = true;
        goto done;
    }

    /* Error → done */
    if (msg->type == MSG_CF_ERROR) {
        if (msg->payload_size > 0) {
            size_t copy = msg->payload_size < sizeof(s->error) - 1
                         ? msg->payload_size : sizeof(s->error) - 1;
            memcpy(s->error, msg->payload, copy);
            s->error[copy] = '\0';
        }

        goto done;
    }

    return true;

done:
    s->done = true;
    runtime_stop(rt);
    return false;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static int test_connect_auth(void) {
    pid_t server = start_mock_server(0);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    cf_proxy_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.url, sizeof(config.url),
             "ws://127.0.0.1:%d/ws", TEST_PORT);
    snprintf(config.token, sizeof(config.token), "test-token");

    actor_id_t cf_id = cf_proxy_init(rt, &config);
    ASSERT_NE(cf_id, ACTOR_ID_INVALID);

    /* Stopper terminates runtime after 300ms */
    actor_id_t s = actor_spawn(rt, stopper_behavior, NULL, NULL, 8);
    actor_send(rt, s, 1, NULL, 0);
    runtime_run(rt);

    actor_id_t looked_up = actor_lookup(rt, "/node/cloudflare/storage/kv");
    ASSERT_EQ(looked_up, cf_id);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_kv_put_get(void) {
    pid_t server = start_mock_server(5);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    cf_proxy_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.url, sizeof(config.url),
             "ws://127.0.0.1:%d/ws", TEST_PORT);
    snprintf(config.token, sizeof(config.token), "test-token");

    actor_id_t cf_id = cf_proxy_init(rt, &config);

    cf_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.cf_id = cf_id;
    ts.stop_at_step = 2;

    actor_id_t tester = actor_spawn(rt, cf_tester_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.put_ok);
    ASSERT(ts.get_ok);
    ASSERT(strcmp(ts.get_value, "test_value") == 0);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_kv_delete(void) {
    pid_t server = start_mock_server(5);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    cf_proxy_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.url, sizeof(config.url),
             "ws://127.0.0.1:%d/ws", TEST_PORT);
    snprintf(config.token, sizeof(config.token), "test-token");

    actor_id_t cf_id = cf_proxy_init(rt, &config);

    cf_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.cf_id = cf_id;
    ts.stop_at_step = 3;

    actor_id_t tester = actor_spawn(rt, cf_tester_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.delete_ok);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_kv_list(void) {
    pid_t server = start_mock_server(5);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    cf_proxy_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.url, sizeof(config.url),
             "ws://127.0.0.1:%d/ws", TEST_PORT);
    snprintf(config.token, sizeof(config.token), "test-token");

    actor_id_t cf_id = cf_proxy_init(rt, &config);

    cf_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.cf_id = cf_id;
    ts.stop_at_step = 4;

    actor_id_t tester = actor_spawn(rt, cf_tester_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.list_ok);
    ASSERT(strstr(ts.list_keys, "alpha") != NULL);
    ASSERT(strstr(ts.list_keys, "beta") != NULL);
    ASSERT(strstr(ts.list_keys, "gamma") != NULL);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_kv_not_found(void) {
    pid_t server = start_mock_server(5);

    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    cf_proxy_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.url, sizeof(config.url),
             "ws://127.0.0.1:%d/ws", TEST_PORT);
    snprintf(config.token, sizeof(config.token), "test-token");

    actor_id_t cf_id = cf_proxy_init(rt, &config);

    cf_tester_state_t ts;
    memset(&ts, 0, sizeof(ts));
    ts.cf_id = cf_id;
    ts.stop_at_step = 99;

    actor_id_t tester = actor_spawn(rt, cf_tester_behavior, &ts, NULL, 32);
    actor_send(rt, tester, 1, NULL, 0);
    runtime_run(rt);

    ASSERT(ts.not_found_ok);

    runtime_destroy(rt);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_namespace_paths(void) {
    /* No mock server needed — just test path registration (no WS) */
    runtime_t *rt = runtime_init(1, 64);
    ns_actor_init(rt);

    cf_proxy_config_t config;
    memset(&config, 0, sizeof(config));
    /* Empty URL = no WS connection attempt */
    snprintf(config.token, sizeof(config.token), "test-token");

    actor_id_t cf_id = cf_proxy_init(rt, &config);
    ASSERT_NE(cf_id, ACTOR_ID_INVALID);

    actor_id_t canon = actor_lookup(rt, "/node/cloudflare/storage/kv");
    ASSERT_EQ(canon, cf_id);

    actor_id_t virt = actor_lookup(rt, "/node/storage/kv");
    ASSERT_EQ(virt, cf_id);

    runtime_destroy(rt);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_cf_proxy:\n");

    RUN_TEST(test_connect_auth);
    RUN_TEST(test_kv_put_get);
    RUN_TEST(test_kv_delete);
    RUN_TEST(test_kv_list);
    RUN_TEST(test_kv_not_found);
    RUN_TEST(test_namespace_paths);

    TEST_REPORT();
}
