# API Reference

## Core types — `microkernel/types.h`

### Typedefs

| Type | Underlying | Description |
|------|-----------|-------------|
| `actor_id_t` | `uint64_t` | Actor identity. Upper 32 bits = node ID, lower 32 = local sequence. |
| `node_id_t` | `uint32_t` | Node identity within a cluster. |
| `msg_type_t` | `uint32_t` | Message type discriminator. Values `0xFF000000+` are reserved for system messages. |
| `timer_id_t` | `uint32_t` | Timer handle returned by `actor_set_timer`. |
| `http_conn_id_t` | `uint32_t` | HTTP/SSE/WS connection handle. |

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `ACTOR_ID_INVALID` | `0` | Sentinel for no/invalid actor. |
| `TIMER_ID_INVALID` | `0` | Sentinel for no/invalid timer. |
| `HTTP_CONN_ID_INVALID` | `0` | Sentinel for no/invalid connection. |

### Actor ID helpers

```c
actor_id_t actor_id_make(node_id_t node, uint32_t seq);
node_id_t  actor_id_node(actor_id_t id);
uint32_t   actor_id_seq(actor_id_t id);
```

### Behavior function

```c
typedef bool (*actor_behavior_fn)(runtime_t *rt, actor_t *self,
                                  message_t *msg, void *state);
```

Called once per message. Return `true` to continue, `false` to stop the actor.

---

## Runtime — `microkernel/runtime.h`

### Lifecycle

#### `runtime_init`

```c
runtime_t *runtime_init(node_id_t node_id, size_t max_actors);
```

Create a runtime. `node_id` identifies this node in a multi-node setup (use any non-zero value for single-node). `max_actors` is the maximum number of concurrent actors.

#### `runtime_destroy`

```c
void runtime_destroy(runtime_t *rt);
```

Destroy the runtime and free all resources. All actors, timers, transports, and connections are cleaned up.

### Actor lifecycle

#### `actor_spawn`

```c
actor_id_t actor_spawn(runtime_t *rt, actor_behavior_fn behavior,
                       void *initial_state, void (*free_state)(void *),
                       size_t mailbox_size);
```

Spawn a new actor. Returns its ID, or `ACTOR_ID_INVALID` on failure. `free_state` is called on the state pointer when the actor stops (may be NULL). `mailbox_size` is rounded up to the next power of 2.

#### `actor_stop`

```c
void actor_stop(runtime_t *rt, actor_id_t id);
```

Stop an actor. Its timers and FD watches are cleaned up, name registry entries removed.

### Messaging

#### `actor_send`

```c
bool actor_send(runtime_t *rt, actor_id_t dest, msg_type_t type,
                const void *payload, size_t payload_size);
```

Send a message to an actor. The payload is deep-copied. For remote actors (different node ID), the message is serialized and sent via the registered transport. Returns `false` if the destination is unreachable.

### Context helpers

#### `actor_self`

```c
actor_id_t actor_self(runtime_t *rt);
```

Returns the ID of the currently executing actor. Only valid inside a behavior function.

#### `actor_state`

```c
void *actor_state(runtime_t *rt);
```

Returns the state pointer of the currently executing actor.

### Transport

#### `runtime_add_transport`

```c
bool runtime_add_transport(runtime_t *rt, transport_t *transport);
```

Register a transport for communication with a remote node. The transport's `peer_node` field determines which node it routes to. Up to 8 transports.

### Execution

#### `runtime_run`

```c
void runtime_run(runtime_t *rt);
```

Blocking event loop. Dispatches messages and polls for I/O until `runtime_stop` is called or all actors are stopped.

#### `runtime_step`

```c
void runtime_step(runtime_t *rt);
```

Process a single scheduling iteration: dequeue one actor, deliver one message, call the behavior function.

#### `runtime_stop`

```c
void runtime_stop(runtime_t *rt);
```

Signal the event loop to exit. Can be called from inside a behavior function.

---

## Message — `microkernel/message.h`

#### `message_create`

```c
message_t *message_create(actor_id_t source, actor_id_t dest,
                          msg_type_t type, const void *payload,
                          size_t payload_size);
```

