# Architecture

## Overview

The microkernel is an actor-based runtime where all computation happens inside actors that communicate through asynchronous message passing. A central event loop (`runtime_run`) drives everything: dispatching messages to actors, polling for I/O events across transports/timers/HTTP connections, and managing actor lifecycles.

```
┌─────────────────────────────────────────────────────────────┐
│                        runtime_t                            │
│                                                             │
│  ┌──────────┐  ┌───────────┐  ┌─────────────────────────┐  │
│  │ Scheduler │  │  Actors[] │  │     poll_and_dispatch()  │  │
│  │ (FIFO)   │  │  id,mail, │  │                         │  │
│  │          │  │  behavior  │  │  transports[]           │  │
│  └──────────┘  └───────────┘  │  timers[]               │  │
│                               │  fd_watches[]           │  │
│  ┌──────────────────────┐     │  http_conns[]           │  │
│  │   Name Registry      │     │  http_listeners[]       │  │
│  │   (FNV-1a hash)      │     └─────────────────────────┘  │
│  └──────────────────────┘                                   │
└─────────────────────────────────────────────────────────────┘
```

## Actor model

### Actors

An actor (`actor_t`) is the fundamental unit of computation. Each actor has:

- **id** (`actor_id_t`) — 64-bit, encoded as `(node_id << 32) | local_seq`
- **mailbox** (`mailbox_t`) — lock-free ring buffer of pending messages
- **behavior** (`actor_behavior_fn`) — callback invoked with each message
- **state** (`void *`) — user data passed to the behavior function
- **status** — IDLE, READY, RUNNING, or STOPPED

The behavior function signature:

```c
bool (*actor_behavior_fn)(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state);
```

Returning `true` keeps the actor alive. Returning `false` stops it (status becomes STOPPED, resources freed at end of step).

### Messages

A message (`message_t`) carries:

- **source** / **dest** — actor IDs
- **type** (`msg_type_t`) — 32-bit discriminator. User types are any value; system types use the `0xFF000000+` range.
- **payload** — deep-copied bytes, freed automatically on message destruction

Messages are always copied on send — actors never share memory through messages.

### Mailbox

The mailbox is a power-of-2 ring buffer using bitwise modulo for O(1) enqueue/dequeue. When an actor receives a message, `mailbox_enqueue` appends it and the actor is placed on the scheduler's ready queue (if not already there).

### Scheduler

The scheduler is a simple FIFO linked list. `scheduler_enqueue` appends actors to the tail; `scheduler_dequeue` pops from the head. This provides fair round-robin scheduling — every actor with pending messages gets a turn before any actor gets a second turn.

## Event loop

The core loop in `runtime_run` alternates between two phases:

```
while (!stopped) {
    1. Drain scheduler: dequeue one actor, pop one message, call behavior
    2. If scheduler empty: poll_and_dispatch() — block on I/O until something happens
}
```

### poll_and_dispatch

This is the unified I/O multiplexer. It builds a `poll()` fd set from all active sources and dispatches events:

```
┌─────────────────────────────────────────────────┐
│                  poll() fd set                   │
│                                                  │
│  [transport fds]  [timer fds]  [fd_watch fds]    │
│  [http_conn fds]  [http_listener fds]            │
└─────────────────────────────────────────────────┘
         │
         ▼
   POLLIN/POLLOUT events
         │
         ├── Transport fd → recv message → deliver to local actor
         ├── Timer fd     → read expirations → deliver MSG_TIMER
         ├── FD watch     → deliver MSG_FD_EVENT
         ├── HTTP conn    → http_conn_drive() state machine
         └── HTTP listen  → accept() → wrap fd → allocate http_conn_t
```

Each poll source is tagged with a type enum (`POLL_SOURCE_TRANSPORT`, `POLL_SOURCE_TIMER`, etc.) so the dispatch loop knows how to handle each fd's events.

The poll timeout is 10ms when the scheduler has pending work (to stay responsive) or -1 (blocking) when idle.

## Transport layer

### Vtable abstraction

All multi-node communication goes through the `transport_t` vtable:

```c
struct transport {
    node_id_t  peer_node;
    int        fd;              // for poll()
    bool     (*send)(transport_t *self, const message_t *msg);
    message_t *(*recv)(transport_t *self);
    bool     (*is_connected)(transport_t *self);
    void     (*destroy)(transport_t *self);
    void      *impl;
};
```

Three implementations exist:

| Transport | Byte order | Connection model |
|-----------|-----------|-----------------|
| Unix domain socket | Host | Listen/accept or connect |
| TCP | Network (big-endian) | Listen/accept or connect |
| UDP | Network (big-endian) | Bind/recvfrom or connect |

### Wire format

Messages are serialized to a 28-byte packed header followed by the payload:

```
Offset  Size  Field
0       8     source (actor_id_t)
8       8     dest (actor_id_t)
16      4     type (msg_type_t)
20      4     payload_size (uint32_t)
24      4     reserved (0)
28      N     payload bytes
```

Two serialization variants:
- `wire_serialize()` / `wire_deserialize()` — host byte order, for Unix sockets (same machine)
- `wire_serialize_net()` / `wire_deserialize_net()` — network byte order (htobe64/htonl), for TCP/UDP

### Message routing

When `actor_send` is called, the runtime checks the destination's node ID:
- **Local** (same node): message goes directly into the actor's mailbox
- **Remote** (different node): message is serialized and sent via the transport registered for that node

Incoming transport messages are deserialized and delivered to local actors by matching the destination actor ID.

## Socket abstraction

The `mk_socket_t` vtable abstracts over plain TCP and TLS sockets:

```c
struct mk_socket {
    ssize_t (*read)(mk_socket_t *self, uint8_t *buf, size_t len);
    ssize_t (*write)(mk_socket_t *self, const uint8_t *buf, size_t len);
    void    (*close)(mk_socket_t *self);
    int     (*get_fd)(mk_socket_t *self);
    void    *ctx;
};
```

This is the key abstraction that makes TLS transparent. The HTTP state machine (`http_conn.c`) calls `sock->read()` and `sock->write()` everywhere — it never touches raw file descriptors. Adding TLS required only:
1. A new vtable implementation (`mk_socket_tls.c`) that wraps OpenSSL's `SSL_read`/`SSL_write`
2. Three lines changed in `http_conn.c` to select TLS vs TCP based on the URL scheme

Three constructors:
- `mk_socket_tcp_connect(host, port)` — blocking connect, then non-blocking I/O
- `mk_socket_tcp_wrap(fd)` — wraps an already-connected fd (used by server accept path)
- `mk_socket_tls_connect(host, port)` — blocking connect + TLS handshake with certificate verification, then non-blocking I/O

### TLS implementation

The TLS socket uses a lazy `SSL_CTX` singleton (initialized via `pthread_once`):
- `TLS_client_method()` with minimum TLS 1.2
- System CA store via `SSL_CTX_set_default_verify_paths()`
- Peer certificate verification enabled
- SNI hostname set via `SSL_set_tlsext_host_name()`
- Hostname verification via `SSL_set1_host()`

After the blocking handshake, `SSL_ERROR_WANT_READ`/`SSL_ERROR_WANT_WRITE` are translated to `errno = EAGAIN`, which the poll loop already handles correctly.

## HTTP state machine

### Connection lifecycle

Each HTTP connection is represented by `http_conn_t` with a state machine driven by `http_conn_drive()`:

**Client-side states:**

```
SENDING ──► RECV_STATUS ──► RECV_HEADERS ──┬──► BODY_CONTENT ──► DONE
                                           ├──► BODY_CHUNKED ──► DONE
                                           ├──► BODY_STREAM (SSE) ──► ...
                                           └──► WS_ACTIVE ──► ...
```

**Server-side states:**

```
SRV_RECV_REQUEST ──► SRV_RECV_HEADERS ──► SRV_RECV_BODY ──► IDLE
                                                              │
                              ┌────────────────────────────────┤
                              ▼                                ▼
                         SRV_SENDING                    WS_ACTIVE / SRV_SSE_ACTIVE
                              │
                              ▼
                            DONE
```

### Connection types

Each connection tracks its protocol:

