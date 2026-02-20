# Phase 13: WASM Fiber Support -- Design Spec

> **Note:** This is a pre-implementation design document. The final implementation
> differs in several areas: fiber stack sizes are 32KB/64KB/128KB (not 512B-8KB),
> `mk_recv()` was implemented instead of `mk_wait_fd_readable/writable`, fibers are
> allocated eagerly at spawn time (not lazily), and ESP32 support was added in Phase
> 13b using setjmp/longjmp + Xtensa assembly instead of ucontext. See
> [api.md](api.md) for the current WASM actor API.

## Context

Phase 12 gave us WASM actors that work perfectly for synchronous logic - compute, send messages, return. But if a WASM actor needs to wait for I/O (HTTP request, sensor read, sleep), it currently blocks the entire runtime thread. All other actors freeze until the blocking operation completes.
WAMR provides fiber support (cooperative user-space threads) that solves this. A WASM actor can call a blocking host function (like mk_sleep_ms), yield its fiber, and let the runtime continue processing other actors. When the I/O completes, the runtime resumes the fiber and the WASM actor continues from where it left off - all transparent to the WASM code.
This enables synchronous-style code (no callbacks, no state machines) while maintaining runtime responsiveness.
Design Decisions
1. Lazy Fiber Allocation
Fibers are created only when first blocking call happens. Synchronous actors pay zero overhead.
2. Stack Size Classes
ctypedef enum {
    STACK_NONE   = 0,      // No fiber (synchronous only) - DEFAULT
    STACK_512    = 512,    // Minimal (simple sleep/timer)
    STACK_1K     = 1024,   // Small (basic I/O)
    STACK_2K     = 2048,   // Medium (HTTP, multi-step)
    STACK_4K     = 4096,   // Large (complex protocols)
    STACK_8K     = 8192,   // XLarge (recursive, deep calls)
    STACK_CUSTOM = 0xFFFF  // User-specified
} fiber_stack_class_t;
Actors declare their stack needs at spawn time. Most actors use STACK_NONE (synchronous). Only actors that call blocking host functions need a fiber.
3. Block State Tracking
ctypedef struct {
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    
    // NEW: Fiber support
    wasm_fiber_t fiber;           // NULL until first blocking call
    fiber_stack_class_t stack_class;
    bool is_blocked;
    
    // What are we blocked on?
    int blocked_fd;               // -1 = none
    uint64_t unblock_time;        // 0 = no timeout
} wasm_actor_state_t;
4. Blocking Host Functions
New host functions that yield:
c// Block for milliseconds
int32_t mk_sleep_ms(exec_env, ms) 
    → yields fiber, runtime continues

// Block until FD readable
int32_t mk_wait_fd_readable(exec_env, fd, timeout_ms)
    → yields fiber, runtime polls fd

// Block until FD writable  
int32_t mk_wait_fd_writable(exec_env, fd, timeout_ms)
    → yields fiber, runtime polls fd
Higher-level APIs built on these:
c// Internally uses mk_wait_fd_readable
int32_t mk_http_get_blocking(exec_env, url, response_buf, buf_size);

// For ESP32 (future)
int32_t mk_i2c_read_blocking(exec_env, device_addr, reg);
5. Runtime Integration
runtime_step() checks blocked actors before delivering messages:
c// For each WASM actor:
if (actor->is_blocked) {
    if (fd_ready(actor->blocked_fd) || timeout_expired(actor->unblock_time)) {
        actor->is_blocked = false;
        // Will resume on next message delivery
    }
}
6. Error Handling
If actor with STACK_NONE tries to block → return -1 (error), actor handles gracefully.
Implementation
1. Update wasm_actor_state_t (src/wasm_actor.c)
ctypedef struct {
    // ... existing fields ...
    
    // NEW: Fiber support
    wasm_fiber_t fiber;
    fiber_stack_class_t stack_class;
    bool is_blocked;
    int blocked_fd;         // -1 = not blocked on FD
    uint64_t unblock_time;  // 0 = no timeout
} wasm_actor_state_t;
2. Lazy Fiber Creation (src/wasm_actor.c)
c// Helper: ensure fiber exists (create on first use)
static wasm_fiber_t ensure_fiber(wasm_actor_state_t *ws) {
    if (!ws->fiber && ws->stack_class != STACK_NONE) {
        ws->fiber = wasm_runtime_create_exec_env_with_fiber(
            ws->instance,
            ws->stack_class  // Use declared stack size
        );
    }
    return ws->fiber;
}
3. Blocking Host Functions (src/wasm_actor.c)
c// mk_sleep_ms: Block for milliseconds
static int32_t mk_sleep_ms_native(wasm_exec_env_t exec_env, int32_t ms) {
    wasm_actor_state_t *ws = wasm_runtime_get_user_data(exec_env);
    
    if (!ensure_fiber(ws)) {
        return -1;  // Error: STACK_NONE actor tried to block
    }
    
    ws->is_blocked = true;
    ws->blocked_fd = -1;
    ws->unblock_time = get_monotonic_time_ms() + ms;
    
    wasm_fiber_yield(exec_env);
    return 0;
}

