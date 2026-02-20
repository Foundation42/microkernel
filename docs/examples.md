# Examples

All examples are self-contained. Compile with:

```bash
gcc -o example example.c -Iinclude -Lbuild/src -lmicrokernel -lpthread
# Add -lssl -lcrypto for TLS examples
```

## 1. Ping-pong: basic actor messaging

Two actors exchange messages until a count is reached.

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <stdio.h>

#define MSG_PING 1
#define MSG_PONG 2

typedef struct {
    actor_id_t peer;
    int count;
} state_t;

static bool ping_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *s) {
    state_t *st = s;
    if (msg->type == MSG_PONG) {
        st->count++;
        printf("Ping received pong #%d\n", st->count);
        if (st->count < 10)
            actor_send(rt, st->peer, MSG_PING, NULL, 0);
    }
    return true;
}

static bool pong_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *s) {
    state_t *st = s;
    if (msg->type == MSG_PING) {
        st->count++;
        printf("Pong received ping #%d\n", st->count);
        actor_send(rt, st->peer, MSG_PONG, NULL, 0);
    }
    return true;
}

int main(void) {
    runtime_t *rt = runtime_init(1, 64);

    state_t ping_st = {0}, pong_st = {0};
    actor_id_t ping_id = actor_spawn(rt, ping_behavior, &ping_st, NULL, 16);
    actor_id_t pong_id = actor_spawn(rt, pong_behavior, &pong_st, NULL, 16);
    ping_st.peer = pong_id;
    pong_st.peer = ping_id;

    // Kick off: send first PING to pong actor
    actor_send(rt, pong_id, MSG_PING, NULL, 0);
    runtime_run(rt);  // runs until all mailboxes are empty

    printf("Ping count: %d, Pong count: %d\n", ping_st.count, pong_st.count);
    runtime_destroy(rt);
}
```

## 2. Timers: one-shot and periodic

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include <stdio.h>

typedef struct {
    timer_id_t periodic_timer;
    int tick_count;
} timer_state_t;

static bool timer_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *s) {
    timer_state_t *st = s;

    if (msg->type == 1) {
        // Setup: arm a 100ms periodic timer
        st->periodic_timer = actor_set_timer(rt, 100, true);
        printf("Timer armed (100ms periodic)\n");
        return true;
    }

    if (msg->type == MSG_TIMER) {
        const timer_payload_t *tp = msg->payload;
        st->tick_count++;
        printf("Tick #%d (timer_id=%u)\n", st->tick_count, tp->id);

        if (st->tick_count >= 5) {
            actor_cancel_timer(rt, st->periodic_timer);
            printf("Timer cancelled after 5 ticks\n");
            runtime_stop(rt);
        }
        return true;
    }

    return true;
}

int main(void) {
    runtime_t *rt = runtime_init(1, 64);
    timer_state_t st = {0};

    actor_id_t id = actor_spawn(rt, timer_behavior, &st, NULL, 16);
    actor_send(rt, id, 1, NULL, 0);  // trigger setup
    runtime_run(rt);

    runtime_destroy(rt);
}
```

## 3. Name registry: discovering actors by name

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include <stdio.h>

#define MSG_GREET 1

static bool greeter_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *s) {
    if (msg->type == 0) {
        // Register ourselves
        actor_register_name(rt, "greeter", actor_self(rt));
        return true;
    }
    if (msg->type == MSG_GREET) {
        printf("Greeter received: %.*s\n",
               (int)msg->payload_size, (char *)msg->payload);
        runtime_stop(rt);
    }
    return true;
}

static bool sender_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *s) {
    if (msg->type == 0) {
        // Look up the greeter by name
        actor_id_t greeter = actor_lookup(rt, "greeter");
        if (greeter != ACTOR_ID_INVALID) {
            const char *hello = "Hello from sender!";
            actor_send(rt, greeter, MSG_GREET, hello, 18);
        }
    }
    return true;
}

