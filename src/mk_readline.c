/* mk_readline.c — reusable readline with history and line editing.
 *
 * Supports: arrow keys (up/down/left/right), Home/End, Delete,
 *           Ctrl+A/E/K/U/W/C/D/L, backspace, insert anywhere.
 */

#include "microkernel/mk_readline.h"
#include <string.h>
#include <stdio.h>   /* snprintf */

/* ── Output helpers ──────────────────────────────────────────────── */

static void rl_write(mk_readline_t *rl, const char *s, size_t n) {
    if (rl->write_cb && n > 0) rl->write_cb(s, n, rl->write_ctx);
}

static void rl_puts(mk_readline_t *rl, const char *s) {
    rl_write(rl, s, strlen(s));
}

/* Redraw: \r → prompt → line → clear-to-EOL → reposition cursor. */
static void rl_redraw(mk_readline_t *rl) {
    rl_puts(rl, "\r");
    rl_write(rl, rl->prompt, rl->prompt_len);
    rl_write(rl, rl->line, rl->len);
    rl_puts(rl, "\033[K");
    if (rl->pos < rl->len) {
        char seq[16];
        int n = snprintf(seq, sizeof(seq), "\033[%uD",
                         (unsigned)(rl->len - rl->pos));
        rl_write(rl, seq, (size_t)n);
    }
}

/* ── History helpers ─────────────────────────────────────────────── */

/* Map "ago" (0 = most recent) to ring index; returns (size_t)-1 if OOB. */
static size_t ring_idx(const mk_readline_t *rl, size_t ago) {
    size_t total = rl->hist_count < MK_RL_MAX_HISTORY
                       ? rl->hist_count : MK_RL_MAX_HISTORY;
    if (ago >= total) return (size_t)-1;
    return (rl->hist_next + MK_RL_MAX_HISTORY - 1 - ago) % MK_RL_MAX_HISTORY;
}

static void history_browse_to(mk_readline_t *rl, size_t target) {
    size_t max = rl->hist_count < MK_RL_MAX_HISTORY
                     ? rl->hist_count : MK_RL_MAX_HISTORY;
    if (target > max) return;

    /* Save current line when first entering browse mode */
    if (rl->hist_browse == 0 && target > 0) {
        memcpy(rl->saved, rl->line, rl->len);
        rl->saved[rl->len] = '\0';
        rl->saved_len = rl->len;
    }

    rl->hist_browse = target;

    if (target == 0) {
        /* Restore saved in-progress line */
        memcpy(rl->line, rl->saved, rl->saved_len);
        rl->len = rl->saved_len;
    } else {
        size_t ri = ring_idx(rl, target - 1);
        if (ri == (size_t)-1) return;
        size_t slen = strlen(rl->history[ri]);
        memcpy(rl->line, rl->history[ri], slen);
        rl->len = slen;
    }
    rl->line[rl->len] = '\0';
    rl->pos = rl->len;
    rl_redraw(rl);
}

/* ── Editing primitives ──────────────────────────────────────────── */

static void insert_char(mk_readline_t *rl, char c) {
    if (rl->len >= MK_RL_MAX_LINE - 1) return;
    if (rl->pos < rl->len)
        memmove(&rl->line[rl->pos + 1], &rl->line[rl->pos],
                rl->len - rl->pos);
    rl->line[rl->pos] = c;
    rl->len++;
    rl->pos++;
    rl->line[rl->len] = '\0';

    if (rl->pos == rl->len)
        rl_write(rl, &c, 1);          /* simple append — fast path */
    else
        rl_redraw(rl);                 /* mid-line insert */
}

static void backspace(mk_readline_t *rl) {
    if (rl->pos == 0) return;
    rl->pos--;
    memmove(&rl->line[rl->pos], &rl->line[rl->pos + 1],
            rl->len - rl->pos - 1);
    rl->len--;
    rl->line[rl->len] = '\0';
    rl_redraw(rl);
}

