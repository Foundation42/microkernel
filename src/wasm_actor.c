#ifdef HAVE_WASM

#include "microkernel/wasm_actor.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "wasm_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_UCONTEXT)
  #include <ucontext.h>
#elif defined(ESP_PLATFORM)
  #include "fiber_xtensa.h"
#endif

/* ── Factory arg (owns module + buffer copy) ──────────────────────── */

struct wasm_factory_arg {
    uint8_t              *wasm_buf;       /* owned copy of WASM bytes */
    size_t                wasm_size;
    wasm_module_t         module;         /* parsed once, shared across instances */
    uint32_t              stack_size;
    uint32_t              heap_size;
    fiber_stack_class_t   fiber_stack;
};

/* ── Per-actor state (owns instance + exec_env only) ──────────────── */

typedef struct {
    wasm_module_inst_t   instance;
    wasm_exec_env_t      exec_env;
    wasm_function_inst_t handle_message_fn;
    runtime_t           *rt;               /* refreshed on each behavior call */
    bool                 owns_module;      /* true for standalone spawn */
    wasm_module_t        module;           /* only valid if owns_module */
    uint8_t             *module_buf;       /* only valid if owns_module */

    /* Fiber support */
#if defined(HAVE_UCONTEXT)
    ucontext_t    fiber_ctx;        /* saved fiber context */
    ucontext_t    caller_ctx;       /* saved caller context (behavior fn) */
#elif defined(ESP_PLATFORM)
    fiber_context_t fiber_ctx;      /* saved fiber context */
    fiber_context_t caller_ctx;     /* saved caller context (behavior fn) */
#endif
    uint8_t      *fiber_stack;      /* malloc'd native stack, NULL if sync-only */
    size_t        fiber_stack_size;
    bool          fiber_yielded;    /* true if fiber suspended mid-execution */
    int           fiber_result;     /* handle_message return: 1=alive, 0=stop, -1=trap */

    /* Pending message data (copied for fiber_entry to consume) */
    uint32_t      pending_argv[5];

    /* For mk_recv: stash message delivered while fiber suspended */
    message_t    *recv_msg;

    /* WASM-side payload buffer offset (for cleanup after fiber completes) */
    uint64_t      wasm_buf_offset;
} wasm_actor_state_t;

/* ── Host functions ───────────────────────────────────────────────── */

static int32_t mk_send_native(wasm_exec_env_t env, int64_t dest,
                                int32_t type, uint8_t *payload, int32_t size) {
    wasm_actor_state_t *s = wasm_runtime_get_user_data(env);
    return actor_send(s->rt, (actor_id_t)dest, (msg_type_t)type,
                      payload, (size_t)size) ? 1 : 0;
}

static int64_t mk_self_native(wasm_exec_env_t env) {
    wasm_actor_state_t *s = wasm_runtime_get_user_data(env);
    return (int64_t)actor_self(s->rt);
}

static void mk_log_native(wasm_exec_env_t env, int32_t level,
                            uint8_t *text, int32_t len) {
    wasm_actor_state_t *s = wasm_runtime_get_user_data(env);
    char buf[256];
    int n = len < 255 ? len : 255;
    memcpy(buf, text, n);
    buf[n] = '\0';
    actor_log(s->rt, level, "%s", buf);
}

static int32_t mk_sleep_ms_native(wasm_exec_env_t env, int32_t ms) {
    wasm_actor_state_t *s = wasm_runtime_get_user_data(env);
    if (!s->fiber_stack)
        return -1;  /* no fiber — can't yield */

    actor_set_timer(s->rt, (uint64_t)ms, false);
    s->fiber_yielded = true;
#if defined(HAVE_UCONTEXT)
    swapcontext(&s->fiber_ctx, &s->caller_ctx);
#elif defined(ESP_PLATFORM)
    fiber_switch(&s->fiber_ctx, &s->caller_ctx);
#endif
    /* Resumed by behavior after timer fires */
    return 0;
}

static int32_t mk_recv_native(wasm_exec_env_t env, uint32_t *type_out,
                                uint8_t *buf, int32_t buf_size,
                                uint32_t *size_out) {
    wasm_actor_state_t *s = wasm_runtime_get_user_data(env);
    if (!s->fiber_stack)
        return -1;  /* no fiber — can't yield */

    s->recv_msg = NULL;
    s->fiber_yielded = true;
#if defined(HAVE_UCONTEXT)
    swapcontext(&s->fiber_ctx, &s->caller_ctx);
#elif defined(ESP_PLATFORM)
    fiber_switch(&s->fiber_ctx, &s->caller_ctx);
#endif
    /* Resumed by behavior when a message arrives */

    if (!s->recv_msg)
        return -1;

    *type_out = (uint32_t)s->recv_msg->type;
    size_t copy_size = s->recv_msg->payload_size;
    if (copy_size > (size_t)buf_size)
        copy_size = (size_t)buf_size;
    if (copy_size > 0 && s->recv_msg->payload)
        memcpy(buf, s->recv_msg->payload, copy_size);
    *size_out = (uint32_t)s->recv_msg->payload_size;
    return 0;
}

