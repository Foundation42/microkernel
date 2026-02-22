# Microkernel Roadmap

## Next Up

- [x] **Local persistence** — SPIFFS-backed KV actor on ESP32, file-backed on Linux; registers `/node/storage/kv` so virtual path resolves locally first, Cloudflare as fallback
- [x] **D1 schema/migrations** — Bootstrap schema for the Worker (logs, config tables); `sql` command works end-to-end out of the box
- [x] **Actor state persistence** — Save/restore actor state across reboots via local KV or SPIFFS; supervision trees auto-recover stateful actors
- [x] **Hot code reload** — Atomic WASM module swap without restarting the runtime; shell `reload` command, WAMR pool allocator for ESP32
- [ ] **Separate ESP32 smoke tests** — Extract smoke tests into a separate ESP-IDF app or Kconfig option; currently disabled (`RUN_SMOKE_TESTS`) to save RAM for the shell + WAMR pool on constrained chips (ESP32-C6, 512KB SRAM)
- [ ] **Node discovery mesh** — Cloudflare as rendezvous point for nodes across networks; extend ESP32 UDP broadcast discovery

## Future

- [ ] **Durable Objects** — Cloudflare DO gives each actor a server-side counterpart with transactional storage
- [ ] **Actor migration** — Serialize WASM actor linear memory + mailbox, ship to another node, resume
- [ ] **Edge orchestration** — Cloudflare Worker as coordinator, pushing WASM actors to ESP32 nodes based on capabilities
