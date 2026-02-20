# Actor-Based Microkernel Runtime Specification

Christian Beaumont - February 2026

> **Note:** This is the original design specification written before implementation
> began. It is retained as a historical reference. The actual implementation diverges
> in several areas (e.g., no separate router module, different transport API signatures,
> additional phases beyond the original five). See [architecture.md](architecture.md)
> and [api.md](api.md) for current documentation.

## 1. Overview & Goals

### What We're Building
A lightweight, capability-based actor microkernel runtime designed for edge compute devices, initially targeting ESP32-P4 but starting development on Linux for rapid iteration.

### Core Philosophy
- **Actor model**: All computation happens in isolated actors communicating via asynchronous messages
- **Capability security**: Actors can only interact with resources they have explicit references to
- **Transport agnostic**: Local and remote actors use identical APIs
- **Zero-copy where possible**: Minimize memory allocation and copying in critical paths
- **Predictable real-time performance**: Bounded latency for time-critical operations

### Primary Use Case (Phase 1)
MIDI CoPilot - intelligent MIDI processing on ESP32-P4 hardware, but the runtime is designed as general-purpose infrastructure for any edge compute scenario.

### Long-term Vision
- WASM modules as portable, sandboxed actor behaviors
- Seamless cloud-edge integration (Cloudflare Workers ↔ ESP32 devices)
- Multi-device topologies (distributed actor systems across hardware)
- Foundation for Social Magnetics displays and sensor networks

## 2. Architecture

### Core Concepts

**Node**: A runtime instance representing a logical compute unit
- In Linux dev mode: a process
- In production: a CPU core, a separate chip (C6), or a distinct device

**Actor**: The fundamental unit of computation
- Has a unique ID within the system
- Owns private state
- Processes messages sequentially from its mailbox
- Can send messages to other actors (if it has their actor_id)
- Can spawn new actors

**Message**: Asynchronous communication between actors
- Contains source, destination, type, and payload
- Delivered exactly once (at-most-once semantics acceptable for phase 1)
- No guaranteed ordering between different sender-receiver pairs

**Transport**: Abstraction for moving messages between nodes
- Local (same process): direct mailbox enqueue
- IPC (same machine): Unix domain sockets or shared memory
- Network (different machines): TCP/WebSockets
- Hardware (ESP32): SPI, UART, I2C

**Router**: Message routing logic within a node
- Maintains mapping of actor_id → local mailbox or remote node
- Forwards messages to appropriate transport

### System Topology

```
┌─────────────────────────────────────────────────┐
│ Board Instance (e.g., MIDI CoPilot Device #1)  │
│                                                 │
│  ┌──────────────┐         ┌──────────────┐    │
│  │   Node: P4-0 │────────▶│   Node: P4-1 │    │
│  │   (Core 0)   │◀────────│   (Core 1)   │    │
│  └──────────────┘         └──────────────┘    │
│         │                         │            │
│         │    ┌──────────────┐    │            │
│         └───▶│   Node: C6   │◀───┘            │
│              │  (Companion)  │                 │
│              └──────────────┘                  │
└─────────────────────────────────────────────────┘
                      │
                      │ Network Transport
                      │
┌─────────────────────────────────────────────────┐
│ Another Board Instance or Cloud Node            │
└─────────────────────────────────────────────────┘
```

## 3. Core Components

### 3.1 Message

```c
typedef uint64_t actor_id_t;  // Globally unique actor identifier
typedef uint32_t node_id_t;   // Node identifier within topology
typedef uint32_t msg_type_t;  // Application-defined message type

typedef struct message {
    actor_id_t source;      // Sending actor
    actor_id_t dest;        // Destination actor
    msg_type_t type;        // Message type/tag
    size_t payload_size;    // Size of payload in bytes
    void *payload;          // Pointer to payload data
    
    // Internal routing metadata
    node_id_t source_node;
    node_id_t dest_node;
    
    // Memory management
    void (*free_payload)(void *); // Custom deallocator, NULL for static data
} message_t;
```