static void delete_at_cursor(mk_readline_t *rl) {
    if (rl->pos >= rl->len) return;
    memmove(&rl->line[rl->pos], &rl->line[rl->pos + 1],
            rl->len - rl->pos - 1);
    rl->len--;
    rl->line[rl->len] = '\0';
    rl_redraw(rl);
}

static void move_left(mk_readline_t *rl) {
    if (rl->pos > 0) { rl->pos--; rl_puts(rl, "\033[D"); }
}

static void move_right(mk_readline_t *rl) {
    if (rl->pos < rl->len) { rl->pos++; rl_puts(rl, "\033[C"); }
}

static void move_home(mk_readline_t *rl) {
    rl->pos = 0;
    rl_redraw(rl);
}

static void move_end(mk_readline_t *rl) {
    if (rl->pos < rl->len) {
        char seq[16];
        int n = snprintf(seq, sizeof(seq), "\033[%uC",
                         (unsigned)(rl->len - rl->pos));
        rl_write(rl, seq, (size_t)n);
        rl->pos = rl->len;
    }
}

static void kill_line(mk_readline_t *rl) {
    rl->len = 0;
    rl->pos = 0;
    rl->line[0] = '\0';
    rl_redraw(rl);
}

static void kill_to_end(mk_readline_t *rl) {
    rl->len = rl->pos;
    rl->line[rl->len] = '\0';
    rl_puts(rl, "\033[K");
}

static void kill_word_back(mk_readline_t *rl) {
    if (rl->pos == 0) return;
    size_t end = rl->pos;
    while (rl->pos > 0 && rl->line[rl->pos - 1] == ' ') rl->pos--;
    while (rl->pos > 0 && rl->line[rl->pos - 1] != ' ') rl->pos--;
    memmove(&rl->line[rl->pos], &rl->line[end], rl->len - end);
    rl->len -= (end - rl->pos);
    rl->line[rl->len] = '\0';
    rl_redraw(rl);
}

/* ── Public API ──────────────────────────────────────────────────── */

void mk_rl_init(mk_readline_t *rl, const char *prompt,
                mk_rl_write_fn write_cb, void *ctx) {
    memset(rl, 0, sizeof(*rl));
    rl->prompt     = prompt ? prompt : "> ";
    rl->prompt_len = strlen(rl->prompt);
    rl->write_cb   = write_cb;
    rl->write_ctx  = ctx;
}

void mk_rl_prompt(mk_readline_t *rl) {
    rl->len         = 0;
    rl->pos         = 0;
    rl->line[0]     = '\0';
    rl->hist_browse = 0;
    rl->esc         = 0;
    rl_write(rl, rl->prompt, rl->prompt_len);
}