Create a message with a deep copy of the payload. Used internally; prefer `actor_send` for normal messaging.

#### `message_destroy`

```c
void message_destroy(message_t *msg);
```

Free a message and its payload.

### Message struct

```c
struct message {
    actor_id_t  source;
    actor_id_t  dest;
    msg_type_t  type;
    size_t      payload_size;
    void       *payload;
    void      (*free_payload)(void *);
};
```

---

## Services — `microkernel/services.h`

### System message types

| Constant | Value | Payload type |
|----------|-------|-------------|
| `MSG_TIMER` | `0xFF000001` | `timer_payload_t` |
| `MSG_FD_EVENT` | `0xFF000002` | `fd_event_payload_t` |
| `MSG_LOG` | `0xFF000003` | `log_payload_t` |
| `MSG_HTTP_RESPONSE` | `0xFF000004` | `http_response_payload_t` |
| `MSG_HTTP_ERROR` | `0xFF000005` | `http_error_payload_t` |
| `MSG_SSE_OPEN` | `0xFF000006` | `sse_status_payload_t` |
| `MSG_SSE_EVENT` | `0xFF000007` | `sse_event_payload_t` |
| `MSG_SSE_CLOSED` | `0xFF000008` | `sse_status_payload_t` |
| `MSG_WS_OPEN` | `0xFF000009` | `ws_status_payload_t` |
| `MSG_WS_MESSAGE` | `0xFF00000A` | `ws_message_payload_t` |
| `MSG_WS_CLOSED` | `0xFF00000B` | `ws_status_payload_t` |
| `MSG_WS_ERROR` | `0xFF00000C` | `ws_status_payload_t` |
| `MSG_HTTP_REQUEST` | `0xFF00000D` | `http_request_payload_t` |
| `MSG_HTTP_LISTEN_ERROR` | `0xFF00000E` | - |
| `MSG_HTTP_CONN_CLOSED` | `0xFF00000F` | `ws_status_payload_t` |
| `MSG_CHILD_EXIT` | `0xFF000010` | `child_exit_payload_t` |
| `MSG_NAME_REGISTER` | `0xFF000012` | `name_register_payload_t` |
| `MSG_NAME_UNREGISTER` | `0xFF000013` | `name_unregister_payload_t` |

### Timers

#### `actor_set_timer`

```c
timer_id_t actor_set_timer(runtime_t *rt, uint64_t interval_ms, bool periodic);
```

Arm a timer. Returns `TIMER_ID_INVALID` on failure. When the timer fires, the actor receives `MSG_TIMER` with a `timer_payload_t`. One-shot timers fire once; periodic timers repeat until cancelled. Up to 32 concurrent timers across all actors.

#### `actor_cancel_timer`

```c
bool actor_cancel_timer(runtime_t *rt, timer_id_t id);
```

Cancel a timer. Only the owning actor can cancel its own timers. Returns `false` if the timer doesn't exist or isn't owned by the caller.

### Payload

```c
typedef struct {
    timer_id_t id;
    uint64_t   expirations;  // >1 if overrun
} timer_payload_t;
```

### FD watching

#### `actor_watch_fd`

```c
bool actor_watch_fd(runtime_t *rt, int fd, uint32_t events);
```

Register a file descriptor for poll monitoring. `events` uses `poll(2)` constants (e.g., `POLLIN`, `POLLOUT`). When events trigger, the actor receives `MSG_FD_EVENT`. Up to 32 concurrent watches.

#### `actor_unwatch_fd`

```c
bool actor_unwatch_fd(runtime_t *rt, int fd);
```

Remove a watched fd. Returns `false` if the fd wasn't being watched.

### Payload

```c
typedef struct {
    int      fd;
    uint32_t events;  // POLLIN, POLLOUT, etc.
} fd_event_payload_t;
```

### Name registry

#### `actor_register_name`

```c
bool actor_register_name(runtime_t *rt, const char *name, actor_id_t id);
```

Register a name for an actor. Names are unique — returns `false` if the name is already taken. Max name length: 63 characters. Up to 128 registered names.

#### `actor_lookup`

```c
actor_id_t actor_lookup(runtime_t *rt, const char *name);
```