// mk_wait_fd_readable: Block until FD ready or timeout
static int32_t mk_wait_fd_readable_native(wasm_exec_env_t exec_env, 
                                          int32_t fd, int32_t timeout_ms) {
    wasm_actor_state_t *ws = wasm_runtime_get_user_data(exec_env);
    
    if (!ensure_fiber(ws)) return -1;
    
    ws->is_blocked = true;
    ws->blocked_fd = fd;
    ws->unblock_time = (timeout_ms > 0) 
        ? get_monotonic_time_ms() + timeout_ms 
        : 0;
    
    wasm_fiber_yield(exec_env);
    
    // When resumed, check why
    if (ws->blocked_fd == -2) {
        return -1;  // Timeout
    }
    return 0;  // FD ready
}

// mk_wait_fd_writable: Similar pattern
4. Update wasm_actor_behavior (src/wasm_actor.c)
cbool wasm_actor_behavior(runtime_t *rt, actor_t *self,
                        message_t *msg, void *state) {
    wasm_actor_state_t *ws = state;
    ws->rt = rt;
    
    // Check if we should resume blocked fiber
    if (ws->is_blocked) {
        // Don't process new messages while blocked
        return true;  
    }
    
    // ... existing message delivery logic ...
}
5. Update runtime_step (src/runtime.c)
In the main poll loop, before processing actor messages:
c// Check blocked WASM actors
for (size_t i = 1; i < rt->max_actors; i++) {
    actor_t *a = rt->actors[i];
    if (!a || a->status != ACTOR_RUNNING) continue;
    if (a->behavior != wasm_actor_behavior) continue;
    
    wasm_actor_state_t *ws = a->state;
    if (!ws->is_blocked) continue;
    
    bool should_resume = false;
    
    // Check FD readiness
    if (ws->blocked_fd >= 0) {
        struct pollfd pfd = { .fd = ws->blocked_fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            should_resume = true;
        }
    }
    
    // Check timeout
    if (ws->unblock_time > 0) {
        if (get_monotonic_time_ms() >= ws->unblock_time) {
            should_resume = true;
            ws->blocked_fd = -2;  // Signal timeout
        }
    }
    
    if (should_resume) {
        ws->is_blocked = false;
        // Fiber will resume on next message delivery to this actor
    }
}
6. Update API (include/microkernel/wasm_actor.h)
c// Add stack_class parameter
actor_id_t actor_spawn_wasm(runtime_t *rt, 
                            const uint8_t *wasm_buf, size_t wasm_size,
                            size_t mailbox_size,
                            fiber_stack_class_t stack_class);

actor_id_t actor_spawn_wasm_file(runtime_t *rt, const char *path,
                                 size_t mailbox_size,
                                 fiber_stack_class_t stack_class);

// Add to wasm_factory_arg_t
typedef struct {
    const uint8_t *wasm_buf;
    size_t wasm_size;
    fiber_stack_class_t stack_class;  // NEW
    uint32_t heap_size;
} wasm_factory_arg_t;