**Design Notes:**
- `payload` can be inline (small messages) or pointer to heap (large messages)
- Zero-copy: payload ownership transfers with message
- `free_payload` allows custom memory management (pools, arenas, etc.)

### 3.2 Mailbox

Lock-free single-producer-single-consumer ring buffer for message delivery.

```c
typedef struct mailbox {
    message_t **messages;   // Ring buffer of message pointers
    size_t capacity;        // Fixed size (power of 2)
    atomic_size_t head;     // Producer index
    atomic_size_t tail;     // Consumer index
} mailbox_t;

// API
mailbox_t* mailbox_create(size_t capacity);
void mailbox_destroy(mailbox_t *mb);
bool mailbox_enqueue(mailbox_t *mb, message_t *msg);  // Returns false if full
message_t* mailbox_dequeue(mailbox_t *mb);  // Returns NULL if empty
```

**Design Notes:**
- Lock-free for single producer, single consumer
- Fixed capacity to bound memory usage
- Back-pressure: enqueue fails if full (caller must handle)

### 3.3 Actor

```c
typedef enum {
    ACTOR_IDLE,      // No messages, not scheduled
    ACTOR_READY,     // Has messages, waiting to run
    ACTOR_RUNNING,   // Currently executing
    ACTOR_STOPPED    // Terminated, awaiting cleanup
} actor_state_t;

// Actor behavior function
// Returns: true to continue, false to stop actor
typedef bool (*actor_behavior_fn)(actor_t *self, message_t *msg, void *state);

typedef struct actor {
    actor_id_t id;
    node_id_t node_id;           // Which node owns this actor
    actor_state_t state;
    
    mailbox_t *mailbox;
    actor_behavior_fn behavior;  // Message handler
    void *state;                 // Private actor state
    
    // Scheduling
    struct actor *next;          // For ready queue
    uint32_t priority;           // For future priority scheduling
} actor_t;
```

**Design Notes:**
- `behavior` function processes one message at a time
- `state` is opaque to runtime, managed by actor implementation
- Actor runs until mailbox empty, then yields

### 3.4 Runtime

```c
typedef struct runtime runtime_t;

// Initialization
runtime_t* runtime_init(node_id_t my_node_id, size_t max_actors);
void runtime_destroy(runtime_t *rt);

// Actor management
actor_id_t actor_spawn(
    runtime_t *rt,
    actor_behavior_fn behavior,
    void *initial_state,
    size_t mailbox_size
);
void actor_stop(runtime_t *rt, actor_id_t actor_id);

// Message sending
bool actor_send(
    runtime_t *rt,
    actor_id_t dest,
    msg_type_t type,
    void *payload,
    size_t payload_size,
    void (*free_payload)(void *)
);

// Transport management
void runtime_add_transport(
    runtime_t *rt,
    node_id_t peer_node,
    transport_t *transport
);

// Execution
void runtime_run(runtime_t *rt);  // Blocking event loop
void runtime_step(runtime_t *rt); // Single scheduling iteration
void runtime_stop(runtime_t *rt); // Signal shutdown
```

**Design Notes:**
- `runtime_run()` is the main event loop (blocking)
- `runtime_step()` allows external integration (testing, custom loops)
- Actor IDs are globally unique (node_id embedded in actor_id)

### 3.5 Transport

```c
typedef struct transport {
    node_id_t peer_node;  // Which node this transport connects to
    
    // Send a message to peer node (non-blocking)
    bool (*send)(struct transport *self, message_t *msg);
    
    // Receive a message from peer node (non-blocking, NULL if none)
    message_t* (*recv)(struct transport *self);
    
    // Check if transport is connected/healthy
    bool (*is_connected)(struct transport *self);
    
    // Cleanup
    void (*destroy)(struct transport *self);
    
    // Opaque transport-specific data
    void *impl;
} transport_t;
```