Look up an actor by name. Returns `ACTOR_ID_INVALID` if not found.

#### `actor_send_named`

```c
bool actor_send_named(runtime_t *rt, const char *name, msg_type_t type,
                      const void *payload, size_t payload_size);
```

Convenience function: performs `actor_lookup` followed by `actor_send` in a single call. Returns `false` if the name is not registered or the send fails.

### Cross-node registry payloads

When a name is registered or unregistered, the runtime broadcasts the event to all connected TCP peers using the following payload types.

```c
typedef struct {
    char       name[64];
    actor_id_t actor_id;
} name_register_payload_t;
```

Carried by `MSG_NAME_REGISTER`. Contains the registered name and the actor ID it maps to.

```c
typedef struct {
    char name[64];
} name_unregister_payload_t;
```

Carried by `MSG_NAME_UNREGISTER`. Contains the name being removed. Names are automatically unregistered when the owning actor stops.

### Logging

#### `runtime_enable_logging`

```c
void runtime_enable_logging(runtime_t *rt);
```

Spawn the internal logging actor. Must be called before `actor_log`.

#### `runtime_set_log_level`

```c
void runtime_set_log_level(runtime_t *rt, int level);
```

Set the minimum log level. Messages below this level are discarded.

| Constant | Value |
|----------|-------|
| `LOG_DEBUG` | 0 |
| `LOG_INFO` | 1 |
| `LOG_WARN` | 2 |
| `LOG_ERROR` | 3 |

#### `actor_log`

```c
void actor_log(runtime_t *rt, int level, const char *fmt, ...);
```

Log a message. Printf-style formatting. Delivered asynchronously to the log actor. Max message length: 255 characters.

---

## HTTP / SSE / WebSocket — `microkernel/http.h`

### Client APIs

#### `actor_http_get`

```c
http_conn_id_t actor_http_get(runtime_t *rt, const char *url);
```

Perform an HTTP GET. Supports `http://` and `https://` (with OpenSSL). Returns `HTTP_CONN_ID_INVALID` on failure. The actor receives `MSG_HTTP_RESPONSE` or `MSG_HTTP_ERROR`.

#### `actor_http_fetch`

```c
http_conn_id_t actor_http_fetch(runtime_t *rt, const char *method,
                                const char *url, const char *const *headers,
                                size_t n_headers, const void *body,
                                size_t body_size);
```

Perform an HTTP request with any method. `headers` is an array of `"Header: Value"` strings. Body is optional (pass NULL/0 for no body).

#### `actor_sse_connect`

```c
http_conn_id_t actor_sse_connect(runtime_t *rt, const char *url);
```

Connect to a Server-Sent Events stream. The actor receives:
1. `MSG_SSE_OPEN` — connection established
2. `MSG_SSE_EVENT` — for each event (repeating)
3. `MSG_SSE_CLOSED` — stream ended

#### `actor_ws_connect`

```c
http_conn_id_t actor_ws_connect(runtime_t *rt, const char *url);
```

Connect to a WebSocket server. Supports `ws://` and `wss://`. The actor receives:
1. `MSG_WS_OPEN` — handshake complete
2. `MSG_WS_MESSAGE` — for each frame (repeating)
3. `MSG_WS_CLOSED` or `MSG_WS_ERROR`

#### `actor_ws_send_text`

```c
bool actor_ws_send_text(runtime_t *rt, http_conn_id_t id,
                        const char *text, size_t len);
```

Send a text frame on an open WebSocket connection.

#### `actor_ws_send_binary`

```c
bool actor_ws_send_binary(runtime_t *rt, http_conn_id_t id,
                          const void *data, size_t len);
```

Send a binary frame on an open WebSocket connection.

#### `actor_ws_close`

```c
bool actor_ws_close(runtime_t *rt, http_conn_id_t id,
                    uint16_t code, const char *reason);
```

Initiate a WebSocket close handshake. Common codes: 1000 (normal), 1001 (going away).

#### `actor_http_close`

```c
void actor_http_close(runtime_t *rt, http_conn_id_t id);
```

Close any HTTP/SSE/WS connection and free its resources.

### Server APIs

#### `actor_http_listen`

```c
bool actor_http_listen(runtime_t *rt, uint16_t port);
```

