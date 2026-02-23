/* mk_readline.h — reusable readline with history, line editing, arrow keys.
 *
 * Platform-agnostic: caller provides a write callback for output.
 * Caller is responsible for putting the terminal into raw mode.
 */

#ifndef MICROKERNEL_MK_READLINE_H
#define MICROKERNEL_MK_READLINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef MK_RL_MAX_LINE
#define MK_RL_MAX_LINE    256
#endif

#ifndef MK_RL_MAX_HISTORY
#define MK_RL_MAX_HISTORY 32
#endif

/* Return codes from mk_rl_feed() */
#define MK_RL_CONTINUE  0   /* Still editing                    */
#define MK_RL_DONE      1   /* Line complete — read rl->line    */
#define MK_RL_EOF      -1   /* Ctrl+D on empty line             */

typedef void (*mk_rl_write_fn)(const char *buf, size_t len, void *ctx);

typedef struct {
    /* Current editing buffer */
    char   line[MK_RL_MAX_LINE];
    size_t len;
    size_t pos;           /* cursor position within line */

    /* History ring buffer */
    char   history[MK_RL_MAX_HISTORY][MK_RL_MAX_LINE];
    size_t hist_count;    /* total entries ever stored (may exceed ring size) */
    size_t hist_next;     /* next write slot (ring index) */
    size_t hist_browse;   /* browse offset: 0=current line, 1=most recent… */
    char   saved[MK_RL_MAX_LINE]; /* in-progress line saved while browsing */
    size_t saved_len;

    /* Escape sequence parser */
    int    esc;           /* 0=normal, 1=ESC, 2=ESC[, 3=ESC[digit */
    int    esc_num;

    /* CRLF dedup */
    bool   last_cr;

    /* Prompt (borrowed pointer — must outlive this struct) */
    const char *prompt;
    size_t      prompt_len;

    /* Output */
    mk_rl_write_fn write_cb;
    void          *write_ctx;
} mk_readline_t;

/**
 * Initialise readline state.
 *
 * @param prompt    Prompt string (borrowed, must outlive rl).
 * @param write_cb  Output function for echoing / redrawing.
 * @param ctx       Opaque context passed to write_cb.
 */
void mk_rl_init(mk_readline_t *rl, const char *prompt,
                mk_rl_write_fn write_cb, void *ctx);

/**
 * Feed one byte of input.  Call this for every byte read from the terminal.
 *
 * @return MK_RL_CONTINUE  — still editing
 *         MK_RL_DONE      — line complete; read rl->line (NUL-terminated)
 *         MK_RL_EOF       — Ctrl+D on empty line
 */
int mk_rl_feed(mk_readline_t *rl, uint8_t ch);

/**
 * Print the prompt and prepare for a fresh input line.
 * Call after processing a command or when ready for new input.
 */
void mk_rl_prompt(mk_readline_t *rl);

/**
 * Add a completed line to history.
 * Empty lines and duplicates of the most recent entry are skipped.
 */
void mk_rl_history_add(mk_readline_t *rl, const char *line);

/**
 * Retrieve a history entry (0 = most recent).
 * Returns NULL if idx is out of range.
 */
const char *mk_rl_history_get(const mk_readline_t *rl, size_t idx);

/** Total number of history entries stored. */
size_t mk_rl_history_count(const mk_readline_t *rl);

#endif /* MICROKERNEL_MK_READLINE_H */