int main(void) {
    runtime_t *rt = runtime_init(1, 64);

    actor_id_t g = actor_spawn(rt, greeter_behavior, NULL, NULL, 16);
    actor_send(rt, g, 0, NULL, 0);  // register

    actor_id_t s = actor_spawn(rt, sender_behavior, NULL, NULL, 16);
    actor_send(rt, s, 0, NULL, 0);  // look up and send

    runtime_run(rt);
    runtime_destroy(rt);
}
```

## 4. Multi-node: two runtimes over TCP

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/transport_tcp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define MSG_HELLO 1
#define PORT 19900

// Node 2 actor: receives a message from node 1
static bool receiver_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *s) {
    if (msg->type == MSG_HELLO) {
        printf("Node 2 received: %.*s\n",
               (int)msg->payload_size, (char *)msg->payload);
        runtime_stop(rt);
    }
    return true;
}

// Node 1 actor: sends a message to node 2
static bool sender_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *s) {
    if (msg->type == 0) {
        // Target actor on node 2 (node_id=2, seq=1)
        actor_id_t remote = actor_id_make(2, 1);
        const char *text = "Hello across TCP!";
        actor_send(rt, remote, MSG_HELLO, text, strlen(text));
    }
    return true;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();

    if (pid == 0) {
        // Child: Node 2 (server)
        runtime_t *rt2 = runtime_init(2, 64);
        transport_t *t = transport_tcp_listen("127.0.0.1", PORT, 1);
        runtime_add_transport(rt2, t);

        actor_spawn(rt2, receiver_behavior, NULL, NULL, 16);
        runtime_run(rt2);
        runtime_destroy(rt2);
    } else {
        // Parent: Node 1 (client)
        usleep(50000);  // let server start
        runtime_t *rt1 = runtime_init(1, 64);
        transport_t *t = transport_tcp_connect("127.0.0.1", PORT, 2);
        runtime_add_transport(rt1, t);

        actor_id_t s = actor_spawn(rt1, sender_behavior, NULL, NULL, 16);
        actor_send(rt1, s, 0, NULL, 0);
        runtime_run(rt1);
        runtime_destroy(rt1);
    }
}
```

## 5. HTTP client: fetching a URL

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int status;
    char body[4096];
} http_state_t;

static bool http_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *s) {
    http_state_t *st = s;

    if (msg->type == 0) {
        actor_http_get(rt, "http://httpbin.org/get");
        return true;
    }

    if (msg->type == MSG_HTTP_RESPONSE) {
        const http_response_payload_t *p = msg->payload;
        st->status = p->status_code;
        size_t len = p->body_size < sizeof(st->body) - 1 ?
                     p->body_size : sizeof(st->body) - 1;
        memcpy(st->body, http_response_body(p), len);
        st->body[len] = '\0';
        printf("HTTP %d\n%s\n", st->status, st->body);
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_HTTP_ERROR) {
        const http_error_payload_t *p = msg->payload;
        printf("Error: %s\n", p->message);
        runtime_stop(rt);
        return false;
    }

    return true;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    runtime_t *rt = runtime_init(1, 16);
    http_state_t st = {0};
    actor_id_t id = actor_spawn(rt, http_behavior, &st, NULL, 16);
    actor_send(rt, id, 0, NULL, 0);
    runtime_run(rt);
    runtime_destroy(rt);
}
```

## 6. HTTPS client: TLS fetch

Same pattern as HTTP, just change the URL:

```c
// Requires OpenSSL (HAVE_OPENSSL defined at build time)
actor_http_get(rt, "https://httpbin.org/get");
```

For POST with headers:

```c
const char *hdrs[] = { "Content-Type: application/json" };
const char *body = "{\"key\": \"value\"}";
actor_http_fetch(rt, "POST", "https://httpbin.org/post",
                 hdrs, 1, body, strlen(body));
```

## 7. SSE client: subscribing to an event stream

```c
static bool sse_behavior(runtime_t *rt, actor_t *self,
                         message_t *msg, void *s) {
    if (msg->type == 0) {
        actor_sse_connect(rt, "http://localhost:8080/events");
        return true;
    }

    if (msg->type == MSG_SSE_OPEN) {
        printf("SSE connected\n");
        return true;
    }

    if (msg->type == MSG_SSE_EVENT) {
        const sse_event_payload_t *p = msg->payload;
        printf("Event: %s\nData: %.*s\n",
               sse_event_name(p),
               (int)p->data_size, sse_event_data(p));
        return true;
    }

    if (msg->type == MSG_SSE_CLOSED) {
        printf("SSE stream closed\n");
        runtime_stop(rt);
        return false;
    }

    return true;
}
```

## 8. WebSocket client: echo

```c
typedef struct {
    http_conn_id_t conn_id;
} ws_state_t;

