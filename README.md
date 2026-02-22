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
- **Hierarchical namespace** -- `/`-prefixed path table, mount points, cross-node path sync
- **Dynamic node interconnection** -- `mount` protocol with hello handshake, automatic registry sync
- **Cloudflare integration** -- cf_proxy actor bridges local actors to Cloudflare Workers via WSS: KV storage, D1 SQL database, AI inference/embeddings, queue push; shell history persists across reboots
- **Capability advertisement** -- nodes report platform, features, and resource counts on request
- **Networking** -- TCP, UDP, DNS resolution via getaddrinfo
- **HTTP client/server** -- GET, POST, chunked transfer, request routing, response building
- **SSE client/server** -- event stream parsing and server push
- **WebSocket client/server** -- text/binary frames, ping/pong, upgrade handling, large frames up to 64KB with dynamic allocation
- **TLS** -- OpenSSL on Linux, mbedTLS on ESP32
- **Core services** -- timers (timerfd), FD watching, name registry, structured logging
- **WASM actors** -- spawn actors from `.wasm` bytecode via WAMR
- **WASM fibers** -- `mk_sleep_ms()` and `mk_recv()` for blocking-style concurrency in WASM
- **Hot code reload** -- atomic WASM module swap preserving names, mailbox, and supervisor state; shell `reload` command
- **Actor state persistence** -- file-backed binary save/load; WASM host functions `mk_save_state()`/`mk_load_state()` for cross-reload state preservation
- **Local KV storage** -- filesystem-backed key-value actor at `/node/storage/kv`, same interface as Cloudflare KV; works offline
- **Interactive shell** -- Rust WASM REPL over TCP; spawn/stop/reload actors, send messages, load `.wasm` from files or URLs, persistent command history via Cloudflare KV
- **ESP32 port** -- full feature parity on ESP32-S3 (Xtensa), ESP32-C6 and ESP32-P4 (RISC-V), including networking, TLS, WASM, hot reload, and interactive shell

## Building (Linux)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

35 tests pass. OpenSSL is detected automatically; if absent, TLS URLs return errors
while everything else works. WASM support requires clang for compiling `.wasm` test
modules and optionally `wat2wasm` (from wabt) for zero-linear-memory WAT modules.
The WAMR submodule auto-initializes on first build.

### CMake options

| Option | Default | Description |
|---|---|---|
| `ENABLE_WASM` | ON | WASM actor runtime via WAMR |
| `CF_PROXY_DEBUG` | OFF | Verbose cf_proxy and WebSocket frame logging |
| `BUILD_REALWORLD_TESTS` | OFF | Tests that hit the public network |
| `BUILD_BENCHMARKS` | OFF | HTTP and actor throughput benchmarks |

## Building (ESP32)

```bash
cd platforms/esp32
cp main/wifi_config.h.example main/wifi_config.h  # edit with WiFi credentials
idf.py set-target esp32s3  # or esp32c6, esp32p4
idf.py build flash monitor
```

Runs 18 smoke tests on boot (6 on chips without WiFi). Tested on:

- **ESP32-S3** (Xtensa) -- TinyS3, Waveshare -- 18 tests
- **ESP32-C6** (RISC-V) -- ESP32-C6-DevKit, C6-Zero -- 18 tests
- **ESP32-P4** (RISC-V dual-core) -- ESP32-P4 -- 6 tests (no WiFi radio)

The build system auto-selects the correct fiber implementation (Xtensa register
window spill vs RISC-V direct stack switch) and compiles out WiFi-dependent
tests on chips without a radio.

**ESP-IDF version notes:** Use ESP-IDF v5.5+ for S3 and C6 targets. The P4
requires v5.4.x for early silicon (rev 1.x) -- v5.5 generates instructions
unsupported on pre-production P4 chips.

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

### WASM actor in Zig

```zig
// actor.zig -- compile with:
//   zig build-lib -target wasm32-freestanding -dynamic -O ReleaseSmall actor.zig
extern "env" fn mk_send(dest: i64, msg_type: i32, payload: [*]const u8, size: i32) i32;
extern "env" fn mk_sleep_ms(ms: i32) i32;

const MSG_PING: i32 = 200;
const MSG_PONG: i32 = 201;

export fn handle_message(
    msg_type: i32,
    source: i64,
    payload: [*]const u8,
    payload_size: i32,
) i32 {
    switch (msg_type) {
        MSG_PING => {
            _ = mk_sleep_ms(100);
            _ = mk_send(source, MSG_PONG, payload, payload_size);
            return 1;
        },
        0 => return 0,
        else => return 1,
    }
}
```

### WASM actor in Go

