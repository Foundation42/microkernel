#include "fiber_xtensa.h"
#include <string.h>
#include <xtensa/hal.h>

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
            /* Spill all register windows to memory before switching stacks.
               This ensures the current stack frames are fully saved. */
            xthal_window_spill();
            /* new_sp = top of stack (stacks grow downward on Xtensa).
               Must be 16-byte aligned — caller ensures stack buffer is aligned. */
            void *new_sp = to->stack + to->stack_size;
            _fiber_start_asm(new_sp, to->entry, to->arg);
            /* Never reached */
        }
    }
    /* Resumed here via longjmp into from->jb */
}