/* WAMR's NativeSymbol uses void* for func_ptr; suppress pedantic warning */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static NativeSymbol native_symbols[] = {
    { "mk_send",     mk_send_native,     "(Ii*~)i",   NULL },
    { "mk_self",     mk_self_native,     "()I",       NULL },
    { "mk_log",      mk_log_native,      "(i*~)",     NULL },
    { "mk_sleep_ms", mk_sleep_ms_native, "(i)i",      NULL },
    { "mk_recv",     mk_recv_native,     "(**~*)i",   NULL },
};
#pragma GCC diagnostic pop

/* ── Init / cleanup ───────────────────────────────────────────────── */

bool wasm_actors_init(void) {
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    init_args.native_module_name = "env";
    init_args.native_symbols = native_symbols;
    init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
    return wasm_runtime_full_init(&init_args);
}

void wasm_actors_cleanup(void) {
    wasm_runtime_destroy();
}

/* ── Factory arg ──────────────────────────────────────────────────── */

wasm_factory_arg_t *wasm_factory_arg_create(const uint8_t *wasm_buf,
                                             size_t wasm_size,
                                             uint32_t stack_size,
                                             uint32_t heap_size,
                                             fiber_stack_class_t fiber_stack) {
    wasm_factory_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg) return NULL;

    arg->wasm_buf = malloc(wasm_size);
    if (!arg->wasm_buf) {
        free(arg);
        return NULL;
    }
    memcpy(arg->wasm_buf, wasm_buf, wasm_size);
    arg->wasm_size = wasm_size;
    arg->stack_size = stack_size;
    arg->heap_size = heap_size;
    arg->fiber_stack = fiber_stack;

    char error_buf[128];
    arg->module = wasm_runtime_load(arg->wasm_buf, (uint32_t)wasm_size,
                                     error_buf, sizeof(error_buf));
    if (!arg->module) {
        fprintf(stderr, "wasm_factory_arg_create: load failed: %s\n", error_buf);
        free(arg->wasm_buf);
        free(arg);
        return NULL;
    }

    return arg;
}

void wasm_factory_arg_destroy(wasm_factory_arg_t *arg) {
    if (!arg) return;
    if (arg->module)
        wasm_runtime_unload(arg->module);
    free(arg->wasm_buf);
    free(arg);
}

/* ── Fiber entry point ────────────────────────────────────────────── */

#if defined(HAVE_UCONTEXT)
/* makecontext only guarantees int-sized args. Split pointer into two. */
static void fiber_entry(unsigned int lo, unsigned int hi) {
    uintptr_t addr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    wasm_actor_state_t *state = (wasm_actor_state_t *)addr;

    bool ok = wasm_runtime_call_wasm(state->exec_env,
                                      state->handle_message_fn, 5,
                                      state->pending_argv);
    if (ok) {
        state->fiber_result = state->pending_argv[0] != 0 ? 1 : 0;
    } else {
        const char *exception = wasm_runtime_get_exception(state->instance);
        fprintf(stderr, "wasm fiber: trap: %s\n",
                exception ? exception : "(unknown)");
        state->fiber_result = -1;
    }

    state->fiber_yielded = false;
    /* Returns to caller_ctx via uc_link */
}
#elif defined(ESP_PLATFORM)
static void fiber_entry_esp(void *arg) {
    wasm_actor_state_t *state = (wasm_actor_state_t *)arg;

    bool ok = wasm_runtime_call_wasm(state->exec_env,
                                      state->handle_message_fn, 5,
                                      state->pending_argv);
    if (ok) {
        state->fiber_result = state->pending_argv[0] != 0 ? 1 : 0;
    } else {
        const char *exception = wasm_runtime_get_exception(state->instance);
        fprintf(stderr, "wasm fiber: trap: %s\n",
                exception ? exception : "(unknown)");
        state->fiber_result = -1;
    }

    state->fiber_yielded = false;
    /* No uc_link on ESP32 — explicitly switch back to caller */
    fiber_switch(&state->fiber_ctx, &state->caller_ctx);
    /* Never reached */
    for (;;) {}
}
#endif

/* ── Factory function (creates per-actor instance) ────────────────── */