**Concrete Implementations:**
- `transport_local`: Direct function call (same process)
- `transport_unix`: Unix domain sockets (IPC)
- `transport_tcp`: TCP sockets (network)
- `transport_websocket`: WebSocket client (cloud connectivity)
- Future: `transport_spi`, `transport_uart` for ESP32 inter-chip

### 3.6 Router

```c
typedef struct router {
    runtime_t *runtime;
    
    // Maps actor_id → local actor or remote node_id
    // Implementation: hash table or radix tree
    void *routing_table;
    
    // List of transports to other nodes
    transport_t **transports;
    size_t num_transports;
} router_t;

// Internal API (used by runtime)
void router_route_message(router_t *router, message_t *msg);
void router_register_local_actor(router_t *router, actor_id_t id, actor_t *actor);
void router_unregister_actor(router_t *router, actor_id_t id);
```

**Design Notes:**
- Routing decision: local actor or remote node?
- Local: direct enqueue to actor's mailbox
- Remote: serialize and send via appropriate transport
- Routing table updated when actors spawn/stop

### 3.7 Scheduler

Simple cooperative scheduler (phase 1).

```c
typedef struct scheduler {
    actor_t *ready_queue_head;  // Singly-linked list
    actor_t *ready_queue_tail;
    size_t ready_count;
} scheduler_t;

// Schedule an actor (has pending messages)
void scheduler_enqueue(scheduler_t *sched, actor_t *actor);

// Get next actor to run
actor_t* scheduler_dequeue(scheduler_t *sched);
```

**Scheduling Policy (Phase 1):**
- Round-robin: process actors in FIFO order
- Each actor processes messages until mailbox empty
- Cooperative: actors voluntarily yield

**Future Enhancements:**
- Priority levels
- Time slicing (preemption)
- Deadline scheduling for real-time tasks

## 4. Message Flow

### Local Message (same node)
```
actor_send(rt, dest_id, ...)
  └─> router_route_message()
      └─> [lookup dest_id → local actor]
          └─> mailbox_enqueue(actor->mailbox, msg)
              └─> scheduler_enqueue(scheduler, actor)
                  └─> [actor now in ready queue]
```

### Remote Message (different node)
```
actor_send(rt, dest_id, ...)
  └─> router_route_message()
      └─> [lookup dest_id → remote node X]
          └─> transport_send(transports[X], msg)
              └─> [message serialized and sent over wire]

[On receiving node]
transport_recv() returns message
  └─> router_route_message()
      └─> [lookup dest_id → local actor]
          └─> mailbox_enqueue(actor->mailbox, msg)
```

## 5. Development Phases

### Phase 1: Core Runtime (Week 1)
**Goal:** Basic actor system working on Linux with local actors only.

**Deliverables:**
- Message, mailbox, actor data structures
- Scheduler (round-robin)
- Runtime initialization and event loop
- Actor spawn, send, receive primitives
- Simple test: spawn 2 actors, ping-pong messages

**Success Criteria:**
- Can spawn actors
- Actors can send/receive messages
- Scheduler runs actors correctly
- No memory leaks (valgrind clean)

### Phase 2: Multi-Node IPC (Week 2)
**Goal:** Multiple runtime instances communicating via Unix sockets.

**Deliverables:**
- Transport abstraction
- Unix domain socket transport implementation
- Router with remote actor support
- Topology configuration (JSON/YAML)
- Test: 3 processes (simulating P4-0, P4-1, C6) exchanging messages

**Success Criteria:**
- Messages route correctly between nodes
- Topology configurable via file
- Clean shutdown and reconnection

### Phase 3: Network Transport (Week 3)
**Goal:** TCP transport for multi-machine communication.

**Deliverables:**
- TCP transport implementation
- Message serialization/deserialization
- Test: 2 machines (or localhost) running distributed actor system

**Success Criteria:**
- Works over real network
- Handles connection failures gracefully
- Performance: sub-millisecond local latency, reasonable network latency

### Phase 4: WASM Integration (Week 4+)
**Goal:** Actors defined by WASM modules.

**Deliverables:**
- WASM-MR embedding
- Capability-based syscall interface
- Actor behavior from WASM module
- WASM ↔ native message serialization