Start listening for HTTP connections on the given port (bound to localhost). Up to 8 concurrent listeners. The actor receives `MSG_HTTP_REQUEST` for each incoming request.

#### `actor_http_unlisten`

```c
bool actor_http_unlisten(runtime_t *rt, uint16_t port);
```

Stop listening on a port.

#### `actor_http_respond`

```c
bool actor_http_respond(runtime_t *rt, http_conn_id_t conn_id,
                        int status_code,
                        const char *const *headers, size_t n_headers,
                        const void *body, size_t body_size);
```

Send an HTTP response for an incoming request. `headers` is an array of `"Header: Value"` strings. Automatically adds `Content-Length` and `Connection: close`.

#### `actor_sse_start`

```c
bool actor_sse_start(runtime_t *rt, http_conn_id_t conn_id);
```

Upgrade an incoming HTTP connection to SSE mode. Sends the SSE response headers (`Content-Type: text/event-stream`). After this, use `actor_sse_push` to send events.

#### `actor_sse_push`

```c
bool actor_sse_push(runtime_t *rt, http_conn_id_t conn_id,
                    const char *event, const char *data, size_t data_size);
```

Push an SSE event. `event` is the event name (NULL for default "message"). The actor receives `MSG_HTTP_CONN_CLOSED` when the client disconnects.

#### `actor_ws_accept`

```c
bool actor_ws_accept(runtime_t *rt, http_conn_id_t conn_id);
```

Accept a WebSocket upgrade request. Sends the `101 Switching Protocols` response. After this, the connection enters WebSocket mode — use `actor_ws_send_text`/`actor_ws_send_binary` to send, and receive `MSG_WS_MESSAGE` for incoming frames.

### Response payloads

#### `http_response_payload_t` (MSG_HTTP_RESPONSE)

```c
typedef struct {
    http_conn_id_t conn_id;
    int            status_code;
    size_t         headers_size;
    size_t         body_size;
} http_response_payload_t;
```

Accessor functions:
```c
const char *http_response_headers(const http_response_payload_t *p);
const void *http_response_body(const http_response_payload_t *p);
```

Headers are packed as `"Key: Value\0Key: Value\0"` — iterate by advancing past each null terminator.

#### `http_error_payload_t` (MSG_HTTP_ERROR)

```c
typedef struct {
    http_conn_id_t conn_id;
    int            error_code;
    char           message[128];
} http_error_payload_t;
```

#### `sse_status_payload_t` (MSG_SSE_OPEN, MSG_SSE_CLOSED)

```c
typedef struct {
    http_conn_id_t conn_id;
    int            status_code;
} sse_status_payload_t;
```

#### `sse_event_payload_t` (MSG_SSE_EVENT)

```c
typedef struct {
    http_conn_id_t conn_id;
    size_t         event_size;
    size_t         data_size;
} sse_event_payload_t;
```

Accessor functions:
```c
const char *sse_event_name(const sse_event_payload_t *p);
const char *sse_event_data(const sse_event_payload_t *p);
```

#### `ws_status_payload_t` (MSG_WS_OPEN, MSG_WS_CLOSED, MSG_WS_ERROR)

```c
typedef struct {
    http_conn_id_t conn_id;
    uint16_t       close_code;
} ws_status_payload_t;
```

#### `ws_message_payload_t` (MSG_WS_MESSAGE)

```c
typedef struct {
    http_conn_id_t conn_id;
    bool           is_binary;
    size_t         data_size;
} ws_message_payload_t;
```

Accessor:
```c
const void *ws_message_data(const ws_message_payload_t *p);
```

#### `http_request_payload_t` (MSG_HTTP_REQUEST)

```c
typedef struct {
    http_conn_id_t conn_id;
    size_t method_size;
    size_t path_size;
    size_t headers_size;
    size_t body_size;
} http_request_payload_t;
```

Accessor functions:
```c
const char *http_request_method(const http_request_payload_t *p);
const char *http_request_path(const http_request_payload_t *p);
const char *http_request_headers(const http_request_payload_t *p);
const void *http_request_body(const http_request_payload_t *p);
```

---

## Supervision — `microkernel/supervision.h`

