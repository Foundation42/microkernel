#ifndef FIBER_XTENSA_H
#define FIBER_XTENSA_H

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

#endif /* FIBER_XTENSA_H */
