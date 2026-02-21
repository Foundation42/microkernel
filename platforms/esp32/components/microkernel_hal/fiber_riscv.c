/*
 * fiber_riscv.c — RISC-V fiber implementation for ESP32-C6 / ESP32-H2.
 *
 * Simpler than Xtensa: RISC-V has no register windows, so no spill is
 * needed before switching stacks.  We just setjmp/longjmp + assembly stub.
 */
#include "fiber_esp.h"
#include <string.h>

void fiber_init(fiber_context_t *ctx, uint8_t *stack, size_t size,
                void (*entry)(void *), void *arg) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->stack      = stack;
    ctx->stack_size = size;
    ctx->entry      = entry;
    ctx->arg        = arg;
    ctx->started    = false;
}

void fiber_switch(fiber_context_t *from, fiber_context_t *to) {
    if (setjmp(from->jb) == 0) {
        if (to->started) {
            longjmp(to->jb, 1);
        } else {
            to->started = true;
            /* RISC-V has no register windows — no spill needed.
               new_sp = top of stack (stacks grow downward).
               Must be 16-byte aligned — caller ensures stack buffer is aligned. */
            void *new_sp = to->stack + to->stack_size;
            _fiber_start_asm(new_sp, to->entry, to->arg);
            /* Never reached */
        }
    }
    /* Resumed here via longjmp into from->jb */
}
