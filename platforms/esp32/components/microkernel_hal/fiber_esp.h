/*
 * fiber_esp.h — Architecture-neutral fiber API for ESP32 platforms.
 *
 * Used by both Xtensa (ESP32-S3, etc.) and RISC-V (ESP32-C6, etc.) targets.
 * The implementation is provided by fiber_xtensa.c/.S or fiber_riscv.c/.S
 * depending on the target architecture.
 */
#ifndef FIBER_ESP_H
#define FIBER_ESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

typedef struct {
    jmp_buf   jb;
    uint8_t  *stack;
    size_t    stack_size;
    void    (*entry)(void *);
    void     *arg;
    bool      started;
} fiber_context_t;

/* Initialize a fiber context (does not start it). */
void fiber_init(fiber_context_t *ctx, uint8_t *stack, size_t size,
                void (*entry)(void *), void *arg);

/* Switch from 'from' to 'to'. If 'to' has not started, launches it on its
   stack via _fiber_start_asm. Otherwise resumes it via longjmp. */
void fiber_switch(fiber_context_t *from, fiber_context_t *to);

/* Assembly stub: switch SP and call entry(arg). Never returns. */
extern void _fiber_start_asm(void *new_sp, void (*entry)(void *), void *arg);

#endif /* FIBER_ESP_H */
