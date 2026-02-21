/*
 * fiber_riscv.S — RISC-V assembly stub for fiber stack switch.
 *
 * void _fiber_start_asm(void *new_sp, void (*entry)(void *), void *arg);
 *
 * Switches the stack pointer to new_sp, then calls entry(arg).
 * Never returns — the entry function must switch back via fiber_switch.
 *
 * RISC-V calling convention:
 *   a0 = new_sp, a1 = entry, a2 = arg
 */

    .text
    .align  2
    .global _fiber_start_asm
    .type   _fiber_start_asm, @function

_fiber_start_asm:
    mv      sp, a0          /* Switch SP to new fiber stack (top) */
    addi    sp, sp, -16     /* Reserve 16-byte aligned save area */
    mv      a0, a2          /* arg → first argument for entry */
    jalr    ra, a1, 0       /* Call entry(arg) — never returns */
    unimp                   /* Trap if entry somehow returns */

    .size   _fiber_start_asm, . - _fiber_start_asm
