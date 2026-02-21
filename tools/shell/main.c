#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/wasm_actor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/poll.h>

#define MSG_SHELL_INPUT      100
#define MSG_INIT             101
#define MSG_SPAWN_REQUEST    102
#define MSG_SPAWN_RESPONSE   103

/* ── Console actor: watches stdin, sends lines, handles spawn requests ── */

typedef struct {
    actor_id_t shell;
    bool       watching;
    char       line_buf[1024];
    size_t     line_len;
} console_state_t;

static bool console_behavior(runtime_t *rt, actor_t *self,
                              message_t *msg, void *state_ptr) {
    (void)self;
    console_state_t *cs = state_ptr;

    if (msg->type == MSG_INIT) {
        actor_register_name(rt, "console", actor_self(rt));
        actor_watch_fd(rt, STDIN_FILENO, POLLIN);
        cs->watching = true;
        return true;
    }

    if (msg->type == MSG_SPAWN_REQUEST) {
        /* Payload = raw WASM bytes; spawn a new WASM actor */
        actor_id_t new_id = ACTOR_ID_INVALID;
        if (msg->payload && msg->payload_size > 0) {
            new_id = actor_spawn_wasm(rt, msg->payload, msg->payload_size,
                                       32,
                                       WASM_DEFAULT_STACK_SIZE,
                                       WASM_DEFAULT_HEAP_SIZE,
                                       FIBER_STACK_SMALL);
        }
        /* Send response back to requester */
        actor_send(rt, msg->source, MSG_SPAWN_RESPONSE,
                   &new_id, sizeof(new_id));
        return true;
    }

    if (msg->type == MSG_FD_EVENT) {
        const fd_event_payload_t *ev = msg->payload;
        if (ev->fd != STDIN_FILENO) return true;

        /* Read available bytes */
        char tmp[256];
        ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n <= 0) {
            /* EOF or error */
            actor_send(rt, cs->shell, 0, NULL, 0); /* stop shell */
            runtime_stop(rt);
            return false;
        }

        /* Accumulate into line buffer, send complete lines */
        for (ssize_t i = 0; i < n; i++) {
            if (tmp[i] == '\n' || cs->line_len >= sizeof(cs->line_buf) - 1) {
                cs->line_buf[cs->line_len] = '\0';
                actor_send(rt, cs->shell, MSG_SHELL_INPUT,
                           cs->line_buf, cs->line_len);
                cs->line_len = 0;
            } else {
                cs->line_buf[cs->line_len++] = tmp[i];
            }
        }
        return true;
    }

    return true;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *wasm_path = NULL;

    if (argc > 1) {
        wasm_path = argv[1];
    } else {
        /* Default: look for shell.wasm next to the binary */
        static char default_path[4096];
        ssize_t len = readlink("/proc/self/exe", default_path,
                                sizeof(default_path) - 20);
        if (len > 0) {
            /* Strip binary name, append shell.wasm */
            while (len > 0 && default_path[len - 1] != '/') len--;
            strcpy(default_path + len, "shell.wasm");
            wasm_path = default_path;
        } else {
            wasm_path = "shell.wasm";
        }
    }

    if (!wasm_actors_init()) {
        fprintf(stderr, "error: wasm_actors_init failed\n");
        return 1;
    }

    runtime_t *rt = runtime_init(1, 64);
    if (!rt) {
        fprintf(stderr, "error: runtime_init failed\n");
        wasm_actors_cleanup();
        return 1;
    }

    /* Spawn shell WASM actor from file */
    actor_id_t shell = actor_spawn_wasm_file(rt, wasm_path, 32,
                                               WASM_DEFAULT_STACK_SIZE,
                                               1024 * 1024,     /* 1MB heap */
                                               FIBER_STACK_LARGE);
    if (shell == ACTOR_ID_INVALID) {
        fprintf(stderr, "error: cannot load %s\n", wasm_path);
        runtime_destroy(rt);
        wasm_actors_cleanup();
        return 1;
    }

    /* Spawn console actor */
    static console_state_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.shell = shell;
    actor_id_t console = actor_spawn(rt, console_behavior, &cs, NULL, 16);
    if (console == ACTOR_ID_INVALID) {
        fprintf(stderr, "error: cannot spawn console actor\n");
        runtime_destroy(rt);
        wasm_actors_cleanup();
        return 1;
    }

    /* Send init messages */
    actor_send(rt, shell, MSG_INIT, NULL, 0);
    actor_send(rt, console, MSG_INIT, NULL, 0);

    /* Run event loop */
    runtime_run(rt);

    runtime_destroy(rt);
    wasm_actors_cleanup();
    return 0;
}