### Exit reasons

| Constant | Value | Description |
|----------|-------|-------------|
| `EXIT_NORMAL` | `0` | The actor's behavior function returned `false`. |
| `EXIT_KILLED` | `1` | The actor was stopped via `actor_stop()`. |

### Restart types (per-child policy)

| Constant | Description |
|----------|-------------|
| `RESTART_PERMANENT` | Always restart the child, regardless of exit reason. |
| `RESTART_TRANSIENT` | Restart only on abnormal exit (`EXIT_KILLED`). |
| `RESTART_TEMPORARY` | Never restart. |

### Restart strategies (per-supervisor)

| Constant | Description |
|----------|-------------|
| `STRATEGY_ONE_FOR_ONE` | Only the crashed child is restarted. |
| `STRATEGY_ONE_FOR_ALL` | All children are stopped and restarted when one crashes. |
| `STRATEGY_REST_FOR_ONE` | The crashed child and all children started after it are restarted. |

### Types

#### `state_factory_fn`

```c
typedef void *(*state_factory_fn)(void *factory_arg);
```

Produces fresh actor state on each (re)start. Receives the `factory_arg` from the child spec.

#### `child_spec_t`

```c
typedef struct {
    const char       *name;           /* for logging, may be NULL */
    actor_behavior_fn behavior;
    state_factory_fn  factory;        /* creates initial state; NULL = no state */
    void             *factory_arg;    /* passed to factory on each (re)start */
    void            (*free_state)(void *);
    size_t            mailbox_size;
    restart_type_t    restart_type;
} child_spec_t;
```

Defines a child actor managed by a supervisor. The `factory` function is called to produce the actor's initial state each time the child is (re)started. `free_state` is called when the child stops.

#### `child_exit_payload_t`

```c
typedef struct {
    actor_id_t child_id;
    uint8_t    exit_reason;
} child_exit_payload_t;
```

Payload for `MSG_CHILD_EXIT`. Delivered to the parent actor (typically a supervisor) when a child actor dies. The `exit_reason` field is one of the exit reason constants above.

### Functions

#### `supervisor_start`

```c
actor_id_t supervisor_start(runtime_t *rt,
                            restart_strategy_t strategy,
                            int max_restarts, uint64_t window_ms,
                            const child_spec_t *specs, size_t n_specs);
```

Start a supervisor actor that manages the given child specs. The supervisor enforces a restart limit: if more than `max_restarts` restarts occur within `window_ms` milliseconds, the supervisor itself stops (preventing infinite restart loops). Returns the supervisor's actor ID, or `ACTOR_ID_INVALID` on failure.

#### `supervisor_get_child`

```c
actor_id_t supervisor_get_child(runtime_t *rt, actor_id_t sup_id,
                                 size_t index);
```

Get the current actor ID of the Nth child (0-indexed). Useful for testing and inspection. Returns `ACTOR_ID_INVALID` if the index is out of range or the supervisor does not exist.

#### `supervisor_stop`

```c
void supervisor_stop(runtime_t *rt, actor_id_t sup_id);
```

Stop the supervisor and all of its children.

---

## WASM Actors — `microkernel/wasm_actor.h`

Requires the `HAVE_WASM` feature flag (CMake option `ENABLE_WASM`, default ON). Uses WAMR (WebAssembly Micro Runtime) as the execution engine.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `WASM_DEFAULT_STACK_SIZE` | `8192` | Default WASM operand stack size in bytes. |
| `WASM_DEFAULT_HEAP_SIZE` | `65536` | Default WASM heap size in bytes. |
| `FIBER_GUARD_SIZE` | `8192` | Guard region at the bottom of each fiber stack. |

### `fiber_stack_class_t`

```c
typedef enum {
    FIBER_STACK_NONE   = 0,      /* No fiber (sync only) -- default */
    FIBER_STACK_SMALL  = 32768,  /* 32 KB */
    FIBER_STACK_MEDIUM = 65536,  /* 64 KB */
    FIBER_STACK_LARGE  = 131072, /* 128 KB */
} fiber_stack_class_t;
```