static bool ws_behavior(runtime_t *rt, actor_t *self,
                        message_t *msg, void *s) {
    ws_state_t *st = s;

    if (msg->type == 0) {
        st->conn_id = actor_ws_connect(rt, "ws://echo.websocket.events/");
        // For TLS: actor_ws_connect(rt, "wss://echo.websocket.events/");
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        printf("WebSocket connected\n");
        actor_ws_send_text(rt, st->conn_id, "Hello WS!", 9);
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        printf("Received: %.*s\n",
               (int)p->data_size, (char *)ws_message_data(p));
        actor_ws_close(rt, st->conn_id, 1000, NULL);
        runtime_stop(rt);
        return false;
    }

    if (msg->type == MSG_WS_CLOSED) {
        printf("WebSocket closed\n");
        return false;
    }

    return true;
}
```

## 9. HTTP server: handling requests

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

static bool server_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *s) {
    if (msg->type == 0) {
        actor_http_listen(rt, 8080);
        printf("Listening on :8080\n");
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *req = msg->payload;
        const char *method = http_request_method(req);
        const char *path = http_request_path(req);
        printf("%s %s\n", method, path);

        if (strcmp(path, "/hello") == 0) {
            const char *body = "Hello, World!";
            const char *hdrs[] = { "Content-Type: text/plain" };
            actor_http_respond(rt, req->conn_id, 200,
                               hdrs, 1, body, strlen(body));
        } else {
            const char *body = "Not Found";
            actor_http_respond(rt, req->conn_id, 404,
                               NULL, 0, body, strlen(body));
        }
        actor_http_close(rt, req->conn_id);
        return true;
    }

    return true;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    runtime_t *rt = runtime_init(1, 64);
    actor_id_t id = actor_spawn(rt, server_behavior, NULL, NULL, 32);
    actor_send(rt, id, 0, NULL, 0);
    runtime_run(rt);
    runtime_destroy(rt);
}
```

## 10. SSE server: pushing events

```c
typedef struct {
    http_conn_id_t conn_id;
    timer_id_t timer;
    int event_count;
} sse_server_state_t;

static bool sse_server_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *s) {
    sse_server_state_t *st = s;

    if (msg->type == 0) {
        actor_http_listen(rt, 8080);
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *req = msg->payload;
        st->conn_id = req->conn_id;
        // Upgrade to SSE
        actor_sse_start(rt, req->conn_id);
        // Push events on a timer
        st->timer = actor_set_timer(rt, 1000, true);
        return true;
    }

    if (msg->type == MSG_TIMER) {
        st->event_count++;
        char data[64];
        int len = snprintf(data, sizeof(data), "count=%d", st->event_count);
        actor_sse_push(rt, st->conn_id, "tick", data, len);

        if (st->event_count >= 5) {
            actor_cancel_timer(rt, st->timer);
            actor_http_close(rt, st->conn_id);
            runtime_stop(rt);
        }
        return true;
    }

    if (msg->type == MSG_HTTP_CONN_CLOSED) {
        printf("Client disconnected\n");
        actor_cancel_timer(rt, st->timer);
        return true;
    }

    return true;
}
```

## 11. WebSocket server: echo

```c
typedef struct {
    http_conn_id_t conn_id;
} ws_server_state_t;

static bool ws_server_behavior(runtime_t *rt, actor_t *self,
                               message_t *msg, void *s) {
    ws_server_state_t *st = s;

    if (msg->type == 0) {
        actor_http_listen(rt, 8080);
        return true;
    }

    if (msg->type == MSG_HTTP_REQUEST) {
        const http_request_payload_t *req = msg->payload;
        st->conn_id = req->conn_id;
        // Accept WebSocket upgrade
        actor_ws_accept(rt, req->conn_id);
        return true;
    }

    if (msg->type == MSG_WS_OPEN) {
        printf("WebSocket client connected\n");
        return true;
    }

    if (msg->type == MSG_WS_MESSAGE) {
        const ws_message_payload_t *p = msg->payload;
        // Echo back
        if (p->is_binary)
            actor_ws_send_binary(rt, st->conn_id,
                                 ws_message_data(p), p->data_size);
        else
            actor_ws_send_text(rt, st->conn_id,
                               ws_message_data(p), p->data_size);
        return true;
    }

    if (msg->type == MSG_WS_CLOSED) {
        printf("WebSocket client disconnected\n");
        actor_http_close(rt, st->conn_id);
        runtime_stop(rt);
        return false;
    }

    return true;
}
```

## 12. Supervision tree

A supervisor monitors child actors and restarts them according to a configured
strategy. Here the worker deliberately crashes (returns `false`) to demonstrate
automatic restart under the one-for-one strategy.