| Type | Purpose | Key states |
|------|---------|-----------|
| `HTTP_CONN_HTTP` | Standard request/response | SENDING → RECV_STATUS → headers → body → DONE |
| `HTTP_CONN_SSE` | Server-Sent Events client | SENDING → headers → BODY_STREAM (continuous) |
| `HTTP_CONN_WS` | WebSocket client | SENDING → headers → WS_ACTIVE (bidirectional) |
| `HTTP_CONN_SERVER` | HTTP server response | SRV_RECV → IDLE → SRV_SENDING → DONE |
| `HTTP_CONN_SERVER_SSE` | SSE server push | SRV_RECV → IDLE → SRV_SSE_ACTIVE |
| `HTTP_CONN_SERVER_WS` | WebSocket server | SRV_RECV → IDLE → WS_ACTIVE |

### Message delivery

The HTTP state machine communicates with actors through system messages:

| Message | When | Payload |
|---------|------|---------|
| `MSG_HTTP_RESPONSE` | HTTP response complete | status, headers, body |
| `MSG_HTTP_ERROR` | Connection/parse error | error message |
| `MSG_SSE_OPEN` | SSE stream connected | status code |
| `MSG_SSE_EVENT` | SSE event received | event name, data |
| `MSG_SSE_CLOSED` | SSE stream ended | status code |
| `MSG_WS_OPEN` | WebSocket handshake done | - |
| `MSG_WS_MESSAGE` | WebSocket frame received | is_binary, data |
| `MSG_WS_CLOSED` | WebSocket closed | close code |
| `MSG_WS_ERROR` | WebSocket error | - |
| `MSG_HTTP_REQUEST` | Server received request | method, path, headers, body |
| `MSG_HTTP_CONN_CLOSED` | Client disconnected | conn_id |

### Server: listeners and accept

Server-side HTTP uses `http_listener_t` entries (max 8 concurrent listeners). Each listener holds a non-blocking `listen_fd`. During `poll_and_dispatch`:

1. `POLLIN` on a listener fd triggers `accept()`
2. The new fd is wrapped with `mk_socket_tcp_wrap()`
3. An `http_conn_t` is allocated in `SRV_RECV_REQUEST` state
4. The connection's `is_server` flag is set, binding it to the listener's owning actor

From there, the same `http_conn_drive()` state machine handles parsing the request and delivering `MSG_HTTP_REQUEST` to the actor.

## Runtime services

### Timers

Built on Linux `timerfd_create(CLOCK_MONOTONIC)`. Each timer is a real fd that the poll loop watches.

- `actor_set_timer(rt, interval_ms, periodic)` — returns a `timer_id_t`
- `actor_cancel_timer(rt, id)` — validates ownership, closes the timerfd
- Fires `MSG_TIMER` with `timer_payload_t` containing the timer ID and expiration count
- Timers are auto-cleaned when their owning actor stops

### FD watching

Allows actors to poll arbitrary file descriptors:

- `actor_watch_fd(rt, fd, events)` — registers the fd in the poll set
- `actor_unwatch_fd(rt, fd)` — removes it
- Fires `MSG_FD_EVENT` with `fd_event_payload_t` containing the fd and triggered events
- Auto-cleaned on actor stop

### Name registry

A fixed-size (128 entries) hash table with FNV-1a hashing and linear probing:

- `actor_register_name(rt, "my_service", id)` — returns false on collision
- `actor_lookup(rt, "my_service")` — returns `ACTOR_ID_INVALID` if not found
- Entries are automatically deregistered when an actor stops

### Logging

A dedicated logging actor processes `MSG_LOG` messages:

- `runtime_enable_logging(rt)` — spawns the log actor
- `runtime_set_log_level(rt, LOG_INFO)` — filters below this level
- `actor_log(rt, LOG_INFO, "fmt", ...)` — printf-style, delivers asynchronously
- Output format: `[timestamp] LEVEL [actor_id] message`

## Supervision trees

### Death notification

Each `actor_t` carries two fields that enable parent-child relationships:

- **parent** (`actor_id_t`) — the actor that spawned this child (set by the supervisor)
- **exit_reason** (`uint8_t`) — `EXIT_NORMAL` (0) when the behavior returns `false`, `EXIT_KILLED` (1) when `actor_stop()` is called externally