Controls cooperative yielding support. `FIBER_STACK_NONE` disables blocking host calls (`mk_sleep_ms`, `mk_recv`). Larger stacks allow deeper call chains within blocking host functions. The enum values are the actual stack allocation sizes in bytes.

### Lifecycle

#### `wasm_actors_init`

```c
bool wasm_actors_init(void);
```

Initialize the WAMR runtime. Must be called once per process before spawning any WASM actors. Returns `false` on failure.

#### `wasm_actors_cleanup`

```c
void wasm_actors_cleanup(void);
```

Shut down the WAMR runtime. Call once at process exit.

### Spawning

#### `actor_spawn_wasm`

```c
actor_id_t actor_spawn_wasm(runtime_t *rt, const uint8_t *wasm_buf,
                             size_t wasm_size, size_t mailbox_size,
                             uint32_t stack_size, uint32_t heap_size,
                             fiber_stack_class_t fiber_stack);
```

Spawn a WASM actor from a bytecode or AOT buffer. The buffer is copied internally. `stack_size` and `heap_size` control the WASM execution environment (use `WASM_DEFAULT_STACK_SIZE` / `WASM_DEFAULT_HEAP_SIZE` for typical workloads). Returns `ACTOR_ID_INVALID` on failure.

#### `actor_spawn_wasm_file`

```c
actor_id_t actor_spawn_wasm_file(runtime_t *rt, const char *path,
                                  size_t mailbox_size,
                                  uint32_t stack_size, uint32_t heap_size,
                                  fiber_stack_class_t fiber_stack);
```

Spawn a WASM actor from a file path. Reads the file, then delegates to `actor_spawn_wasm`.

### Supervision integration

These functions are designed to be used as fields in a `child_spec_t`, allowing supervisors to manage WASM actors with automatic restart.

#### `wasm_actor_behavior`

```c
bool wasm_actor_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state);
```

Behavior function for supervised WASM actors. Use as the `behavior` field in `child_spec_t`.

#### `wasm_actor_factory`

```c
void *wasm_actor_factory(void *arg);
```

State factory for supervised WASM actors. `arg` is a `wasm_factory_arg_t *`. Use as the `factory` field in `child_spec_t`.

#### `wasm_actor_free`

```c
void wasm_actor_free(void *state);
```

State destructor for supervised WASM actors. Use as the `free_state` field in `child_spec_t`.

#### `wasm_factory_arg_create`

```c
wasm_factory_arg_t *wasm_factory_arg_create(const uint8_t *wasm_buf,
                                             size_t wasm_size,
                                             uint32_t stack_size,
                                             uint32_t heap_size,
                                             fiber_stack_class_t fiber_stack);
```

Create a factory argument that holds a copy of the WASM bytecode and a pre-parsed module. The module is loaded once; creating actor instances from it is cheap. The returned pointer should be passed as the `factory_arg` in `child_spec_t`.

#### `wasm_factory_arg_destroy`

```c
void wasm_factory_arg_destroy(wasm_factory_arg_t *arg);
```

Free the factory argument and its internal module. Call after the supervisor is stopped.

### WASM module contract

A WASM module used as an actor must satisfy the following interface:

**Required export:**

```
handle_message(i32 msg_type, i64 source_id, i32 payload_ptr, i32 payload_size) -> i32
```

Called once per message delivered to the actor. Return `1` to keep the actor alive, `0` to stop it. The `payload_ptr` points into WASM linear memory where the payload has been copied.

**Required imports (from `"env"` module):**

| Function | Signature | Description |
|----------|-----------|-------------|
| `mk_send` | `(i64 dest, i32 type, ptr payload, i32 size) -> i32` | Send a message to another actor. Returns 0 on success. |
| `mk_self` | `() -> i64` | Get the current actor's ID. |
| `mk_log` | `(i32 level, ptr text, i32 len)` | Log a message at the given level. |
| `mk_sleep_ms` | `(i32 ms) -> i32` | Sleep for the given duration. Requires fiber support (`fiber_stack != FIBER_STACK_NONE`). |
| `mk_recv` | `(ptr type_out, ptr buf, i32 buf_size, ptr actual_size_out) -> i32` | Block until the next message arrives. Requires fiber support. Returns 0 on success. |

---

