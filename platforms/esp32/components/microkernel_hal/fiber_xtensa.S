/*
 * fiber_xtensa.S — Xtensa assembly stub for fiber stack switch.
 *
 * void _fiber_start_asm(void *new_sp, void (*entry)(void *), void *arg);
 *
 * Switches the stack pointer to new_sp, then calls entry(arg).
 * Never returns — the entry function must switch back via fiber_switch.
 */

    .text
    .align  4
    .global _fiber_start_asm
    .type   _fiber_start_asm, @function

_fiber_start_asm:
    entry   a1, 16          /* Windowed ABI prologue */
    mov     a1, a2          /* Switch SP to new fiber stack (top) */
    addi    a1, a1, -16     /* Reserve 16-byte base save area */
    mov     a6, a4          /* arg → callee's a2 (callx4 rotates window by 4) */
    callx4  a3              /* Call entry(arg) — never returns */
    ill                     /* Trap if entry somehow returns */

    .size   _fiber_start_asm, . - _fiber_start_asm
