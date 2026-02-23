/* shell_actor.c — native C interactive shell for the microkernel.
 *
 * Receives MSG_SHELL_INPUT lines from a console actor (which watches
 * stdin or a serial port) and dispatches commands.  Output goes to
 * stdout.  Cloud/AI commands (ai, embed, sql, queue) are separate
 * loadable WASM programs, not built into the shell.
 */

#include "microkernel/shell.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#ifdef HAVE_WASM
#include "microkernel/wasm_actor.h"
#endif

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#endif

/* ── Shell state ─────────────────────────────────────────────────── */

typedef struct {
    bool       pending_call;
    timer_id_t call_timer;
    actor_id_t console;        /* console actor (learned from first input) */
} shell_state_t;

/* ── Helpers ─────────────────────────────────────────────────────── */

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Copy next whitespace-delimited word into buf; return pointer past it. */
static const char *next_word(const char *s, char *buf, size_t buf_sz) {
    s = skip_ws(s);
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < buf_sz - 1)
        buf[i++] = *s++;
    buf[i] = '\0';
    return s;
}

/* Try actor_lookup(name), then parse as decimal uint64. */
static actor_id_t resolve_target(runtime_t *rt, const char *arg) {
    actor_id_t id = actor_lookup(rt, arg);
    if (id != ACTOR_ID_INVALID) return id;
    char *end;
    unsigned long long val = strtoull(arg, &end, 10);
    if (end != arg && *end == '\0')
        return (actor_id_t)val;
    return ACTOR_ID_INVALID;
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "AABBCC" hex string into bytes.  Returns byte count or -1. */
static int parse_hex(const char *hex, uint8_t *out, size_t max) {
    size_t slen = strlen(hex);
    if (slen % 2 != 0) return -1;
    size_t n = slen / 2;
    if (n > max) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

/* Send MSG_SHELL_PROMPT to the console actor so it can show the
   readline prompt.  Falls back to printf if console is unknown. */
static void send_prompt(runtime_t *rt, shell_state_t *sh) {
    if (sh->console != ACTOR_ID_INVALID) {
        actor_send(rt, sh->console, MSG_SHELL_PROMPT, NULL, 0);
    } else {
        actor_id_t c = actor_lookup(rt, "console");
        if (c != ACTOR_ID_INVALID) {
            sh->console = c;
            actor_send(rt, c, MSG_SHELL_PROMPT, NULL, 0);
        } else {
            printf("> ");
            fflush(stdout);
        }
    }
}

/* Print a message payload (hex or text). */
static void print_payload(const message_t *msg) {
    if (!msg->payload || msg->payload_size == 0) return;
    const uint8_t *p = msg->payload;
    bool printable = true;
    for (uint32_t i = 0; i < msg->payload_size; i++) {
        if (p[i] < 0x20 && p[i] != '\n' && p[i] != '\r' && p[i] != '\t') {
            printable = false;
            break;
        }
    }
    if (printable) {
        printf(" \"%.*s\"", (int)msg->payload_size, (const char *)p);
    } else {
        printf(" [");
        for (uint32_t i = 0; i < msg->payload_size && i < 32; i++) {
            if (i) printf(" ");
            printf("%02x", p[i]);
        }
        if (msg->payload_size > 32) printf(" ...");
        printf("]");
    }
}

/* ── Command handlers ────────────────────────────────────────────── */

static void cmd_help(void) {
    printf(
        "Commands:\n"
        "  help              Show this help\n"
        "  list              List active actors\n"
        "  self              Show own actor ID\n"
        "  whoami            Show node identity\n"
        "  register <name>   Register name for shell\n"
        "  lookup <name>     Look up actor by name\n"
        "  send <to> <type>  [payload|x:hex]   Send message\n"
        "  call <to> <type>  [payload|x:hex]   Send + wait for reply\n"
        "  info              System info, heap, actors\n"
        "  stop <target>     Stop an actor\n"
#ifdef HAVE_WASM
        "  load <path>       Load WASM actor from file\n"
        "  reload <name> <path>  Hot-reload WASM actor\n"
#endif
        "  ls [/prefix]      List namespace entries\n"
#if SOC_WIFI_SUPPORTED || !defined(ESP_PLATFORM)
        "  mount <host>[:<port>]  Connect to remote node\n"
#endif
        "  caps [target]     Query node capabilities\n"
        "  exit              Exit shell\n"
    );
}

static const char *status_str(actor_status_t s) {
    switch (s) {
    case ACTOR_IDLE:    return "idle";
    case ACTOR_READY:   return "ready";
    case ACTOR_RUNNING: return "running";
    case ACTOR_STOPPED: return "stopped";
    default:            return "?";
    }
}

static void cmd_list(runtime_t *rt) {
    actor_info_t info[64];
    size_t n = runtime_actor_info(rt, info, 64);

    printf("  %-4s %-17s %-8s %-5s %s\n",
           "SEQ", "ID", "STATUS", "MBOX", "NAME");

    for (size_t i = 0; i < n; i++) {
        uint32_t seq = actor_id_seq(info[i].id);
        char names[512];
        size_t nlen = actor_reverse_lookup_all(rt, info[i].id,
                                                names, sizeof(names));
        const char *first = nlen > 0 ? names : "-";
        size_t flen = nlen;
        const char *comma = nlen > 0 ? strchr(names, ',') : NULL;
        if (comma) flen = (size_t)(comma - names);

        char mbox[12];
        snprintf(mbox, sizeof(mbox), "%zu/%zu",
                 info[i].mailbox_used, info[i].mailbox_cap);

        printf("  %-4u 0x%015" PRIx64 "  %-8s %-5s %.*s\n",
               (unsigned)seq,
               (uint64_t)info[i].id,
               status_str(info[i].status),
               mbox,
               (int)flen, first);

        if (comma) {
            const char *p = comma + 2;
            while (*p) {
                const char *next = strchr(p, ',');
                int len = next ? (int)(next - p) : (int)strlen(p);
                printf("  %4s %-17s %-8s %-5s %.*s\n",
                       "", "", "", "", len, p);
                p = next ? next + 2 : p + len;
            }
        }
    }
}

static void cmd_send(runtime_t *rt, const char *args, bool wait_reply,
                     shell_state_t *sh) {
    char target_str[64], type_str[16];
    args = next_word(args, target_str, sizeof(target_str));
    args = next_word(args, type_str, sizeof(type_str));
    if (target_str[0] == '\0' || type_str[0] == '\0') {
        printf("Usage: %s <target> <type> [data|x:hex]\n",
               wait_reply ? "call" : "send");
        return;
    }

    actor_id_t target = resolve_target(rt, target_str);
    if (target == ACTOR_ID_INVALID) {
        printf("Unknown target: %s\n", target_str);
        return;
    }

    char *end;
    unsigned long type_val = strtoul(type_str, &end, 0);
    if (end == type_str) {
        printf("Invalid type: %s\n", type_str);
        return;
    }

    /* Parse optional payload */
    const char *data = skip_ws(args);
    uint8_t hex_buf[256];
    const void *payload = NULL;
    size_t payload_size = 0;

    if (data[0] == 'x' && data[1] == ':') {
        int n = parse_hex(data + 2, hex_buf, sizeof(hex_buf));
        if (n < 0) {
            printf("Bad hex payload\n");
            return;
        }
        payload = hex_buf;
        payload_size = (size_t)n;
    } else if (data[0] != '\0') {
        payload = data;
        payload_size = strlen(data);
    }

    actor_send(rt, target, (msg_type_t)type_val, payload, payload_size);

    if (wait_reply) {
        sh->pending_call = true;
        sh->call_timer = actor_set_timer(rt, 5000, false);
    } else {
        printf("Sent type=%lu to %" PRIu64 " (%zu bytes)\n",
               type_val, (uint64_t)target, payload_size);
    }
}

static void cmd_stop(runtime_t *rt, const char *args) {
    char target_str[64];
    next_word(args, target_str, sizeof(target_str));
    if (target_str[0] == '\0') {
        printf("Usage: stop <name-or-id>\n");
        return;
    }
    actor_id_t target = resolve_target(rt, target_str);
    if (target == ACTOR_ID_INVALID) {
        printf("Unknown target: %s\n", target_str);
        return;
    }
    actor_stop(rt, target);
    printf("Stopped %" PRIu64 "\n", (uint64_t)target);
}

static void cmd_register(runtime_t *rt, const char *args) {
    char name[64];
    next_word(args, name, sizeof(name));
    if (name[0] == '\0') {
        printf("Usage: register <name>\n");
        return;
    }
    if (actor_register_name(rt, name, actor_self(rt)))
        printf("Registered as '%s'\n", name);
    else
        printf("Failed to register '%s'\n", name);
}

static void cmd_lookup(runtime_t *rt, const char *args) {
    char name[64];
    next_word(args, name, sizeof(name));
    if (name[0] == '\0') {
        printf("Usage: lookup <name>\n");
        return;
    }
    actor_id_t id = actor_lookup(rt, name);
    if (id != ACTOR_ID_INVALID)
        printf("%" PRIu64 "\n", (uint64_t)id);
    else
        printf("Not found: %s\n", name);
}

static void cmd_ls(runtime_t *rt, const char *args) {
    char prefix[128];
    next_word(args, prefix, sizeof(prefix));
    if (prefix[0] == '\0') strcpy(prefix, "/");

    char buf[1024];
    size_t n = ns_list_paths(rt, prefix, buf, sizeof(buf));
    if (n == 0) {
        printf("(no entries under '%s')\n", prefix);
        return;
    }
    /* ns_list_paths returns newline-separated entries */
    printf("%.*s", (int)n, buf);
    /* ensure trailing newline */
    if (n > 0 && buf[n - 1] != '\n') printf("\n");
}

#ifdef HAVE_WASM
static void cmd_load(runtime_t *rt, const char *args) {
    char path[256];
    next_word(args, path, sizeof(path));
    if (path[0] == '\0') {
        printf("Usage: load <path>\n");
        return;
    }

    /* Read file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Cannot open: %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 256 * 1024) {
        printf("Invalid file size: %ld\n", fsize);
        fclose(f);
        return;
    }

    uint8_t *wasm = malloc((size_t)fsize);
    if (!wasm) {
        printf("Out of memory (%ld bytes)\n", fsize);
        fclose(f);
        return;
    }
    size_t nread = fread(wasm, 1, (size_t)fsize, f);
    fclose(f);
    if ((long)nread != fsize) {
        printf("Read error: got %zu / %ld bytes\n", nread, fsize);
        free(wasm);
        return;
    }

    /* Spawn */
    actor_id_t id = actor_spawn_wasm(rt, wasm, (size_t)fsize, 32,
                                      WASM_DEFAULT_STACK_SIZE,
                                      0, FIBER_STACK_NONE);
    free(wasm);
    if (id == ACTOR_ID_INVALID) {
        printf("Failed to spawn WASM actor\n");
        return;
    }

    /* Extract name from path: basename minus .wasm extension */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    char name[64];
    size_t blen = strlen(base);
    if (blen > 5 && strcmp(base + blen - 5, ".wasm") == 0)
        blen -= 5;
    if (blen > sizeof(name) - 4) blen = sizeof(name) - 4; /* room for _99 */
    memcpy(name, base, blen);
    name[blen] = '\0';

    /* Register with fallback suffix */
    char reg_name[64];
    char base_name[60];
    strncpy(base_name, name, sizeof(base_name) - 1);
    base_name[sizeof(base_name) - 1] = '\0';
    strncpy(reg_name, base_name, sizeof(reg_name) - 1);
    reg_name[sizeof(reg_name) - 1] = '\0';
    if (!actor_register_name(rt, reg_name, id)) {
        for (int i = 1; i <= 99; i++) {
            snprintf(reg_name, sizeof(reg_name), "%s_%d", base_name, i);
            if (actor_register_name(rt, reg_name, id)) break;
        }
    }

    printf("Spawned %" PRIu64 " as '%s'\n", (uint64_t)id, reg_name);
}

static void cmd_reload(runtime_t *rt, const char *args) {
    char name[64], path[256];
    args = next_word(args, name, sizeof(name));
    next_word(args, path, sizeof(path));
    if (name[0] == '\0' || path[0] == '\0') {
        printf("Usage: reload <name> <path>\n");
        return;
    }

    actor_id_t target = actor_lookup(rt, name);
    if (target == ACTOR_ID_INVALID) {
        printf("Not found: %s\n", name);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Cannot open: %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 256 * 1024) {
        printf("Invalid file size: %ld\n", fsize);
        fclose(f);
        return;
    }

    uint8_t *wasm = malloc((size_t)fsize);
    if (!wasm) {
        printf("Out of memory (%ld bytes)\n", fsize);
        fclose(f);
        return;
    }
    fread(wasm, 1, (size_t)fsize, f);
    fclose(f);

    actor_id_t new_id;
    reload_result_t rc = actor_reload_wasm(rt, target, wasm, (size_t)fsize,
                                            &new_id);
    free(wasm);

    if (rc == RELOAD_OK)
        printf("Reloaded '%s' → %" PRIu64 "\n", name, (uint64_t)new_id);
    else
        printf("Reload failed (code %d)\n", (int)rc);
}
#endif /* HAVE_WASM */

#if SOC_WIFI_SUPPORTED || !defined(ESP_PLATFORM)
static void cmd_mount(runtime_t *rt, const char *args) {
    char hostport[256];
    next_word(args, hostport, sizeof(hostport));
    if (hostport[0] == '\0') {
        printf("Usage: mount <host>[:<port>]\n");
        return;
    }

    /* Split host:port */
    char host[256];
    uint16_t port = MK_MOUNT_PORT;
    strncpy(host, hostport, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = (uint16_t)atoi(colon + 1);
    }

    mount_result_t result;
    int rc = ns_mount_connect(rt, host, port, &result);
    if (rc == 0)
        printf("Mounted: %s\n", result.identity);
    else
        printf("Mount failed (rc=%d)\n", rc);
}
#endif

/* ── info command ─────────────────────────────────────────────────── */

#ifdef ESP_PLATFORM
static void print_kb(size_t bytes) {
    if (bytes >= 1024)
        printf("%zu.%zu KB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    else
        printf("%zu B", bytes);
}
#endif

static void cmd_info(runtime_t *rt) {
    /* ── Header line ──────────────────────────────────────────────── */
    printf("%s", mk_node_identity());
#ifdef ESP_PLATFORM
    int64_t us = esp_timer_get_time();
    printf(" | up %lld.%llds", (long long)(us / 1000000),
           (long long)((us / 1000) % 1000) / 100);
#endif
    printf(" | ");
#ifdef HAVE_WASM
    printf("wasm ");
#endif
#ifdef HAVE_TLS
    printf("tls ");
#endif
    printf("http\n");

    /* ── Heap (ESP32 only) ────────────────────────────────────────── */
#ifdef ESP_PLATFORM
    {
        size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t avail = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t wmark = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        size_t blk   = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        printf("\nHeap: ");
        print_kb(avail); printf(" free / "); print_kb(total);
        printf(" (min "); print_kb(wmark);
        printf(", largest "); print_kb(blk); printf(")\n");

        size_t dt = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t df = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        printf("  DRAM:  "); print_kb(df); printf(" / "); print_kb(dt); printf("\n");

        size_t pt = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        if (pt > 0) {
            size_t pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            printf("  PSRAM: "); print_kb(pf); printf(" / "); print_kb(pt); printf("\n");
        }
    }
#endif

    /* ── Actor table ──────────────────────────────────────────────── */
    actor_info_t info[64];
    size_t n = runtime_actor_info(rt, info, 64);

    printf("\nActors: %zu active\n", n);
    printf("  %-4s %-17s %-8s %-5s %-17s %s\n",
           "SEQ", "ID", "STATUS", "MBOX", "PARENT", "NAME");

    for (size_t i = 0; i < n; i++) {
        uint32_t seq = actor_id_seq(info[i].id);
        char names[512];
        size_t nlen = actor_reverse_lookup_all(rt, info[i].id,
                                                names, sizeof(names));
        const char *first = nlen > 0 ? names : "-";
        size_t flen = nlen;
        const char *comma = nlen > 0 ? strchr(names, ',') : NULL;
        if (comma) flen = (size_t)(comma - names);

        char mbox[12];
        snprintf(mbox, sizeof(mbox), "%zu/%zu",
                 info[i].mailbox_used, info[i].mailbox_cap);

        char parent[20];
        if (info[i].parent != ACTOR_ID_INVALID)
            snprintf(parent, sizeof(parent), "0x%015" PRIx64,
                     (uint64_t)info[i].parent);
        else
            snprintf(parent, sizeof(parent), "---");

        printf("  %-4u 0x%015" PRIx64 "  %-8s %-5s %-17s %.*s\n",
               (unsigned)seq,
               (uint64_t)info[i].id,
               status_str(info[i].status),
               mbox,
               parent,
               (int)flen, first);

        if (comma) {
            const char *p = comma + 2;
            while (*p) {
                const char *next = strchr(p, ',');
                int len = next ? (int)(next - p) : (int)strlen(p);
                printf("  %4s %-17s %-8s %-5s %-17s %.*s\n",
                       "", "", "", "", "", len, p);
                p = next ? next + 2 : p + len;
            }
        }
    }

    size_t tc = runtime_get_transport_count(rt);
    if (tc > 0)
        printf("\nTransports: %zu\n", tc);
}

static void cmd_caps(runtime_t *rt, const char *args) {
    char target_str[64];
    next_word(args, target_str, sizeof(target_str));

    actor_id_t target;
    if (target_str[0] == '\0') {
        target = actor_lookup(rt, "caps");
        if (target == ACTOR_ID_INVALID) {
            printf("No caps actor found\n");
            return;
        }
    } else {
        target = resolve_target(rt, target_str);
        if (target == ACTOR_ID_INVALID) {
            printf("Unknown target: %s\n", target_str);
            return;
        }
    }

    /* MSG_CAPS_REQUEST = 0xFF00001D */
    actor_send(rt, target, 0xFF00001D, NULL, 0);
    printf("(caps request sent, reply will appear below)\n");
}

/* ── Command dispatch ────────────────────────────────────────────── */

static void dispatch_command(runtime_t *rt, shell_state_t *sh,
                             const char *line) {
    char cmd[32];
    const char *rest = next_word(line, cmd, sizeof(cmd));

    if (cmd[0] == '\0') return;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "list") == 0) {
        cmd_list(rt);
    } else if (strcmp(cmd, "self") == 0) {
        printf("%" PRIu64 "\n", (uint64_t)actor_self(rt));
    } else if (strcmp(cmd, "whoami") == 0) {
        printf("%s (actor %" PRIu64 ")\n",
               mk_node_identity(), (uint64_t)actor_self(rt));
    } else if (strcmp(cmd, "register") == 0) {
        cmd_register(rt, rest);
    } else if (strcmp(cmd, "lookup") == 0) {
        cmd_lookup(rt, rest);
    } else if (strcmp(cmd, "send") == 0) {
        cmd_send(rt, rest, false, sh);
    } else if (strcmp(cmd, "call") == 0) {
        cmd_send(rt, rest, true, sh);
    } else if (strcmp(cmd, "stop") == 0) {
        cmd_stop(rt, rest);
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(rt, rest);
    } else if (strcmp(cmd, "info") == 0 || strcmp(cmd, "top") == 0) {
        cmd_info(rt);
    } else if (strcmp(cmd, "caps") == 0) {
        cmd_caps(rt, rest);
    } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        printf("Bye!\n");
        runtime_stop(rt);
#ifdef HAVE_WASM
    } else if (strcmp(cmd, "load") == 0) {
        cmd_load(rt, rest);
    } else if (strcmp(cmd, "reload") == 0) {
        cmd_reload(rt, rest);
#endif
#if SOC_WIFI_SUPPORTED || !defined(ESP_PLATFORM)
    } else if (strcmp(cmd, "mount") == 0) {
        cmd_mount(rt, rest);
#endif
    } else {
        printf("Unknown command: %s (type 'help')\n", cmd);
    }
}