## Socket — `microkernel/mk_socket.h`

### Vtable struct

```c
struct mk_socket {
    ssize_t (*read)(mk_socket_t *self, uint8_t *buf, size_t len);
    ssize_t (*write)(mk_socket_t *self, const uint8_t *buf, size_t len);
    void    (*close)(mk_socket_t *self);
    int     (*get_fd)(mk_socket_t *self);
    void    *ctx;
};
```

### Constructors

#### `mk_socket_tcp_connect`

```c
mk_socket_t *mk_socket_tcp_connect(const char *host, uint16_t port);
```

Connect via TCP. Resolves hostname with `getaddrinfo`, blocks on `connect()`, then sets non-blocking. Returns NULL on failure.

#### `mk_socket_tcp_wrap`

```c
mk_socket_t *mk_socket_tcp_wrap(int fd);
```

Wrap an already-connected fd. Used internally by the HTTP server accept path.

#### `mk_socket_tls_connect`

```c
mk_socket_t *mk_socket_tls_connect(const char *host, uint16_t port);  // #ifdef HAVE_TLS
```

Connect via TCP + TLS. Blocks on connect and TLS handshake. Verifies the server certificate against the system CA store. Sets SNI hostname. Returns NULL on failure (connection error, certificate verification failure, etc.). The underlying implementation is OpenSSL on Linux (`HAVE_OPENSSL`) or mbedTLS on ESP32 (`HAVE_MBEDTLS`); `HAVE_TLS` is defined when either backend is available.

---

## Transport — `microkernel/transport.h`

### Vtable struct

```c
struct transport {
    node_id_t  peer_node;
    int        fd;
    bool     (*send)(transport_t *self, const message_t *msg);
    message_t *(*recv)(transport_t *self);
    bool     (*is_connected)(transport_t *self);
    void     (*destroy)(transport_t *self);
    void      *impl;
};
```

### Unix domain sockets — `microkernel/transport_unix.h`

```c
transport_t *transport_unix_listen(const char *path, node_id_t peer_node);
transport_t *transport_unix_connect(const char *path, node_id_t peer_node);
```

Uses host byte order wire format. Server accepts lazily on first poll.

### TCP — `microkernel/transport_tcp.h`

```c
transport_t *transport_tcp_listen(const char *host, uint16_t port, node_id_t peer_node);
transport_t *transport_tcp_connect(const char *host, uint16_t port, node_id_t peer_node);
```

Uses network byte order wire format.

### UDP — `microkernel/transport_udp.h`

```c
transport_t *transport_udp_bind(const char *host, uint16_t port, node_id_t peer_node);
transport_t *transport_udp_connect(const char *host, uint16_t port, node_id_t peer_node);
```

Uses network byte order. Bound transport learns peer from first `recvfrom`, then locks in with `connect()`. Max datagram: 65507 bytes.

---

## Wire format — `microkernel/wire.h`

```c
void      *wire_serialize(const message_t *msg, size_t *out_size);
message_t *wire_deserialize(const void *buf, size_t buf_size);
void      *wire_serialize_net(const message_t *msg, size_t *out_size);
message_t *wire_deserialize_net(const void *buf, size_t buf_size);
```

Serializes messages to/from the 28-byte header + payload format. `_net` variants use big-endian encoding for cross-machine communication.

---

## Mailbox — `microkernel/mailbox.h`

```c
mailbox_t *mailbox_create(size_t capacity);    // rounds up to power of 2
void       mailbox_destroy(mailbox_t *mb);     // drains remaining messages
bool       mailbox_enqueue(mailbox_t *mb, message_t *msg);
message_t *mailbox_dequeue(mailbox_t *mb);
bool       mailbox_is_empty(const mailbox_t *mb);
size_t     mailbox_count(const mailbox_t *mb);
```

---

## Scheduler — `microkernel/scheduler.h`

```c
void     scheduler_init(scheduler_t *sched);
void     scheduler_enqueue(scheduler_t *sched, actor_t *actor);
actor_t *scheduler_dequeue(scheduler_t *sched);
bool     scheduler_is_empty(const scheduler_t *sched);
```

FIFO ready queue. Guards against double-enqueueing.