void *wasm_actor_factory(void *arg_ptr) {
    wasm_factory_arg_t *arg = arg_ptr;
    if (!arg || !arg->module) return NULL;

    wasm_actor_state_t *state = calloc(1, sizeof(*state));
    if (!state) return NULL;

    char error_buf[128];
    state->instance = wasm_runtime_instantiate(arg->module, arg->stack_size,
                                                arg->heap_size, error_buf,
                                                sizeof(error_buf));
    if (!state->instance) {
        fprintf(stderr, "wasm_actor_factory: instantiate failed: %s\n", error_buf);
        free(state);
        return NULL;
    }

    state->exec_env = wasm_runtime_create_exec_env(state->instance,
                                                     arg->stack_size);
    if (!state->exec_env) {
        fprintf(stderr, "wasm_actor_factory: create_exec_env failed\n");
        wasm_runtime_deinstantiate(state->instance);
        free(state);
        return NULL;
    }

    state->handle_message_fn = wasm_runtime_lookup_function(state->instance,
                                                             "handle_message");
    if (!state->handle_message_fn) {
        fprintf(stderr, "wasm_actor_factory: handle_message not found\n");
        wasm_runtime_destroy_exec_env(state->exec_env);
        wasm_runtime_deinstantiate(state->instance);
        free(state);
        return NULL;
    }

    wasm_runtime_set_user_data(state->exec_env, state);
    state->owns_module = false;

    /* Allocate fiber stack if requested */
    if (arg->fiber_stack != FIBER_STACK_NONE) {
        size_t sz = (size_t)arg->fiber_stack;
        state->fiber_stack = malloc(sz);
        if (!state->fiber_stack) {
            fprintf(stderr, "wasm_actor_factory: fiber stack alloc failed\n");
            wasm_runtime_destroy_exec_env(state->exec_env);
            wasm_runtime_deinstantiate(state->instance);
            free(state);
            return NULL;
        }
        state->fiber_stack_size = sz;

        /* Tell WAMR about our fiber stack boundary (guard zone at bottom) */
        wasm_runtime_set_native_stack_boundary(
            state->exec_env,
            state->fiber_stack + FIBER_GUARD_SIZE);
    }

    return state;
}

/* ── Free function ────────────────────────────────────────────────── */

void wasm_actor_free(void *state_ptr) {
    wasm_actor_state_t *state = state_ptr;
    if (!state) return;
    if (state->exec_env)
        wasm_runtime_destroy_exec_env(state->exec_env);
    if (state->instance)
        wasm_runtime_deinstantiate(state->instance);
    if (state->owns_module) {
        if (state->module)
            wasm_runtime_unload(state->module);
        free(state->module_buf);
    }
    free(state->fiber_stack);
    free(state);
}

/* ── Behavior function ────────────────────────────────────────────── */

