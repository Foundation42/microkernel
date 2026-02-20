# Development Guide

## Project structure

```
microkernel/
├── CMakeLists.txt              Top-level CMake (project declaration, subdirs)
├── README.md                   Project overview
├── docs/
│   ├── Spec.md                 Original design spec
│   ├── architecture.md         System architecture
│   ├── api.md                  API reference
│   ├── examples.md             Usage examples
│   └── development.md          This file
├── include/microkernel/        Public headers (installed/exposed to consumers)
│   ├── types.h                 Core typedefs and constants
│   ├── message.h               Message struct and create/destroy
│   ├── mailbox.h               Ring buffer mailbox
│   ├── actor.h                 Actor struct and lifecycle
│   ├── scheduler.h             FIFO ready queue
│   ├── runtime.h               Runtime init, messaging, event loop
│   ├── services.h              Timers, FD watching, names, logging
│   ├── http.h                  HTTP/SSE/WS client + server APIs
│   ├── transport.h             Transport vtable
│   ├── transport_unix.h        Unix domain socket transport
│   ├── transport_tcp.h         TCP transport
│   ├── transport_udp.h         UDP transport
│   ├── wire.h                  Wire format serialization
│   ├── mk_socket.h             Socket abstraction (TCP, TLS)
│   ├── supervision.h           Supervision trees and restart strategies
│   └── wasm_actor.h            WASM actor runtime API
├── src/                        Implementation files
│   ├── CMakeLists.txt          Library build definition
│   ├── runtime_internal.h      Private types shared between src/ files
│   ├── url_parse.h/c           URL parser (private)
│   ├── sha1.h/c                SHA-1 for WebSocket (private)
│   ├── base64.h/c              Base64 for WebSocket (private)
│   ├── ws_frame.h/c            WebSocket frame codec (private)
│   ├── supervision.c           Supervisor behavior and helpers
│   ├── wasm_actor.c            WASM actor runtime via WAMR
│   └── ...                     All .c implementation files
├── third_party/
│   └── wamr/                   WAMR submodule (pinned to WAMR-2.2.0)
├── platforms/
│   └── esp32/                  ESP-IDF project with components
│       ├── main/               Application entry point and smoke tests
│       └── components/         microkernel + microkernel_hal components
└── tests/                      All test files
    ├── CMakeLists.txt          Test build definitions
    ├── test_framework.h        Assertion and test runner macros
    └── wasm_modules/echo.c     WASM test module source
```

**Key convention**: Public headers live in `include/microkernel/`. Private headers used only within `src/` stay in `src/` and are accessed via the private include path.

## Build system

### CMake structure

The top-level `CMakeLists.txt` declares the project and adds subdirectories:

```cmake
cmake_minimum_required(VERSION 3.16)
project(microkernel C)
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
```

`src/CMakeLists.txt` builds the static library:
- Lists all `.c` files explicitly (no globs)
- Sets `include/` as PUBLIC include directory
- Sets `src/` as PRIVATE include directory (for internal headers)
- Links pthreads
- Optionally detects and links OpenSSL

### Adding a new source file

1. Create `src/my_feature.c`
2. Add `my_feature.c` to the `add_library(microkernel STATIC ...)` list in `src/CMakeLists.txt`
3. If it has a public API, create `include/microkernel/my_feature.h`
4. If it only has internal APIs, put the header in `src/`

### Optional features

| CMake variable | Default | Effect |
|---------------|---------|--------|
| `OpenSSL_FOUND` | auto-detected | Adds TLS socket, defines `HAVE_OPENSSL` |
| `ENABLE_WASM` | ON | WASM actor support via WAMR. Requires clang for compiling `.wasm` test modules. |
| `BUILD_REALWORLD_TESTS` | OFF | Builds tests that require network access |
| `BUILD_BENCHMARKS` | OFF | Builds performance benchmarks |

**WASM notes**: The WAMR submodule auto-initializes on first build. The vmlib target is built with `-fno-sanitize=address,undefined` to avoid WAMR internal ASan/UBSan issues.

### Build commands

