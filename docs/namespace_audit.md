# Namespace Audit

Audit of the existing flat name registry prior to introducing the namespace actor.

## 1. Call Sites

### `actor_register_name(rt, name, id)`

| File | Line | Context |
|------|------|---------|
| `src/name_registry.c` | 43 | Implementation — inserts into hash table, broadcasts `MSG_NAME_REGISTER` |
| `src/wasm_actor.c` | 288 | `mk_register` host function — registers WASM actor by name |
| `tools/shell/main.c` | 37 | Console actor registers itself as `"console"` |
| `tools/shell/main.c` | 103 | `MSG_SPAWN_REQUEST_NAMED` handler — registers loaded WASM actor by name |
| `tools/shell/main.c` | 106 | Suffix fallback (`_1`, `_2`, ...) if base name is taken |
| `platforms/esp32/main/main.c` | 1607 | ESP32 console actor registers as `"console"` |
| `platforms/esp32/main/main.c` | 1680 | ESP32 `MSG_SPAWN_REQUEST_NAMED` handler |
| `platforms/esp32/main/main.c` | 1683 | ESP32 suffix fallback |
| `tests/test_name_registry.c` | 27, 48, 49, 60 | Unit tests for register/duplicate/deregister |
| `tests/test_distributed_registry.c` | 48, 113, 285, 438, 614, 627 | Cross-node registry tests |
| `tests/test_wasm_actor.c` | 303 | Register WASM echo actor for named lookup test |
| `include/microkernel/services.h` | 85 | Declaration |

### `actor_lookup(rt, name)`

| File | Line | Context |
|------|------|---------|
| `src/name_registry.c` | 56 | Implementation — FNV-1a hash lookup with linear probing |
| `src/runtime.c` | 855 | Used by `actor_send_named()` |
| `src/wasm_actor.c` | 298 | `mk_lookup` host function — returns actor_id to WASM |
| `tests/test_name_registry.c` | 28, 37, 50, 61, 67 | Unit tests |
| `tests/test_distributed_registry.c` | 70, 138, 469, 655 | Cross-node tests (poll until found) |
| `tests/test_wasm_actor.c` | 304 | Verify WASM echo actor is registered |
| `include/microkernel/services.h` | 86 | Declaration |

### `actor_send_named(rt, name, type, payload, size)`

| File | Line | Context |
|------|------|---------|
| `src/runtime.c` | 853 | Implementation — `actor_lookup` + `actor_send` |
| `tests/test_distributed_registry.c` | 51, 71, 308 | Local and cross-node send-by-name tests |
| `include/microkernel/services.h` | 89 | Declaration |

### Internal helpers (not public API)

| Function | File | Line | Context |
|----------|------|------|---------|
| `name_registry_insert` | `src/name_registry.c:20` | Insert without broadcast (for remote entries) |
| `name_registry_remove_by_name` | `src/name_registry.c:73` | Remove without broadcast (for incoming unregister) |
| `name_registry_deregister_actor` | `src/name_registry.c:85` | Remove all entries for an actor (on death), broadcasts |
| `handle_registry_msg` | `src/runtime.c:839` | Intercepts `MSG_NAME_REGISTER`/`MSG_NAME_UNREGISTER` in poll loop |
| `runtime_broadcast_registry` | `src/runtime.c:817` | Sends registry message to all TCP transports |

## 2. Registry Implementation

**File:** `src/name_registry.c`

- **Hash function:** FNV-1a over name string → `uint32_t`
- **Table:** `name_entry_t name_registry[NAME_REGISTRY_SIZE]` embedded in `runtime_t`
- **Entry:** `{ char name[64]; actor_id_t actor_id; bool occupied; }` (defined in `src/runtime_internal.h`)
- **Collision resolution:** Linear probing (open addressing)
- **Capacity:** `NAME_REGISTRY_SIZE` = 128 on Linux (via `#ifndef` guard in `src/runtime.c:53`), 16 on ESP32 (overridden via `target_compile_definitions`)
- **Name length limit:** 64 bytes including null terminator (`NAME_MAX_LEN`)

Operations:
- `name_registry_insert()` — finds empty slot via linear probe, rejects duplicates
- `actor_register_name()` — calls `name_registry_insert()` then broadcasts
- `actor_lookup()` — FNV-1a probe, returns `ACTOR_ID_INVALID` on miss
- `name_registry_remove_by_name()` — linear scan (not hash-based), clears entry
- `name_registry_deregister_actor()` — linear scan by `actor_id`, broadcasts `MSG_NAME_UNREGISTER` for each match, then clears

## 3. Initialization

**File:** `src/runtime.c:88`

`runtime_init()` allocates `runtime_t` with `calloc(1, sizeof(*rt))`, which zero-initializes the entire struct including `name_registry[]`. Since `occupied=false` is zero, all slots start empty. No explicit registry init call is needed.

## 4. Teardown

**File:** `src/runtime.c:125`

`runtime_destroy()` calls `free(rt)`, which deallocates the entire `runtime_t` including the embedded `name_registry[]` array. No explicit registry cleanup is needed since entries contain no heap pointers — names are fixed-size char arrays.

When individual actors stop, `cleanup_stopped()` (line 327) calls `name_registry_deregister_actor()` which removes that actor's entries and broadcasts `MSG_NAME_UNREGISTER` to peers.

## 5. Cross-Node Protocol

**Message types** (defined in `include/microkernel/services.h`):
- `MSG_NAME_REGISTER` (`0xFF000012`) — payload: `name_register_payload_t { char name[64]; actor_id_t actor_id; }`
- `MSG_NAME_UNREGISTER` (`0xFF000013`) — payload: `name_unregister_payload_t { char name[64]; }`

**Broadcast** (`src/runtime.c:817`):
`runtime_broadcast_registry()` iterates all transport slots, creates a message with `source=dest=ACTOR_ID_INVALID`, and calls `tp->send()` for each connected transport.

**Reception** (`src/runtime.c:839`):
`handle_registry_msg()` is called in the `poll_and_dispatch()` transport receive loop (line 554) *before* `deliver_local()`. It intercepts `MSG_NAME_REGISTER` and `MSG_NAME_UNREGISTER`, updates the local registry using the non-broadcasting internal helpers (`name_registry_insert` / `name_registry_remove_by_name`), and returns `true` to consume the message.

**Death cleanup:**
When an actor dies, `name_registry_deregister_actor()` is called from `cleanup_stopped()`. This removes the actor's name entries and broadcasts `MSG_NAME_UNREGISTER` to all peers, ensuring the name is freed on remote nodes as well.