**Success Criteria:**
- Can load WASM module and spawn actors from it
- WASM actors communicate with native actors
- Security: WASM cannot escape sandbox

### Phase 5: ESP32 Port (Month 2)
**Goal:** Port runtime to ESP32-P4.

**Deliverables:**
- FreeRTOS integration
- SPI/UART transports for inter-chip communication
- PSRAM-optimized memory management
- MIDI actor example

## 6. Topology Model

### Topology Description (YAML example)
```yaml
nodes:
  - id: 0
    name: "p4-core-0"
    role: "primary"
    
  - id: 1
    name: "p4-core-1"
    role: "compute"
    
  - id: 2
    name: "c6-companion"
    role: "io"

transports:
  - from: 0
    to: 1
    type: "local"  # Shared memory
    
  - from: 0
    to: 2
    type: "unix"
    path: "/tmp/p4-to-c6.sock"
    
  - from: 1
    to: 2
    type: "unix"
    path: "/tmp/p4-1-to-c6.sock"

actors:
  # Initial actors to spawn on boot
  - node: 0
    behavior: "system_supervisor"
    
  - node: 2
    behavior: "midi_input_handler"
```

### Actor ID Encoding
```
┌────────────┬────────────────────────┐
│ Node ID    │ Local Actor Sequence   │
│ (32 bits)  │ (32 bits)              │
└────────────┴────────────────────────┘
```

This ensures globally unique actor IDs and allows router to extract node from ID.

## 7. Example Use Cases

### Example 1: MIDI Note Processing

```c
// MIDI input actor (runs on C6 node, handles hardware UART)
bool midi_input_behavior(actor_t *self, message_t *msg, void *state) {
    if (msg->type == MSG_HARDWARE_UART_DATA) {
        uint8_t *midi_bytes = msg->payload;
        // Parse MIDI message
        if (is_note_on(midi_bytes)) {
            // Send to processing actor on P4
            actor_send(runtime, processor_actor_id, MSG_MIDI_NOTE_ON, 
                       midi_bytes, 3, NULL);
        }
    }
    return true;
}

// MIDI processor actor (runs on P4 core, does intelligent processing)
bool midi_processor_behavior(actor_t *self, message_t *msg, void *state) {
    if (msg->type == MSG_MIDI_NOTE_ON) {
        // Apply arpeggiator, harmonizer, etc.
        uint8_t *processed = apply_effects(msg->payload, state);
        
        // Send to output actor on C6
        actor_send(runtime, output_actor_id, MSG_MIDI_SEND_OUT,
                   processed, 3, free);
    }
    return true;
}
```

### Example 2: Multi-Board Coordination

```c
// Device #1 broadcasts state update
actor_send(runtime, BROADCAST_ALL_PEERS, MSG_STATE_UPDATE, 
           &my_state, sizeof(my_state), NULL);

// Device #2 receives and synchronizes
bool sync_behavior(actor_t *self, message_t *msg, void *state) {
    if (msg->type == MSG_STATE_UPDATE) {
        update_local_state(msg->payload, state);
    }
    return true;
}
```

## 8. Implementation Notes

### Memory Management Strategy
- **Message pools**: Pre-allocated message structs, recycled after delivery
- **Payload arenas**: Per-node memory arenas for short-lived payloads
- **Zero-copy**: Pass pointers, transfer ownership via `free_payload` callback
- **Bounded mailboxes**: Fixed-size ring buffers prevent runaway memory growth

### Error Handling
- **Mailbox full**: `actor_send()` returns false, caller must retry or drop
- **Transport failure**: Log error, mark transport disconnected, retry with backoff
- **Actor crash**: Supervisor pattern (future), restart actor with clean state
- **Message deserialization failure**: Drop message, log error

### Threading Model (Linux)
- **Phase 1**: Single-threaded event loop per node process
- **Phase 2**: Optional worker thread pool for CPU-intensive actors
- **ESP32**: FreeRTOS tasks, one per core, or single task with round-robin