int mk_rl_feed(mk_readline_t *rl, uint8_t ch) {
    /* CRLF dedup: ignore \n immediately after \r */
    bool was_cr = rl->last_cr;
    rl->last_cr = false;

    /* ── Escape sequence state machine ──────────────────────────── */
    if (rl->esc == 1) {                        /* got ESC */
        if (ch == '[') { rl->esc = 2; rl->esc_num = 0; return MK_RL_CONTINUE; }
        if (ch == 'O') { rl->esc = 4; return MK_RL_CONTINUE; }  /* SS3 */
        rl->esc = 0;
        return MK_RL_CONTINUE;
    }
    if (rl->esc == 2) {                        /* got ESC [ */
        if (ch >= '0' && ch <= '9') {
            rl->esc_num = ch - '0';
            rl->esc = 3;
            return MK_RL_CONTINUE;
        }
        rl->esc = 0;
        switch (ch) {
        case 'A': history_browse_to(rl, rl->hist_browse + 1); break;
        case 'B':
            if (rl->hist_browse > 0)
                history_browse_to(rl, rl->hist_browse - 1);
            break;
        case 'C': move_right(rl); break;
        case 'D': move_left(rl);  break;
        case 'H': move_home(rl);  break;
        case 'F': move_end(rl);   break;
        default: break;
        }
        return MK_RL_CONTINUE;
    }
    if (rl->esc == 3) {                        /* got ESC [ digit */
        rl->esc = 0;
        if (ch == '~') {
            if (rl->esc_num == 3) delete_at_cursor(rl);
            /* 1~=Home, 4~=End also handled above via H/F */
        }
        return MK_RL_CONTINUE;
    }
    if (rl->esc == 4) {                        /* SS3 (ESC O) */
        rl->esc = 0;
        switch (ch) {
        case 'H': move_home(rl); break;
        case 'F': move_end(rl);  break;
        default: break;
        }
        return MK_RL_CONTINUE;
    }

    /* ── Normal character processing ────────────────────────────── */
    switch (ch) {
    case 27:                                   /* ESC */
        rl->esc = 1;
        return MK_RL_CONTINUE;

    case '\r':
    case '\n':
        if (ch == '\n' && was_cr) return MK_RL_CONTINUE;
        rl->last_cr = (ch == '\r');
        rl->line[rl->len] = '\0';
        rl_puts(rl, "\n");
        rl->hist_browse = 0;
        return MK_RL_DONE;

    case 0x7F:                                 /* DEL  (backspace key) */
    case 0x08:                                 /* BS   */
        backspace(rl);
        return MK_RL_CONTINUE;

    case 1:  move_home(rl);       return MK_RL_CONTINUE;  /* Ctrl+A */
    case 2:  move_left(rl);       return MK_RL_CONTINUE;  /* Ctrl+B */
    case 5:  move_end(rl);        return MK_RL_CONTINUE;  /* Ctrl+E */
    case 6:  move_right(rl);      return MK_RL_CONTINUE;  /* Ctrl+F */
    case 11: kill_to_end(rl);     return MK_RL_CONTINUE;  /* Ctrl+K */
    case 21: kill_line(rl);       return MK_RL_CONTINUE;  /* Ctrl+U */
    case 23: kill_word_back(rl);  return MK_RL_CONTINUE;  /* Ctrl+W */

    case 3:                                    /* Ctrl+C */
        rl->len = 0;
        rl->pos = 0;
        rl->line[0] = '\0';
        rl->hist_browse = 0;
        rl_puts(rl, "^C\n");
        mk_rl_prompt(rl);
        return MK_RL_CONTINUE;

    case 4:                                    /* Ctrl+D */
        if (rl->len == 0) return MK_RL_EOF;
        delete_at_cursor(rl);
        return MK_RL_CONTINUE;

    case 12:                                   /* Ctrl+L  clear screen */
        rl_puts(rl, "\033[2J\033[H");
        mk_rl_prompt(rl);
        if (rl->len > 0) rl_redraw(rl);
        return MK_RL_CONTINUE;

    default:
        if (ch >= 0x20 && ch < 0x7F)
            insert_char(rl, (char)ch);
        return MK_RL_CONTINUE;
    }
}

void mk_rl_history_add(mk_readline_t *rl, const char *line) {
    if (!line || line[0] == '\0') return;

    /* Skip duplicate of most recent entry */
    if (rl->hist_count > 0) {
        size_t last = (rl->hist_next + MK_RL_MAX_HISTORY - 1)
                          % MK_RL_MAX_HISTORY;
        if (strcmp(rl->history[last], line) == 0) return;
    }

    strncpy(rl->history[rl->hist_next], line, MK_RL_MAX_LINE - 1);
    rl->history[rl->hist_next][MK_RL_MAX_LINE - 1] = '\0';
    rl->hist_next = (rl->hist_next + 1) % MK_RL_MAX_HISTORY;
    rl->hist_count++;
}

const char *mk_rl_history_get(const mk_readline_t *rl, size_t idx) {
    size_t ri = ring_idx(rl, idx);
    if (ri == (size_t)-1) return NULL;
    return rl->history[ri];
}

size_t mk_rl_history_count(const mk_readline_t *rl) {
    return rl->hist_count < MK_RL_MAX_HISTORY
               ? rl->hist_count : MK_RL_MAX_HISTORY;
}