```c
#include "microkernel/runtime.h"
#include "microkernel/supervision.h"
#include "microkernel/services.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { int restart_count; } worker_state_t;

static void *worker_factory(void *arg) {
    (void)arg;
    worker_state_t *s = calloc(1, sizeof(*s));
    return s;
}

static bool worker_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    worker_state_t *s = state;
    if (msg->type == 1) {
        printf("Worker processing (restart_count=%d)\n", s->restart_count);
        return false;  /* simulate crash */
    }
    return true;
}

int main(void) {
    runtime_t *rt = runtime_init(1, 32);

    child_spec_t specs[] = {{
        .behavior    = worker_behavior,
        .factory     = worker_factory,
        .free_state  = free,
        .factory_arg = NULL,
        .restart     = RESTART_PERMANENT,
        .name        = "worker",
    }};

    actor_id_t sup = supervisor_start(rt, STRATEGY_ONE_FOR_ONE,
                                       3, 5000, specs, 1);

    /* Send work to the child -- it will crash and be restarted */
    actor_id_t worker = supervisor_get_child(rt, sup, "worker");
    actor_send(rt, worker, 1, NULL, 0);

    runtime_run(rt);
    runtime_destroy(rt);
}
```

## 13. Named actors across nodes

`actor_register_name` broadcasts a name binding to all connected TCP peers.
Remote nodes can then use `actor_send_named` to reach the actor without
knowing its numeric ID.

```c
#include "microkernel/runtime.h"
#include "microkernel/services.h"
#include "microkernel/transport_tcp.h"

static bool greeter_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state) {
    (void)self; (void)state;
    if (msg->type == 1) {
        const char *reply = "hello back";
        actor_send(rt, msg->source, 2, reply, 10);
    }
    return true;
}

/* Node A: register a named actor */
void node_a(void) {
    runtime_t *rt = runtime_init(1, 16);
    transport_t *tp = transport_tcp_listen("0.0.0.0", 9000, 2);
    runtime_add_transport(rt, tp);

    actor_id_t id = actor_spawn(rt, greeter_behavior, NULL, NULL, 16);
    actor_register_name(rt, "greeter", id);

    runtime_run(rt);
    runtime_destroy(rt);
}

/* Node B: send to the named actor without knowing its ID */
void node_b(void) {
    runtime_t *rt = runtime_init(2, 16);
    transport_t *tp = transport_tcp_connect("127.0.0.1", 9000, 1);
    runtime_add_transport(rt, tp);

    /* Sends to "greeter" on any node that has it registered */
    actor_send_named(rt, "greeter", 1, "hi", 2);

    runtime_run(rt);
    runtime_destroy(rt);
}
```

## 14. WASM actor

Spawn an actor whose behavior is implemented in a WebAssembly module. The WASM
module must export `handle_message(i32, i64, i32, i32) -> i32` and may import
host functions from the `"env"` namespace (`mk_send`, `mk_self`, `mk_log`,
`mk_sleep_ms`, `mk_recv`).

```c
#include "microkernel/runtime.h"
#include "microkernel/wasm_actor.h"
#include <stdio.h>

/* The WASM module must export:
   handle_message(i32 msg_type, i64 source, i32 payload_ptr, i32 size) -> i32
   It can import from "env": mk_send, mk_self, mk_log, mk_sleep_ms, mk_recv */

int main(void) {
    wasm_actors_init();

    runtime_t *rt = runtime_init(1, 16);

    /* Spawn from file (sync-only, no fiber) */
    actor_id_t wasm = actor_spawn_wasm_file(rt, "echo.wasm", 16,
                                              WASM_DEFAULT_STACK_SIZE,
                                              WASM_DEFAULT_HEAP_SIZE,
                                              FIBER_STACK_NONE);

    /* Send a message -- the WASM handle_message will be called */
    uint32_t payload = 42;
    actor_send(rt, wasm, 200, &payload, sizeof(payload));

    runtime_run(rt);
    runtime_destroy(rt);
    wasm_actors_cleanup();
}
```

## 15. WASM actor with fiber (blocking concurrency)

When spawned with a fiber stack, the WASM module can call `mk_sleep_ms()` and
`mk_recv()`. These cooperatively yield back to the runtime so that other actors
continue to run while the WASM actor is "blocked."

```c
#include "microkernel/runtime.h"
#include "microkernel/wasm_actor.h"

/* With FIBER_STACK_SMALL, the WASM module can call mk_sleep_ms() and
   mk_recv() which cooperatively yield back to the runtime. The actor
   stays alive while "blocked" and other actors continue to run. */

int main(void) {
    wasm_actors_init();

    runtime_t *rt = runtime_init(1, 16);

    actor_id_t wasm = actor_spawn_wasm_file(rt, "echo.wasm", 16,
                                              WASM_DEFAULT_STACK_SIZE,
                                              WASM_DEFAULT_HEAP_SIZE,
                                              FIBER_STACK_SMALL);

    /* MSG_SLEEP_TEST (204): the WASM module calls mk_sleep_ms(50),
       yields to the runtime, timer fires, fiber resumes, sends reply */
    actor_send(rt, wasm, 204, NULL, 0);

    runtime_run(rt);
    runtime_destroy(rt);
    wasm_actors_cleanup();
}
```