```bash
# Standard debug build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# With everything enabled
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DBUILD_REALWORLD_TESTS=ON \
         -DBUILD_BENCHMARKS=ON
make -j$(nproc)

# Release build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Testing

### Test framework

Tests use a minimal framework defined in `tests/test_framework.h`:

```c
ASSERT(condition)             // Fail with file:line if false
ASSERT_EQ(a, b)              // Fail if a != b
ASSERT_NE(a, b)              // Fail if a == b
ASSERT_NULL(p)               // Fail if p != NULL
ASSERT_NOT_NULL(p)           // Fail if p == NULL
RUN_TEST(test_function)      // Run a test, track pass/fail
TEST_REPORT()                // Print summary, return exit code
```

Each test file has a `main()` that calls `RUN_TEST` for each test function, then `TEST_REPORT()`.

### Test registration

The `add_microkernel_test(name)` macro in `tests/CMakeLists.txt` does three things:
1. Compiles `tests/{name}.c` into an executable
2. Links it against the `microkernel` library
3. Registers it with CTest via `add_test()`

To add a new test:
1. Create `tests/test_my_feature.c` with test functions and a `main()`
2. Add `add_microkernel_test(test_my_feature)` to `tests/CMakeLists.txt`

### Running tests

```bash
cd build

# Run all tests
ctest --output-on-failure

# Run a specific test
ctest -R test_http --output-on-failure

# Run with verbose output
ctest -V

# Run real-world tests (requires network + cmake flag)
cmake .. -DBUILD_REALWORLD_TESTS=ON && make
./tests/test_http_realworld
./tests/test_ws_realworld
./tests/test_https_realworld
```

### Port assignments

Network tests use fixed ports to avoid conflicts when running in parallel. If adding a test that listens on a port, pick the next available one:

| Test | Port(s) |
|------|---------|
| test_transport_tcp | 19876 |
| test_multinode_tcp | 19877 |
| test_transport_udp | 19878 |
| test_mk_socket | 19879 |
| test_http | 19880 |
| test_sse | 19881 |
| test_websocket | 19882 |
| test_http_runtime | 19883 |
| test_http_server | 19884–19888 |
| test_sse_server | 19885–19887 |
| test_ws_server | 19886–19888 |
| bench_http | 19890 |
| test_distributed_registry | 19891–19894 |
| ESP32 TCP test | 19900 |
| ESP32 multi-node | 19901 |
| ESP32 discovery (UDP broadcast) | 19902 |
| ESP32 cross-device TCP | 19903 |
| ESP32 HTTP server GET | 19904 |
| ESP32 HTTP server POST | 19905 |
| ESP32 SSE server | 19906 |
| ESP32 WS server | 19907 |
| *Next available* | *19908+* |

### Test patterns

**Fork-based server tests**: Most network tests fork a child process to run a server, then test against it from the parent. Pattern:

```c
pid_t pid = fork();
if (pid == 0) {
    // Child: run server (often raw socket or actor-based)
    _exit(0);
}
// Parent: connect and test
usleep(50000);  // let server start
// ... test code ...
kill(pid, SIGTERM);
waitpid(pid, NULL, 0);
```

**Actor integration tests**: Test the full actor → runtime → service loop:

```c
runtime_t *rt = runtime_init(1, 64);
my_state_t st = {0};
actor_id_t id = actor_spawn(rt, my_behavior, &st, NULL, 16);
actor_send(rt, id, 0, NULL, 0);  // trigger initial action
runtime_run(rt);
ASSERT(st.expected_result);
runtime_destroy(rt);
```

## Adding new features

### New transport

Follow the transport vtable pattern:

1. Define in `include/microkernel/transport_foo.h`:
   ```c
   transport_t *transport_foo_listen(...);
   transport_t *transport_foo_connect(...);
   ```

2. Implement in `src/transport_foo.c`:
   - Define an `impl` struct with transport-specific state
   - Implement `send`, `recv`, `is_connected`, `destroy`
   - Set the `fd` field for poll integration
   - Use `wire_serialize_net()`/`wire_deserialize_net()` for cross-machine transports

3. Add to `src/CMakeLists.txt`

4. Write tests in `tests/test_transport_foo.c`

### New message type

1. Add a `#define MSG_MY_TYPE ((msg_type_t)0xFF0000XX)` to `services.h` (next available value is `0xFF000014`+)
2. Define the payload struct
3. Add an inline accessor if the payload has variable-length trailing data
4. Deliver via `runtime_deliver_msg()` from the producing module

