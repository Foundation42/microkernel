# Microkernel

Actor-based microkernel runtime in C for Linux and ESP32. Erlang-style supervision
trees, Go-style blocking concurrency via WASM fibers, and sandboxed actor isolation
via WAMR. All communication is asynchronous message passing over a cooperative
scheduler with integrated I/O polling.

## Features

- **Actor model** -- message passing, mailboxes, cooperative round-robin scheduling
- **Supervision trees** -- one-for-one, one-for-all, rest-for-one restart strategies with rate limiting
- **Multi-node IPC** -- Unix domain sockets and TCP transport with binary wire protocol
- **Cross-node name registry** -- location-transparent `actor_send_named()` across nodes
- **Networking** -- TCP, UDP, DNS resolution via getaddrinfo
- **HTTP client/server** -- GET, POST, chunked transfer, request routing, response building
- **SSE client/server** -- event stream parsing and server push
- **WebSocket client/server** -- text/binary frames, ping/pong, upgrade handling
- **TLS** -- OpenSSL on Linux, mbedTLS on ESP32
- **Core services** -- timers (timerfd), FD watching, name registry, structured logging
- **WASM actors** -- spawn actors from `.wasm` bytecode via WAMR
- **WASM fibers** -- `mk_sleep_ms()` and `mk_recv()` for blocking-style concurrency in WASM
- **ESP32 port** -- full feature parity on ESP32-S3 (Xtensa) and ESP32-C6 (RISC-V), including networking, TLS, and WASM

## Building (Linux)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

28 tests pass. OpenSSL is detected automatically; if absent, TLS URLs return errors
while everything else works. WASM support requires clang for compiling `.wasm` test
modules. The WAMR submodule auto-initializes on first build.

### CMake options

| Option | Default | Description |
|---|---|---|
| `ENABLE_WASM` | ON | WASM actor runtime via WAMR |
| `BUILD_REALWORLD_TESTS` | OFF | Tests that hit the public network |
| `BUILD_BENCHMARKS` | OFF | HTTP and actor throughput benchmarks |

## Building (ESP32)

```bash
cd platforms/esp32
cp main/wifi_config.h.example main/wifi_config.h  # edit with WiFi credentials
idf.py set-target esp32s3  # or esp32c6
idf.py build flash monitor
```

Requires ESP-IDF v5.5+. Runs 18 smoke tests on boot. Tested on:

- **ESP32-S3** (Xtensa) -- TinyS3, Waveshare
- **ESP32-C6** (RISC-V) -- ESP32-C6-DevKit

The build system auto-selects the correct fiber implementation (Xtensa register
window spill vs RISC-V direct stack switch) based on the target architecture.

## Quick example

Ping-pong between two actors:

```c
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"

typedef struct { actor_id_t peer; int count; } state_t;

static bool ping(runtime_t *rt, actor_t *self, message_t *msg, void *s) {
    (void)self;
    state_t *st = s;
    if (msg->type == 1 && ++st->count < 5)
        actor_send(rt, st->peer, 1, NULL, 0);
    else { actor_stop(rt, st->peer); return false; }
    return true;
}

static bool pong(runtime_t *rt, actor_t *self, message_t *msg, void *s) {
    (void)self; (void)s;
    if (msg->type == 1) actor_send(rt, msg->source, 1, NULL, 0);
    return true;
}

int main(void) {
    runtime_t *rt = runtime_init(1, 16);
    state_t ps = {0}, qs = {0};
    actor_id_t a = actor_spawn(rt, ping, &ps, NULL, 16);
    actor_id_t b = actor_spawn(rt, pong, &qs, NULL, 16);
    ps.peer = b; qs.peer = a;
    actor_send(rt, a, 1, NULL, 0);
    runtime_run(rt);
    runtime_destroy(rt);
}
```

### WASM actor in Rust

Actors can be written in any language that compiles to WASM. Here is a Rust actor
that echoes messages back to the sender, with a sleep to demonstrate fiber-based
cooperative yielding:

```rust
// actor.rs -- compile with: rustup target add wasm32-unknown-unknown
//   cargo build --target wasm32-unknown-unknown --release
#![no_std]
#![no_main]

extern "C" {
    fn mk_send(dest: i64, msg_type: i32, payload: *const u8, size: i32) -> i32;
    fn mk_sleep_ms(ms: i32) -> i32;
}

const MSG_PING: i32 = 200;
const MSG_PONG: i32 = 201;

#[no_mangle]
pub extern "C" fn handle_message(
    msg_type: i32,
    source: i64,
    payload: *const u8,
    payload_size: i32,
) -> i32 {
    match msg_type {
        MSG_PING => unsafe {
            // Yield to the runtime for 100ms -- other actors keep running
            mk_sleep_ms(100);
            // Resumed after sleep; echo the payload back
            mk_send(source, MSG_PONG, payload, payload_size);
            1 // keep alive
        },
        0 => 0, // stop
        _ => 1, // ignore
    }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }
```

Spawn it from C (or from another WASM actor):

```c
wasm_actors_init();
runtime_t *rt = runtime_init(1, 16);
actor_id_t wasm = actor_spawn_wasm_file(rt, "actor.wasm", 16,
                                          WASM_DEFAULT_STACK_SIZE,
                                          WASM_DEFAULT_HEAP_SIZE,
                                          FIBER_STACK_SMALL);
actor_send(rt, wasm, 200, "hello", 5);
runtime_run(rt);
```

The same `.wasm` binary runs on both Linux and ESP32 without recompilation.

## Project structure

```
include/microkernel/    Public headers (types, runtime, actor, message, services,
                        transport, http, mk_socket, supervision, wasm_actor)
src/                    Implementation (runtime, actors, transports, HTTP state
                        machine, supervision, wasm_actor, wire format, utilities)
tests/                  28 unit/integration tests + realworld tests + benchmarks
tests/wasm_modules/     WASM test module source (echo.c)
third_party/wamr/       WAMR submodule (pinned to WAMR-2.2.0)
platforms/esp32/        ESP-IDF project (components: microkernel, microkernel_hal)
docs/                   Architecture, API reference, examples, development guide
```

## Dependencies

**Required:** libc, pthreads.

**Optional:** OpenSSL (TLS on Linux), WAMR (WASM actors, default ON via submodule).

**ESP32:** ESP-IDF v5.5+ (includes mbedTLS, lwIP, FreeRTOS).

## Documentation

- [Architecture](docs/architecture.md) -- system design, event loop, state machines
- [API Reference](docs/api.md) -- public functions and types
- [Examples](docs/examples.md) -- runnable code for each feature
- [Development Guide](docs/development.md) -- build system, testing, contributing

## License

BSD 3-Clause (non-commercial). Commercial license available -- contact
chris@foundation42.org.