bool wasm_actor_behavior(runtime_t *rt, actor_t *self,
                          message_t *msg, void *state_ptr) {
    (void)self;
    wasm_actor_state_t *state = state_ptr;
    state->rt = rt;

    /* ── Case 1: Resume suspended fiber ────────────────────────────── */
    if (state->fiber_yielded) {
        /* Stash message for mk_recv if the fiber was blocked on recv */
        state->recv_msg = msg;
#if defined(HAVE_UCONTEXT)
        swapcontext(&state->caller_ctx, &state->fiber_ctx);
#elif defined(ESP_PLATFORM)
        fiber_switch(&state->caller_ctx, &state->fiber_ctx);
#endif

        if (state->fiber_yielded) {
            /* Fiber yielded again (another blocking call) — keep alive */
            return true;
        }

        /* Fiber completed — clean up WASM payload buffer from initial call */
        if (state->wasm_buf_offset != 0) {
            wasm_runtime_module_free(state->instance, state->wasm_buf_offset);
            state->wasm_buf_offset = 0;
        }

        if (state->fiber_result < 0)
            return false;  /* trap */
        return state->fiber_result != 0;
    }

    wasm_module_inst_t inst = state->instance;
    uint64_t wasm_buf_offset = 0;

    /* Copy payload into WASM linear memory if present */
    if (msg->payload && msg->payload_size > 0) {
        void *native_addr = NULL;
        wasm_buf_offset = wasm_runtime_module_malloc(inst,
                                                      (uint64_t)msg->payload_size,
                                                      &native_addr);
        if (wasm_buf_offset == 0) {
            fprintf(stderr, "wasm_actor_behavior: module_malloc failed\n");
            return false;
        }
        memcpy(native_addr, msg->payload, msg->payload_size);
    }

    /* Build argv: handle_message(i32 msg_type, i64 source_id, i32 payload_ptr, i32 payload_size)
       i64 occupies two uint32 slots (lo, hi) in WAMR's argv convention */
    uint32_t argv[5];
    argv[0] = (uint32_t)msg->type;
    argv[1] = (uint32_t)(msg->source & 0xFFFFFFFF);       /* i64 lo */
    argv[2] = (uint32_t)((msg->source >> 32) & 0xFFFFFFFF); /* i64 hi */
    argv[3] = (uint32_t)wasm_buf_offset;
    argv[4] = (uint32_t)msg->payload_size;

    /* ── Case 2: Sync path (no fiber stack) ────────────────────────── */
    if (!state->fiber_stack) {
        bool ok = wasm_runtime_call_wasm(state->exec_env,
                                          state->handle_message_fn, 5, argv);

        if (wasm_buf_offset != 0)
            wasm_runtime_module_free(inst, wasm_buf_offset);

        if (!ok) {
            const char *exception = wasm_runtime_get_exception(inst);
            fprintf(stderr, "wasm_actor_behavior: trap: %s\n",
                    exception ? exception : "(unknown)");
            return false;
        }
        return argv[0] != 0;
    }

    /* ── Case 3: Start new fiber ───────────────────────────────────── */
    memcpy(state->pending_argv, argv, sizeof(argv));
    state->wasm_buf_offset = wasm_buf_offset;
    state->fiber_result = 1;
    state->fiber_yielded = false;

#if defined(HAVE_UCONTEXT)
    if (getcontext(&state->fiber_ctx) == -1) {
        fprintf(stderr, "wasm_actor_behavior: getcontext failed\n");
        if (wasm_buf_offset != 0)
            wasm_runtime_module_free(inst, wasm_buf_offset);
        return false;
    }

    state->fiber_ctx.uc_stack.ss_sp = state->fiber_stack;
    state->fiber_ctx.uc_stack.ss_size = state->fiber_stack_size;
    state->fiber_ctx.uc_link = &state->caller_ctx;

    uintptr_t addr = (uintptr_t)state;
    unsigned int lo = (unsigned int)(addr & 0xFFFFFFFF);
    unsigned int hi = (unsigned int)((addr >> 32) & 0xFFFFFFFF);
    makecontext(&state->fiber_ctx, (void (*)(void))fiber_entry, 2, lo, hi);

    swapcontext(&state->caller_ctx, &state->fiber_ctx);
#elif defined(ESP_PLATFORM)
    fiber_init(&state->fiber_ctx, state->fiber_stack,
               state->fiber_stack_size, fiber_entry_esp, state);
    /* Reset caller context for fresh switch */
    memset(&state->caller_ctx, 0, sizeof(state->caller_ctx));
    state->caller_ctx.started = true;
    fiber_switch(&state->caller_ctx, &state->fiber_ctx);
#endif

    if (state->fiber_yielded) {
        /* Fiber yielded — keep alive, don't free WASM buffer yet */
        return true;
    }

    /* Fiber completed synchronously (no yield) */
    if (wasm_buf_offset != 0)
        wasm_runtime_module_free(inst, wasm_buf_offset);
    state->wasm_buf_offset = 0;

    if (state->fiber_result < 0)
        return false;
    return state->fiber_result != 0;
}

/* ── Standalone spawn (owns its own module) ───────────────────────── */

actor_id_t actor_spawn_wasm(runtime_t *rt, const uint8_t *wasm_buf,
                             size_t wasm_size, size_t mailbox_size,
                             uint32_t stack_size, uint32_t heap_size,
                             fiber_stack_class_t fiber_stack) {
    /* Create a private factory arg */
    wasm_factory_arg_t *arg = wasm_factory_arg_create(wasm_buf, wasm_size,
                                                       stack_size, heap_size,
                                                       fiber_stack);
    if (!arg) return ACTOR_ID_INVALID;

    /* Create the actor state */
    wasm_actor_state_t *state = wasm_actor_factory(arg);
    if (!state) {
        wasm_factory_arg_destroy(arg);
        return ACTOR_ID_INVALID;
    }

    /* Transfer module ownership to the state so it cleans up on actor death */
    state->owns_module = true;
    state->module = arg->module;
    state->module_buf = arg->wasm_buf;

    /* Detach ownership from the factory arg (don't double-free) */
    arg->module = NULL;
    arg->wasm_buf = NULL;
    wasm_factory_arg_destroy(arg);

    return actor_spawn(rt, wasm_actor_behavior, state, wasm_actor_free,
                       mailbox_size);
}

actor_id_t actor_spawn_wasm_file(runtime_t *rt, const char *path,
                                  size_t mailbox_size,
                                  uint32_t stack_size, uint32_t heap_size,
                                  fiber_stack_class_t fiber_stack) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "actor_spawn_wasm_file: cannot open %s\n", path);
        return ACTOR_ID_INVALID;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return ACTOR_ID_INVALID;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return ACTOR_ID_INVALID;
    }

    size_t read_size = fread(buf, 1, (size_t)file_size, f);
    fclose(f);

    if (read_size != (size_t)file_size) {
        free(buf);
        return ACTOR_ID_INVALID;
    }

    actor_id_t id = actor_spawn_wasm(rt, buf, read_size, mailbox_size,
                                      stack_size, heap_size, fiber_stack);
    free(buf);
    return id;
}

#endif /* HAVE_WASM */