When an actor enters the STOPPED state, `cleanup_stopped()` in the runtime delivers a `MSG_CHILD_EXIT` (0xFF000010) message to the parent before destroying the actor's resources. The payload is a `child_exit_payload_t` containing the child's ID and exit reason. This guarantees that a parent always learns of its child's termination, regardless of how the child stopped.

A subtle detail: `runtime_step()` calls `cleanup_stopped()` on every iteration, even when the scheduler queue is empty. This ensures death notifications propagate promptly rather than waiting until the next message delivery cycle.

### Supervisor actor

The supervisor is a regular actor whose behavior function interprets `MSG_CHILD_EXIT` messages and applies a restart policy. It is created via `supervisor_start()`, which takes:

- A **restart strategy** governing how failures propagate across siblings
- **Rate-limiting parameters** (`max_restarts` within `window_ms`)
- An array of **child specs** describing the children to manage

Three restart strategies are supported:

| Strategy | Behavior on child failure |
|----------|--------------------------|
| `STRATEGY_ONE_FOR_ONE` | Restart only the failed child |
| `STRATEGY_ONE_FOR_ALL` | Stop all children, then restart all |
| `STRATEGY_REST_FOR_ONE` | Stop the failed child and all children started after it, then restart those |

### Restart rate limiting

The supervisor tracks restart timestamps in a circular buffer of size `max_restarts`. Each restart records the current time; if the oldest entry in the buffer falls within `window_ms` of the current time, the rate limit has been exceeded and the supervisor itself stops (propagating the failure upward). This prevents infinite restart loops from consuming resources.

### Child specs and the factory pattern

Each `child_spec_t` describes one child:

```c
typedef struct {
    const char       *name;           /* for logging, may be NULL */
    actor_behavior_fn behavior;
    state_factory_fn  factory;        /* creates initial state; NULL = no state */
    void             *factory_arg;    /* passed to factory on each (re)start */
    void            (*free_state)(void *);
    size_t            mailbox_size;
    restart_type_t    restart_type;   /* PERMANENT, TRANSIENT, or TEMPORARY */
} child_spec_t;
```

The `factory` / `factory_arg` pair enables stateful restarts: the factory function is called each time the child needs to be (re)started, producing fresh state from a persistent argument. The `restart_type` controls per-child policy:

- **PERMANENT** — always restart, regardless of exit reason
- **TRANSIENT** — restart only on abnormal exit (EXIT_KILLED)
- **TEMPORARY** — never restart

Supervisors can be nested: a supervisor is just an actor, so it can appear as a child in another supervisor's child specs. This creates supervision trees where failures escalate through the hierarchy.

## Cross-node name registry

The local name registry (described above) maps string names to actor IDs on a single node. The cross-node extension broadcasts registry changes to all connected TCP peers, enabling location-transparent messaging across a cluster.

### Registration broadcast

When `actor_register_name(rt, name, id)` is called, the runtime broadcasts a `MSG_NAME_REGISTER` (0xFF000012) message to every connected TCP transport. The payload is a `name_register_payload_t`:

```c
typedef struct {
    char       name[64];
    actor_id_t actor_id;
} name_register_payload_t;
```

Remote nodes receiving this message add the entry to their local registry, associating the name with the remote actor ID.

### Deregistration on death

When an actor dies, `name_registry_deregister_actor()` removes all of its name bindings and broadcasts `MSG_NAME_UNREGISTER` (0xFF000013) to all TCP peers. The payload is a `name_unregister_payload_t` containing the 64-byte name. Remote nodes remove the corresponding entry from their local registries.

Both registration and unregistration messages use `source = dest = ACTOR_ID_INVALID` since they are infrastructure messages, not actor-to-actor communication.

### Message interception

In `poll_and_dispatch()`, incoming transport messages are checked by `handle_registry_msg()` before being delivered locally. If the message is a registry broadcast (`MSG_NAME_REGISTER` or `MSG_NAME_UNREGISTER`), it is handled directly (updating the local registry) and not forwarded to any actor's mailbox.

### Named send

`actor_send_named(rt, name, type, payload, size)` is a convenience function that performs a `actor_lookup()` followed by `actor_send()`. If the name resolves to a remote actor ID, the message is automatically routed through the appropriate transport. This provides location transparency: callers do not need to know whether the target actor is local or remote.