### Serialization (for remote messages)
- **Phase 1**: Simple binary format (header + payload)
- **Future**: MessagePack, Protobuf, or custom schema
- **WASM messages**: Linear memory slice serialization

## 9. API Summary

### Core Runtime API
```c
// Initialization
runtime_t* runtime_init(node_id_t my_node_id, size_t max_actors);
void runtime_destroy(runtime_t *rt);

// Actor lifecycle
actor_id_t actor_spawn(runtime_t *rt, actor_behavior_fn behavior, 
                       void *initial_state, size_t mailbox_size);
void actor_stop(runtime_t *rt, actor_id_t actor_id);

// Messaging
bool actor_send(runtime_t *rt, actor_id_t dest, msg_type_t type,
                void *payload, size_t payload_size, 
                void (*free_payload)(void *));

// Helpers for actor implementations
actor_id_t actor_self(runtime_t *rt);  // Get current actor's ID
void* actor_state(runtime_t *rt);      // Get current actor's state

// Transport
void runtime_add_transport(runtime_t *rt, node_id_t peer_node, 
                           transport_t *transport);

// Execution
void runtime_run(runtime_t *rt);   // Run until shutdown
void runtime_step(runtime_t *rt);  // Single iteration
void runtime_stop(runtime_t *rt);  // Graceful shutdown
```

### Transport API
```c
transport_t* transport_unix_create(const char *socket_path, node_id_t peer_node);
transport_t* transport_tcp_create(const char *host, uint16_t port, node_id_t peer_node);
void transport_destroy(transport_t *transport);
```

## 10. Testing Strategy

### Unit Tests
- Mailbox operations (enqueue/dequeue, wraparound, full/empty)
- Message creation/destruction
- Actor ID encoding/decoding
- Router lookups

### Integration Tests
- Single-node: spawn multiple actors, message passing
- Multi-node: IPC between processes
- Failure scenarios: transport disconnect, mailbox overflow

### Performance Tests
- Message throughput (messages/sec)
- Latency (send to receive time)
- Scheduling fairness
- Memory usage under load

### Target Metrics (Phase 1, Linux)
- Local message latency: < 1 microsecond
- IPC message latency: < 100 microseconds
- Throughput: > 100K messages/sec
- Memory: < 1MB per runtime instance (idle)

## 11. Future Extensions

### Advanced Scheduling
- Priority levels (real-time vs best-effort)
- Deadline scheduling
- CPU affinity (pin actors to cores)

### Fault Tolerance
- Supervision trees (Erlang-style)
- Actor restart policies
- Distributed consensus for critical state

### Monitoring & Introspection
- Message tracing
- Mailbox depth metrics
- Actor CPU time profiling
- Live topology visualization

### WASM Enhancements
- Hot code reloading
- Capability marketplace
- Pre-JIT compilation server
- Sandboxed debugging

### Cloud Integration
- WebSocket transport with reconnection
- Cloudflare Durable Objects backend
- Edge-cloud RPC patterns
- Distributed actor registry

## 12. References & Inspirations

- **Erlang/OTP**: Actor model, supervision trees, let-it-crash philosophy
- **Akka**: JVM actor framework, clustering, persistence
- **CAF (C++ Actor Framework)**: Efficient C++ implementation
- **Pony language**: Capabilities-based concurrency
- **BEAM VM**: Efficient process scheduling, message passing
- **Cloudflare Workers**: Edge compute model
- **WASM Component Model**: Portable, sandboxed modules

---

## Next Steps

1. Implement core data structures (message, mailbox, actor)
2. Build basic scheduler and runtime event loop
3. Create simple ping-pong test between two actors
4. Add Unix socket transport for multi-process communication
5. Implement router with local/remote dispatch
6. Build topology parser and initialization

**Initial Development Environment:**
- Linux (Ubuntu/Debian)
- GCC or Clang
- Valgrind for memory checking
- CMake for build system
- Unit tests with simple C test framework

Let's build the infrastructure for warmth! 🔥