### New runtime service

1. Create the header and implementation files
2. Add accessor functions to `runtime_internal.h` if the service needs runtime state
3. Add the service's fds to the `poll_and_dispatch()` function in `runtime.c`
4. Handle the poll source type in the dispatch switch

## Phase history

| Phase | What it added |
|-------|--------------|
| **1** | Core actor runtime: message, mailbox, actor, scheduler, runtime |
| **2** | Multi-node IPC via Unix domain sockets: wire format, transport vtable |
| **2.5** | Runtime services: timers (timerfd), FD watching, name registry, logging actor |
| **3** | TCP and UDP transports with network byte order wire format |
| **3.5** | HTTP, SSE, and WebSocket client: socket vtable, URL parser, HTTP state machine |
| **4** | DNS resolution (getaddrinfo in mk_socket), real-world tests, benchmarks |
| **5** | Server-side HTTP, SSE, WebSocket: listeners, accept, request routing |
| **5.5** | TLS via OpenSSL: mk_socket_tls, https:// and wss:// support |
| **6** | Core runtime on ESP32 -- actors + timers verified on TinyS3 (ESP32-S3) |
| **6.5** | TCP networking on ESP32 -- WiFi init + TCP loopback |
| **7** | HTTP/WS client on ESP32 -- 6 smoke tests against dev machine server |
| **7.5** | TLS via mbedTLS on ESP32 -- HTTPS GET + WSS echo (8 smoke tests) |
| **8** | Multi-node distributed actors on ESP32 -- TCP loopback ping-pong (9 smoke tests) |
| **8.5** | Cross-device distributed actors -- UDP broadcast discovery + TCP ping-pong (10 smoke tests) |
| **9** | ESP32 HTTP/SSE/WS server self-tests via loopback (14 smoke tests) |
| **10** | Supervision trees -- 26 ctest (death notification, restart strategies, restart limits, nested supervisors) |
| **11** | Cross-node registry and location transparency -- 27 ctest (register broadcast, remote send_named, deregister on death) |
| **12** | WASM actor runtime via WAMR -- 28 ctest (spawn, echo, mk_self, stop, supervision, named) |
| **13** | WASM fiber support via ucontext -- 28 ctest (WASM test: 9 sub-tests with fiber_sleep, fiber_recv) |
| **13b** | WASM + Xtensa fibers on ESP32 -- 18 ESP32 smoke tests (wasm_spawn, echo, fiber_sleep, fiber_recv) |

Each phase was additive — earlier APIs remain stable and backward-compatible.

## ESP32 development

The ESP32 port lives in `platforms/esp32/` and references the core `src/*.c` files via relative paths (no code duplication). It builds as an ESP-IDF project with two components: `microkernel` (core sources) and `microkernel_hal` (platform-specific timer, TLS, and fiber implementations).

### Requirements

- ESP-IDF v5.5 or later
- A supported board (tested on TinyS3 ESP32-S3 and Waveshare ESP32-S3)

### Building and flashing

```bash
cd platforms/esp32
idf.py build flash monitor
```

### WiFi configuration

Create `platforms/esp32/main/wifi_config.h` (gitignored) with your network credentials:

```c
#define WIFI_SSID "YourSSID"
#define WIFI_PASS "YourPassword"
```

### Test server

Some smoke tests (HTTP, WebSocket, TLS) require a test server running on a dev machine:

```bash
python3 platforms/esp32/test_server.py
```

This starts an HTTP server on port 8080 and a WebSocket echo server on port 8081. Set `TEST_SERVER_IP` in `wifi_config.h` to your dev machine's IP address.

### Smoke tests

The ESP32 build runs 18 smoke tests. Tests 1--14 cover core runtime, networking, and server functionality. Tests 15--18 are WASM actor tests (spawn, echo, fiber_sleep, fiber_recv) that run in a pthread to provide the stack space required by WAMR.