// Add stack class enum
typedef enum {
    STACK_NONE   = 0,
    STACK_512    = 512,
    STACK_1K     = 1024,
    STACK_2K     = 2048,
    STACK_4K     = 4096,
    STACK_8K     = 8192,
    STACK_CUSTOM = 0xFFFF
} fiber_stack_class_t;
7. Register New Host Functions (src/wasm_actor.c)
cstatic NativeSymbol native_symbols[] = {
    { "mk_send", mk_send_native, "(Ii*~)i", NULL },
    { "mk_self", mk_self_native, "()I", NULL },
    { "mk_log", mk_log_native, "(i*~)", NULL },
    
    // NEW: Blocking functions
    { "mk_sleep_ms", mk_sleep_ms_native, "(i)i", NULL },
    { "mk_wait_fd_readable", mk_wait_fd_readable_native, "(ii)i", NULL },
    { "mk_wait_fd_writable", mk_wait_fd_writable_native, "(ii)i", NULL },
};
8. Update Test Module (tests/wasm_modules/echo.c)
c// Add blocking test
extern int32_t mk_sleep_ms(int32_t ms);

#define MSG_SLEEP_TEST 204

int32_t handle_message(int32_t msg_type, int64_t source_id,
                      const void *payload, int32_t payload_size) {
    // ... existing cases ...
    
    if (msg_type == MSG_SLEEP_TEST) {
        // Block for 50ms (yields fiber, runtime continues)
        mk_sleep_ms(50);
        
        // Send reply after sleep
        mk_send(source_id, MSG_PONG, "slept", 5);
        return 1;
    }
    
    return 1;
}
9. New Test (tests/test_wasm_actor.c)
c// Test 7: Fiber blocking (sleep)
static void test_wasm_fiber_sleep(void) {
    // Spawn WASM actor with STACK_1K
    actor_id_t wasm = actor_spawn_wasm(..., STACK_1K);
    
    // Spawn second native actor
    actor_id_t native = actor_spawn(...);
    
    // Send MSG_SLEEP_TEST to WASM (will block for 50ms)
    actor_send(rt, wasm, MSG_SLEEP_TEST, NULL, 0);
    
    // Send MSG_PING to native actor
    actor_send(rt, native, MSG_PING, NULL, 0);
    
    // Run runtime
    for (int i = 0; i < 100; i++) {
        runtime_step(rt);
    }
    
    // Verify:
    // - Native actor replied immediately
    // - WASM actor replied after ~50ms
    // - Both actors processed messages (runtime didn't block)
}
```

## Files to Modify
```
src/wasm_actor.c         - Add fiber state, ensure_fiber(), blocking host functions
src/runtime.c            - Check blocked WASM actors in poll loop
include/microkernel/wasm_actor.h - Add stack_class parameter
tests/wasm_modules/echo.c - Add MSG_SLEEP_TEST case
tests/test_wasm_actor.c   - Add test_wasm_fiber_sleep (test 7)
```

## Verification

1. `cmake --build build && ctest --test-dir build` → 28/28 pass (27 existing + 1 WASM with 7 sub-tests)
2. Test 7 specifically verifies: WASM actor blocks, native actor continues, both complete
3. Memory check: WASM actor with `STACK_NONE` uses no extra memory; actor with `STACK_2K` uses 2KB only when blocking

## Expected Behavior
```
Runtime thread (single):
  t=0ms:   WASM actor receives MSG_SLEEP_TEST
           Calls mk_sleep_ms(50)
           Fiber yields, actor marked blocked
           
  t=1ms:   Native actor receives MSG_PING
           Processes immediately (not blocked)
           
  t=2-49ms: Runtime skips blocked WASM actor
            Continues processing native actor
            
  t=50ms:  Runtime checks: WASM unblock time reached
           Marks WASM as unblocked
           
  t=51ms:  WASM fiber resumes, mk_sleep_ms returns
           Sends reply, returns 1 (keep alive)
Known Considerations

WAMR fiber API: wasm_runtime_create_exec_env_with_fiber() requires WAMR built with WAMR_BUILD_THREAD_MGR=1
Fiber stack is separate from WASM linear memory (allocated by WAMR from native heap)
STACK_NONE actors that try to block get error return (-1), can handle gracefully
Timeout=0 means "block forever until FD ready" (no timeout)


Goal: After Phase 13, WASM actors can write synchronous-style code with blocking I/O, while the runtime stays responsive and continues processing other actors. 🚀