/* ── Shell behavior ──────────────────────────────────────────────── */

static bool shell_behavior(runtime_t *rt, actor_t *self,
                            message_t *msg, void *state_ptr) {
    (void)self;
    shell_state_t *sh = state_ptr;

    if (msg->type == MSG_SHELL_INIT) {
        actor_register_name(rt, "shell", actor_self(rt));
        sh->console = ACTOR_ID_INVALID;
        printf("\nmk-shell v0.3 (native)\n"
               "Type 'help' for commands.\n\n");
        fflush(stdout);
        /* Console will show the first prompt via its own MSG_SHELL_INIT */
        return true;
    }

    if (msg->type == MSG_SHELL_INPUT) {
        /* Remember who sends us input */
        sh->console = msg->source;

        /* Command line from console actor */
        char line[256];
        size_t n = msg->payload_size;
        if (n > sizeof(line) - 1) n = sizeof(line) - 1;
        memcpy(line, msg->payload, n);
        line[n] = '\0';

        dispatch_command(rt, sh, line);

        if (!sh->pending_call)
            send_prompt(rt, sh);
        return true;
    }

    if (msg->type == MSG_TIMER) {
        /* Call timeout */
        if (sh->pending_call) {
            printf("Timeout (5s)\n");
            fflush(stdout);
            sh->pending_call = false;
            send_prompt(rt, sh);
        }
        return true;
    }

    /* Any other message: call reply or unsolicited */
    if (sh->pending_call) {
        actor_cancel_timer(rt, sh->call_timer);
        printf("[reply] type=%lu from=%" PRIu64 " size=%zu",
               (unsigned long)msg->type, (uint64_t)msg->source,
               (size_t)msg->payload_size);
        print_payload(msg);
        printf("\n");
        fflush(stdout);
        sh->pending_call = false;
        send_prompt(rt, sh);
    } else {
        printf("[msg] type=%lu from=%" PRIu64 " size=%zu",
               (unsigned long)msg->type, (uint64_t)msg->source,
               (size_t)msg->payload_size);
        print_payload(msg);
        printf("\n");
        fflush(stdout);
        send_prompt(rt, sh);
    }
    return true;
}

/* ── Public API ──────────────────────────────────────────────────── */

actor_id_t shell_actor_init(runtime_t *rt) {
    static shell_state_t state;
    memset(&state, 0, sizeof(state));
    return actor_spawn(rt, shell_behavior, &state, NULL, 32);
}