Requires [TinyGo](https://tinygo.org/) for bare WASM output (no WASI):

```go
// actor.go -- compile with:
//   tinygo build -o actor.wasm -target wasm -scheduler=none -no-debug .
package main

//go:wasmimport env mk_send
func mk_send(dest int64, msgType int32, payload *byte, size int32) int32

//go:wasmimport env mk_sleep_ms
func mk_sleep_ms(ms int32) int32

const (
	msgPing = 200
	msgPong = 201
)

//export handle_message
func handleMessage(msgType int32, source int64, payload *byte, payloadSize int32) int32 {
	switch msgType {
	case msgPing:
		mk_sleep_ms(100)
		mk_send(source, msgPong, payload, payloadSize)
		return 1
	case 0:
		return 0
	default:
		return 1
	}
}

func main() {}
```

### Interactive WASM shell

The microkernel includes an interactive shell written in Rust, compiled to WASM,
and running as an actor inside the runtime. On ESP32 it listens on TCP port 23
after WiFi connects:

```
$ nc 192.168.1.135 23
╔════════════════════════════════════╗
║  Entrained OS                      ║
║  microkernel WASM shell v0.2       ║
║  Type 'help' for commands          ║
╚════════════════════════════════════╝
mk> load http://192.168.1.235:8080/files/build/tests/echo.wasm
Downloaded 1031 bytes
Loading...
Spawned actor 4294967299 as 'echo'
mk> call echo 200 hello
[reply] type=201 from=4294967299 size=5 "hello"
mk> ai Explain the actor model in one sentence
The actor model is a concurrent computing approach where process...
mk> stop echo
Stopped actor 4294967299
mk> exit
Goodbye.
```

The shell itself fits in a single 64KB WASM page (`#![no_std]`, static buffers,
no allocator). A C console actor bridges TCP I/O into the actor message loop.
On Linux, the same shell binary runs with stdin/stdout via `tools/shell/`.

Commands: `help`, `list`, `ls /prefix`, `self`, `whoami`, `load <path-or-url>`,
`reload <name> <path-or-url>`, `send <name-or-id> <type> [payload]`,
`call <name-or-id> <type> [payload]`, `stop <name-or-id>`,
`register <name>`, `lookup <name>`, `mount <host>[:<port>]`,
`caps [target]`, `ai <prompt>`, `embed <text>`, `sql <query>`,
`queue <message>`, `history [clear]`, `exit`

Loaded actors are auto-registered by filename (`echo.wasm` becomes `echo`; duplicates
get `echo_1`, `echo_2`, etc.). The `call` command sends a message and waits up to 5
seconds for a reply. Unsolicited messages from actors are printed between prompts.

Building the shell WASM module:

```bash
cd tools/shell/shell_wasm
cargo build --target wasm32-unknown-unknown --release
```

The same binary runs on both Linux and ESP32 -- a single 64KB WASM page via
`.cargo/config.toml` (16KB stack, 32KB file buffer).

### Cloudflare cloud services

The `cf_proxy` actor maintains a persistent WSS connection to a Cloudflare Worker,
giving every node transparent access to cloud services through virtual namespace
paths. Local actors send simple `key=value\n` payloads; the proxy translates to
JSON over WebSocket; the Worker handles server-side key prefixing by node identity.

| Service | Virtual path | Cloudflare binding |
|---|---|---|
| Key-value store | `/node/storage/kv` | Workers KV |
| SQL database | `/node/storage/db` | D1 |
| Message queue | `/node/queue/default` | Queues |
| Text inference | `/node/ai/infer` | Workers AI |
| Embeddings | `/node/ai/embed` | Workers AI |

Cloudflare is the guaranteed floor -- always reachable. Virtual paths resolve to
the best available implementation: local flash/RAM > mounted peer > Cloudflare.
Actors address services by name, never by backend.

The shell's `history` command uses KV storage to persist command history across
reboots -- the acceptance test for the integration. Configuration:

```bash
# Linux: environment variables
MK_CF_URL="wss://your-worker.workers.dev/ws" MK_CF_TOKEN="secret" ./mk-shell

# ESP32: create platforms/esp32/main/cf_config.h (gitignored)
#define CF_PROXY_URL   "wss://your-worker.workers.dev/ws"
#define CF_PROXY_TOKEN "secret"
```

Deploy the Worker:

```bash
cd platforms/cloudflare/worker
npm install
npx wrangler deploy
npx wrangler secret put AUTH_TOKEN
```

## Project structure

```
include/microkernel/    Public headers (types, runtime, actor, message, services,
                        transport, http, mk_socket, supervision, wasm_actor,
                        namespace, cf_proxy)
src/                    Implementation (runtime, actors, transports, HTTP state
                        machine, supervision, wasm_actor, hot reload, namespace,
                        cf_proxy, local_kv, state_persist, wire format, utilities)
tests/                  35 unit/integration tests + realworld tests + benchmarks
tests/wasm_modules/     WASM test module source (C and WAT)
tools/shell/            Interactive shell (C driver + Rust WASM REPL)
third_party/wamr/       WAMR submodule (pinned to WAMR-2.2.0)
platforms/esp32/        ESP-IDF project (components: microkernel, microkernel_hal)
platforms/cloudflare/   Cloudflare Worker (mk-proxy) for cloud KV, D1, AI
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