## WASM actor runtime

WASM actors allow untrusted or portable code to run within the actor model. The implementation embeds the WebAssembly Micro Runtime (WAMR), with the module loaded once and lightweight per-actor instances created on demand.

### WAMR embedding

The WAMR submodule lives at `third_party/wamr/` (pinned to WAMR-2.2.0). Build integration is controlled by the `ENABLE_WASM` CMake option (default ON), which defines `HAVE_WASM` when WAMR is available.

Module loading follows a two-phase pattern:

1. `wasm_runtime_load()` parses the bytecode once, producing a shared module object
2. `wasm_runtime_instantiate()` creates a per-actor instance with its own linear memory

The `wasm_factory_arg_t` struct owns a copy of the WASM bytecode buffer and the parsed module. It is created once and reused across actor restarts (especially relevant for supervised WASM actors).

### Module contract

Every WASM actor module must export:

```
handle_message(msg_type: i32, source: i64, payload_ptr: i32, payload_size: i32) -> i32
```

Returning nonzero keeps the actor alive; returning zero stops it (same semantics as the native behavior function).

The module imports the following host functions from the `"env"` namespace:

| Import | Signature | Purpose |
|--------|-----------|---------|
| `mk_send` | `(i64, i32, i32, i32) -> i32` | Send a message to another actor |
| `mk_self` | `() -> i64` | Get the current actor's ID |
| `mk_log` | `(i32, i32) -> void` | Log a message (pointer + length) |
| `mk_sleep_ms` | `(i32) -> void` | Sleep for N milliseconds (fiber only) |
| `mk_recv` | `(i32*, i32, i32, i32*) -> i32` | Receive next message (fiber only) |

### Sync execution path

When a WASM actor is spawned with `FIBER_STACK_NONE`, the behavior function calls `wasm_runtime_call_wasm()` directly. The `handle_message` export runs to completion for each message. This is the zero-overhead path: no fiber context is allocated, and blocking host functions (`mk_sleep_ms`, `mk_recv`) return an error if called.

### Fiber execution path

When a WASM actor is spawned with a fiber stack class (`FIBER_STACK_SMALL`, `FIBER_STACK_MEDIUM`, or `FIBER_STACK_LARGE`), the runtime allocates a fiber context that enables cooperative yielding from within WASM host functions.

On Linux, fibers use `ucontext_t` with `makecontext`/`swapcontext`. On ESP32 (Xtensa), fibers use `setjmp`/`longjmp` with a custom `_fiber_start_asm` assembly stub and `xthal_window_spill()` for register window flushing.

The behavior function handles three cases on each invocation:

1. **Resume a yielded fiber** — if the fiber is suspended (from a previous `mk_sleep_ms` or `mk_recv`), swap back into it
2. **Sync call** — if no fiber is active and no yield is needed, call `handle_message` directly
3. **Start a new fiber** — create a fresh fiber context and run `handle_message` inside it

The yielding host functions work as follows:

- **mk_sleep_ms(ms)**: Sets a one-shot timer for the given duration, then yields the fiber back to the behavior function. The behavior function returns, allowing the event loop to continue. When the timer fires, the next behavior invocation resumes the fiber at the point after the yield.
- **mk_recv(type_ptr, buf, size, out_size_ptr)**: Yields the fiber. When the next message arrives for this actor, the behavior function stashes the message in the actor's `recv_msg` slot and resumes the fiber. The host function then copies the message data into the WASM linear memory and returns.

A critical detail: `wasm_runtime_set_native_stack_boundary()` must be called to inform WAMR about the fiber stack boundaries, since the fiber runs on a separately allocated stack rather than the thread stack.

### Supervision integration

WASM actors integrate with the supervision tree through three function pointers:

- `wasm_actor_behavior` — the behavior function dispatched by the scheduler
- `wasm_actor_factory` — creates a fresh WASM instance from a `wasm_factory_arg_t`
- `wasm_actor_free` — destroys the WASM instance and fiber context

These can be used directly in a `child_spec_t`, with the `wasm_factory_arg_t` as the `factory_arg`. This means WASM actors can be supervised, restarted, and managed identically to native actors.

## ESP32 platform

The microkernel runs on ESP32-S3 (verified on TinyS3 and Waveshare boards) using ESP-IDF v5.5.3. The port reuses all core source files with no code duplication, relying on compile-time feature flags and a hardware abstraction layer for platform differences.

### Project structure

The ESP32 platform project lives at `platforms/esp32/` and references `src/*.c` via relative paths in CMake. Two ESP-IDF components provide the build:

- **microkernel** — compiles all core source files with platform-specific compile definitions
- **microkernel_hal** — platform-specific implementations: `timer_esp32.c`, `mk_socket_tls_esp.c`, `fiber_xtensa.c` / `fiber_xtensa.S`

### Portability adaptations

Several abstractions bridge the gap between Linux and ESP-IDF:

| Concern | Linux | ESP32 |
|---------|-------|-------|
| Timer close | `close(fd)` | `esp_timer_stop()` + `close(eventfd)` |
| Endian swap | `<endian.h>` htobe64/etc. | Portable inline byte-swap functions |
| Signal flags | `MSG_NOSIGNAL`, `MSG_DONTWAIT` | Guarded with `#ifdef` (not available on all targets) |
| eventfd | `<sys/eventfd.h>` | `<esp_vfs_eventfd.h>` with `eventfd(0, 0)` + `fcntl()` |
| TLS | OpenSSL (`HAVE_OPENSSL`) | mbedTLS/esp-tls (`HAVE_MBEDTLS`) |
| Fibers | `ucontext_t` makecontext/swapcontext | setjmp/longjmp + `_fiber_start_asm` assembly |

A unifying `HAVE_TLS` macro is defined when either `HAVE_OPENSSL` or `HAVE_MBEDTLS` is set. The HTTP state machine in `http_conn.c` uses `HAVE_TLS` to conditionally enable HTTPS and WSS support.

### Memory constraints

The ESP32-S3 has approximately 280 KB of usable heap. The `runtime_t` struct embeds fixed-size arrays for HTTP connections, timers, and other resources. On Linux, the default pool sizes are generous (e.g., 32 HTTP connections with 8 KB read buffers each). On ESP32, these are overridden via `target_compile_definitions` in the component CMakeLists.txt:

- `MAX_HTTP_CONNS=4`
- `MAX_TIMERS=8`
- Other pool sizes reduced proportionally

All pool size defines in `runtime.c` and `runtime_internal.h` are guarded with `#ifndef`, allowing platform-specific overrides without modifying core source files.

### TLS via mbedTLS

The ESP32 TLS socket (`mk_socket_tls_esp.c`) uses the `esp-tls` component backed by mbedTLS:

- CA certificate verification via `esp_crt_bundle_attach` (no manual cert management)
- Each TLS connection consumes approximately 20-25 KB of heap
- Task stack size of 16384 bytes is required for the mbedTLS handshake
- `esp_tls_conn_destroy()` closes the underlying fd internally -- the socket vtable must not double-close

### WASM on ESP32

WASM support uses the WAMR ESP-IDF component configured for the fast interpreter (no AOT, no WASI, no pthread support within WASM). WASM tests run inside a pthread rather than a FreeRTOS task because WAMR's ESP-IDF platform layer calls `pthread_self()`, which requires pthread context.

### Xtensa fibers

The ESP32 fiber implementation differs from the Linux ucontext approach:

- `setjmp`/`longjmp` save and restore the execution context
- A 5-instruction assembly stub (`_fiber_start_asm`) sets up the stack pointer and calls the fiber entry function
- `xthal_window_spill()` flushes the Xtensa register windows to the stack before context switches, ensuring that all live registers are saved

### Test coverage

18 smoke tests are verified on ESP32-S3 hardware, covering:

- Core actor runtime and timers
- TCP networking with WiFi
- HTTP GET, POST, WebSocket echo against a test server
- HTTPS GET and WSS echo against public servers (TLS)
- Multi-node distributed actors (two FreeRTOS tasks, TCP loopback)
- Cross-device distributed actors (UDP broadcast discovery + TCP ping-pong between two boards)
- HTTP, SSE, and WebSocket server self-tests via loopback
- WASM actor spawn, echo, sleep, receive, supervision, and named messaging
