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
#include "microkernel/midi.h"
#include "microkernel/midi_monitor.h"
#include "microkernel/arpeggiator.h"
#include "microkernel/sequencer.h"
#include "midi_hal.h"
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
        "  midi [help]       MIDI commands (configure, send, monitor, arp)\n"
        "  seq [help]        Sequencer commands (start, stop, tempo, demo)\n"
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

        printf("  %-4u 0x%015" PRIx64 " %-8s %-5s %.*s\n",
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

        printf("  %-4u 0x%015" PRIx64 " %-8s %-5s %-17s %.*s\n",
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

/* ── MIDI note sequence player actor ─────────────────────────────── */

#define PLAYER_MAX_NOTES  128
#define PLAYER_NEW_SEQ    2   /* message: load new sequence and start playing */

typedef struct __attribute__((packed)) {
    uint8_t  notes[PLAYER_MAX_NOTES];
    uint8_t  count;
    uint8_t  vel;
    uint8_t  ch;
    uint8_t  _pad;
    uint32_t interval_ms;
} player_seq_payload_t;

typedef struct {
    actor_id_t midi_id;
    uint8_t    notes[PLAYER_MAX_NOTES];
    int        count;
    int        pos;
    uint8_t    vel;
    uint8_t    ch;
    uint8_t    last_note;
    bool       note_on;
    uint64_t   interval_ms;
    timer_id_t timer;
    bool       timer_running;
} player_state_t;

static void player_note_off(player_state_t *p, runtime_t *rt) {
    if (!p->note_on) return;
    midi_send_payload_t off;
    memset(&off, 0, sizeof(off));
    off.status = (uint8_t)(0x80 | (p->ch & 0x0F));
    off.data1  = p->last_note & 0x7F;
    off.data2  = 0;
    actor_send(rt, p->midi_id, MSG_MIDI_SEND, &off, sizeof(off));
    p->note_on = false;
}

static void player_stop_timer(player_state_t *p, runtime_t *rt) {
    if (!p->timer_running) return;
    actor_cancel_timer(rt, p->timer);
    p->timer_running = false;
}

static bool player_behavior(runtime_t *rt, actor_t *self,
                             message_t *msg, void *state) {
    (void)self;
    player_state_t *p = state;

    if (msg->type == PLAYER_NEW_SEQ) {
        /* Load new sequence — stop any current playback first */
        player_note_off(p, rt);
        player_stop_timer(p, rt);

        if (msg->payload_size < 4) return true; /* bad payload */
        const player_seq_payload_t *seq =
            (const player_seq_payload_t *)msg->payload;

        memcpy(p->notes, seq->notes, seq->count);
        p->count = seq->count;
        p->vel = seq->vel;
        p->ch = seq->ch;
        p->interval_ms = seq->interval_ms;
        p->pos = 0;

        /* Start periodic timer */
        p->timer = actor_set_timer(rt, p->interval_ms, true);
        p->timer_running = true;
        return true;
    }

    if (msg->type == MSG_TIMER) {
        /* Note-off for previous note */
        player_note_off(p, rt);

        if (p->pos >= p->count) {
            /* Sequence finished — stop timer, stay alive for reuse */
            player_stop_timer(p, rt);
            return true;
        }

        /* Note-on for next note (0 = rest) */
        uint8_t note = p->notes[p->pos];
        if (note > 0) {
            midi_send_payload_t on;
            memset(&on, 0, sizeof(on));
            on.status = (uint8_t)(0x90 | (p->ch & 0x0F));
            on.data1  = note & 0x7F;
            on.data2  = p->vel & 0x7F;
            actor_send(rt, p->midi_id, MSG_MIDI_SEND, &on, sizeof(on));
            p->last_note = note;
            p->note_on = true;
        }

        p->pos++;
        return true;
    }

    return true;
}

/* ── MIDI commands ────────────────────────────────────────────────── */

/* Default SC16IS752 config for ESP32-P4-Pico */
#define MIDI_DEFAULT_I2C_PORT  0
#define MIDI_DEFAULT_I2C_ADDR  0x48
#define MIDI_DEFAULT_SDA       7
#define MIDI_DEFAULT_SCL       8
#define MIDI_DEFAULT_IRQ       3
#define MIDI_DEFAULT_RST       2
#define MIDI_DEFAULT_I2C_FREQ  400000

static const char *note_names[] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

static void midi_note_str(uint8_t note, char *buf, size_t sz) {
    int octave = (int)note / 12 - 1;
    snprintf(buf, sz, "%s%d", note_names[note % 12], octave);
}

static void cmd_midi(runtime_t *rt, const char *args, shell_state_t *sh) {
    char sub[32];
    args = next_word(args, sub, sizeof(sub));

    if (sub[0] == '\0' || strcmp(sub, "help") == 0) {
        printf(
            "MIDI commands:\n"
            "  midi configure [port addr sda scl irq rst freq]\n"
            "                    Configure SC16IS752 (defaults: 0 0x48 7 8 3 2 400000)\n"
            "  midi send <status> <d1> [d2]\n"
            "                    Send MIDI message (hex: 90 3C 7F = Note On C4)\n"
            "  midi note <ch> <note> <vel>\n"
            "                    Note On (vel=0 for Note Off)\n"
            "  midi cc <ch> <cc#> <val>\n"
            "                    Control Change\n"
            "  midi pc <ch> <program>\n"
            "                    Program Change\n"
            "  midi play <notes...> [--bpm 120] [--vel 100] [--ch 0]\n"
            "                    Play note sequence (0=rest)\n"
            "  midi stop         Stop player\n"
            "  midi monitor      Start MIDI monitor\n"
            "  midi arp [on|off|bpm N|pattern up|down|updown|random|octaves N]\n"
            "                    Arpeggiator control\n"
        );
        return;
    }

    /* ── midi configure ──────────────────────────────────────────── */
    if (strcmp(sub, "configure") == 0 || strcmp(sub, "config") == 0) {
        midi_config_payload_t cfg;
        memset(&cfg, 0, sizeof(cfg));

        char arg1[16];
        const char *peek = next_word(args, arg1, sizeof(arg1));

        if (arg1[0] != '\0') {
            /* Parse all 7 params: port addr sda scl irq rst freq */
            cfg.i2c_port   = (uint8_t)strtoul(arg1, NULL, 0);
            char a2[16], a3[16], a4[16], a5[16], a6[16], a7[16];
            peek = next_word(peek, a2, sizeof(a2));
            peek = next_word(peek, a3, sizeof(a3));
            peek = next_word(peek, a4, sizeof(a4));
            peek = next_word(peek, a5, sizeof(a5));
            peek = next_word(peek, a6, sizeof(a6));
            next_word(peek, a7, sizeof(a7));
            cfg.i2c_addr    = (uint8_t)strtoul(a2, NULL, 0);
            cfg.sda_pin     = (uint8_t)strtoul(a3, NULL, 0);
            cfg.scl_pin     = (uint8_t)strtoul(a4, NULL, 0);
            cfg.irq_pin     = (uint8_t)strtoul(a5, NULL, 0);
            cfg.rst_pin     = (uint8_t)strtoul(a6, NULL, 0);
            cfg.i2c_freq_hz = (uint32_t)strtoul(a7, NULL, 0);
            if (cfg.rst_pin == 0) cfg.rst_pin = 0xFF; /* 0 = none */
        } else {
            /* Defaults */
            cfg.i2c_port    = MIDI_DEFAULT_I2C_PORT;
            cfg.i2c_addr    = MIDI_DEFAULT_I2C_ADDR;
            cfg.sda_pin     = MIDI_DEFAULT_SDA;
            cfg.scl_pin     = MIDI_DEFAULT_SCL;
            cfg.irq_pin     = MIDI_DEFAULT_IRQ;
            cfg.rst_pin     = MIDI_DEFAULT_RST;
            cfg.i2c_freq_hz = MIDI_DEFAULT_I2C_FREQ;
        }

        printf("MIDI configure: I2C%d addr=0x%02X sda=%d scl=%d irq=%d rst=%s freq=%lu\n",
               cfg.i2c_port, cfg.i2c_addr, cfg.sda_pin, cfg.scl_pin,
               cfg.irq_pin,
               cfg.rst_pin == 0xFF ? "none" : "GPIO",
               (unsigned long)cfg.i2c_freq_hz);
        if (cfg.rst_pin != 0xFF)
            printf("  rst=GPIO%d\n", cfg.rst_pin);

        actor_id_t midi = actor_lookup(rt, "/node/hardware/midi");
        if (midi == ACTOR_ID_INVALID) {
            printf("MIDI actor not found\n");
            return;
        }

        actor_send(rt, midi, MSG_MIDI_CONFIGURE, &cfg, sizeof(cfg));
        sh->pending_call = true;
        sh->call_timer = actor_set_timer(rt, 3000, false);
        return;
    }

    /* ── midi send <status> <d1> [d2] ────────────────────────────── */
    if (strcmp(sub, "send") == 0) {
        char s1[8], s2[8], s3[8];
        args = next_word(args, s1, sizeof(s1));
        args = next_word(args, s2, sizeof(s2));
        next_word(args, s3, sizeof(s3));

        if (s1[0] == '\0' || s2[0] == '\0') {
            printf("Usage: midi send <status> <d1> [d2]\n");
            return;
        }

        midi_send_payload_t pay;
        memset(&pay, 0, sizeof(pay));
        pay.status = (uint8_t)strtoul(s1, NULL, 0);
        pay.data1  = (uint8_t)strtoul(s2, NULL, 0);
        pay.data2  = s3[0] ? (uint8_t)strtoul(s3, NULL, 0) : 0;

        actor_id_t midi = actor_lookup(rt, "/node/hardware/midi");
        if (midi == ACTOR_ID_INVALID) {
            printf("MIDI actor not found\n");
            return;
        }

        actor_send(rt, midi, MSG_MIDI_SEND, &pay, sizeof(pay));
        sh->pending_call = true;
        sh->call_timer = actor_set_timer(rt, 3000, false);
        return;
    }

    /* ── midi note <ch> <note> <vel> ─────────────────────────────── */
    if (strcmp(sub, "note") == 0) {
        char s1[8], s2[8], s3[8];
        args = next_word(args, s1, sizeof(s1));
        args = next_word(args, s2, sizeof(s2));
        next_word(args, s3, sizeof(s3));

        if (s1[0] == '\0' || s2[0] == '\0') {
            printf("Usage: midi note <channel 0-15> <note 0-127> <velocity 0-127>\n");
            return;
        }

        uint8_t ch  = (uint8_t)strtoul(s1, NULL, 0);
        uint8_t note = (uint8_t)strtoul(s2, NULL, 0);
        uint8_t vel  = s3[0] ? (uint8_t)strtoul(s3, NULL, 0) : 127;

        midi_send_payload_t pay;
        memset(&pay, 0, sizeof(pay));
        pay.status = (uint8_t)(0x90 | (ch & 0x0F));
        pay.data1  = note & 0x7F;
        pay.data2  = vel & 0x7F;

        char nstr[8];
        midi_note_str(note, nstr, sizeof(nstr));
        printf("Note %s ch=%d vel=%d\n", nstr, ch, vel);

        actor_id_t midi = actor_lookup(rt, "/node/hardware/midi");
        if (midi == ACTOR_ID_INVALID) {
            printf("MIDI actor not found\n");
            return;
        }

        actor_send(rt, midi, MSG_MIDI_SEND, &pay, sizeof(pay));
        sh->pending_call = true;
        sh->call_timer = actor_set_timer(rt, 3000, false);
        return;
    }

    /* ── midi cc <ch> <cc#> <val> ────────────────────────────────── */
    if (strcmp(sub, "cc") == 0) {
        char s1[8], s2[8], s3[8];
        args = next_word(args, s1, sizeof(s1));
        args = next_word(args, s2, sizeof(s2));
        next_word(args, s3, sizeof(s3));

        if (s1[0] == '\0' || s2[0] == '\0' || s3[0] == '\0') {
            printf("Usage: midi cc <channel> <cc#> <value>\n");
            return;
        }

        midi_send_payload_t pay;
        memset(&pay, 0, sizeof(pay));
        pay.status = (uint8_t)(0xB0 | ((uint8_t)strtoul(s1, NULL, 0) & 0x0F));
        pay.data1  = (uint8_t)strtoul(s2, NULL, 0) & 0x7F;
        pay.data2  = (uint8_t)strtoul(s3, NULL, 0) & 0x7F;

        actor_send_named(rt, "/node/hardware/midi", MSG_MIDI_SEND,
                         &pay, sizeof(pay));
        sh->pending_call = true;
        sh->call_timer = actor_set_timer(rt, 3000, false);
        return;
    }

    /* ── midi pc <ch> <program> ──────────────────────────────────── */
    if (strcmp(sub, "pc") == 0) {
        char s1[8], s2[8];
        args = next_word(args, s1, sizeof(s1));
        next_word(args, s2, sizeof(s2));

        if (s1[0] == '\0' || s2[0] == '\0') {
            printf("Usage: midi pc <channel> <program>\n");
            return;
        }

        midi_send_payload_t pay;
        memset(&pay, 0, sizeof(pay));
        pay.status = (uint8_t)(0xC0 | ((uint8_t)strtoul(s1, NULL, 0) & 0x0F));
        pay.data1  = (uint8_t)strtoul(s2, NULL, 0) & 0x7F;

        actor_send_named(rt, "/node/hardware/midi", MSG_MIDI_SEND,
                         &pay, sizeof(pay));
        sh->pending_call = true;
        sh->call_timer = actor_set_timer(rt, 3000, false);
        return;
    }

    /* ── midi monitor ────────────────────────────────────────────── */
    if (strcmp(sub, "monitor") == 0 || strcmp(sub, "mon") == 0) {
        actor_id_t existing = actor_lookup(rt, "/sys/midi_monitor");
        if (existing != ACTOR_ID_INVALID) {
            printf("MIDI monitor already running (%" PRIu64 ")\n",
                   (uint64_t)existing);
            return;
        }

        actor_id_t mon = midi_monitor_init(rt);
        if (mon == ACTOR_ID_INVALID)
            printf("Failed (is MIDI actor running?)\n");
        else
            printf("MIDI monitor started (%" PRIu64 ")\n", (uint64_t)mon);
        return;
    }

    /* ── midi arp ────────────────────────────────────────────────── */
    if (strcmp(sub, "arp") == 0) {
        char action[16];
        args = next_word(args, action, sizeof(action));

        /* midi arp (no args) — start arpeggiator */
        if (action[0] == '\0') {
            actor_id_t existing = actor_lookup(rt, "/sys/arpeggiator");
            if (existing != ACTOR_ID_INVALID) {
                printf("Arpeggiator already running (%" PRIu64 ")\n",
                       (uint64_t)existing);
                return;
            }
            actor_id_t arp = arpeggiator_init(rt);
            if (arp == ACTOR_ID_INVALID)
                printf("Failed (is MIDI actor running?)\n");
            else
                printf("Arpeggiator started (%" PRIu64 ")\n", (uint64_t)arp);
            return;
        }

        actor_id_t arp = actor_lookup(rt, "/sys/arpeggiator");
        if (arp == ACTOR_ID_INVALID) {
            printf("Arpeggiator not running (use 'midi arp' to start)\n");
            return;
        }

        if (strcmp(action, "on") == 0) {
            arp_enable_payload_t p = { .enable = 1 };
            actor_send(rt, arp, MSG_ARP_ENABLE, &p, sizeof(p));
            printf("Arpeggiator enabled\n");
        } else if (strcmp(action, "off") == 0) {
            arp_enable_payload_t p = { .enable = 0 };
            actor_send(rt, arp, MSG_ARP_ENABLE, &p, sizeof(p));
            printf("Arpeggiator disabled (bypass)\n");
        } else if (strcmp(action, "bpm") == 0) {
            char val[8];
            next_word(args, val, sizeof(val));
            if (val[0] == '\0') { printf("Usage: midi arp bpm <30-300>\n"); return; }
            arp_bpm_payload_t p = { .bpm = (uint16_t)atoi(val) };
            actor_send(rt, arp, MSG_ARP_SET_BPM, &p, sizeof(p));
            printf("BPM → %d\n", p.bpm);
        } else if (strcmp(action, "pattern") == 0) {
            char val[16];
            next_word(args, val, sizeof(val));
            arp_pattern_payload_t p;
            memset(&p, 0, sizeof(p));
            if (strcmp(val, "up") == 0)          p.pattern = ARP_UP;
            else if (strcmp(val, "down") == 0)   p.pattern = ARP_DOWN;
            else if (strcmp(val, "updown") == 0) p.pattern = ARP_UPDOWN;
            else if (strcmp(val, "random") == 0) p.pattern = ARP_RANDOM;
            else { printf("Patterns: up down updown random\n"); return; }
            actor_send(rt, arp, MSG_ARP_SET_PATTERN, &p, sizeof(p));
            printf("Pattern → %s\n", val);
        } else if (strcmp(action, "octaves") == 0) {
            char val[8];
            next_word(args, val, sizeof(val));
            if (val[0] == '\0') { printf("Usage: midi arp octaves <1-4>\n"); return; }
            arp_octaves_payload_t p = { .octaves = (uint8_t)atoi(val) };
            actor_send(rt, arp, MSG_ARP_SET_OCTAVES, &p, sizeof(p));
            printf("Octaves → %d\n", p.octaves);
        } else if (strcmp(action, "stop") == 0) {
            actor_stop(rt, arp);
            printf("Arpeggiator stopped\n");
        } else {
            printf("Unknown: midi arp %s\n", action);
        }
        return;
    }

    /* ── midi play <notes...> [--bpm N] [--vel N] [--ch N] ─────── */
    if (strcmp(sub, "play") == 0) {
        /* Defaults */
        uint16_t bpm = 120;
        uint8_t  vel = 100;
        uint8_t  ch  = 0;

        /* Parse notes and flags */
        uint8_t notes[PLAYER_MAX_NOTES];
        int     note_count = 0;
        char    tok[16];

        while (1) {
            args = next_word(args, tok, sizeof(tok));
            if (tok[0] == '\0') break;

            if (strcmp(tok, "--bpm") == 0 || strcmp(tok, "-b") == 0) {
                args = next_word(args, tok, sizeof(tok));
                if (tok[0]) bpm = (uint16_t)atoi(tok);
                continue;
            }
            if (strcmp(tok, "--vel") == 0 || strcmp(tok, "-v") == 0) {
                args = next_word(args, tok, sizeof(tok));
                if (tok[0]) vel = (uint8_t)atoi(tok);
                continue;
            }
            if (strcmp(tok, "--ch") == 0 || strcmp(tok, "-c") == 0) {
                args = next_word(args, tok, sizeof(tok));
                if (tok[0]) ch = (uint8_t)atoi(tok);
                continue;
            }

            if (note_count < PLAYER_MAX_NOTES)
                notes[note_count++] = (uint8_t)strtoul(tok, NULL, 0);
        }

        if (note_count == 0) {
            printf(
                "Usage: midi play <note> [note...] [--bpm 120] [--vel 100] [--ch 0]\n"
                "  Notes are MIDI numbers: 60=C4 62=D4 64=E4 65=F4 67=G4 69=A4 71=B4 72=C5\n"
                "  Example: midi play 60 62 64 65 67 69 71 72 --bpm 180\n"
            );
            return;
        }

        actor_id_t midi = actor_lookup(rt, "/node/hardware/midi");
        if (midi == ACTOR_ID_INVALID) {
            printf("MIDI actor not found\n");
            return;
        }

        /* Spawn player once, reuse on subsequent calls */
        actor_id_t player = actor_lookup(rt, "/sys/midi_player");
        if (player == ACTOR_ID_INVALID) {
            player_state_t *ps = calloc(1, sizeof(*ps));
            if (!ps) { printf("Out of memory\n"); return; }
            ps->midi_id = midi;

            player = actor_spawn(rt, player_behavior, ps, free, 16);
            if (player == ACTOR_ID_INVALID) {
                printf("Failed to spawn player\n");
                return;
            }
            actor_register_name(rt, "/sys/midi_player", player);
        }

        /* Send new sequence to player (resets and starts playback) */
        player_seq_payload_t seq;
        memset(&seq, 0, sizeof(seq));
        memcpy(seq.notes, notes, (size_t)note_count);
        seq.count = (uint8_t)note_count;
        seq.vel = vel;
        seq.ch = ch;
        seq.interval_ms = (uint32_t)(60000 / (bpm ? bpm : 120));

        actor_send(rt, player, PLAYER_NEW_SEQ, &seq, sizeof(seq));

        printf("Playing %d notes at %u BPM, vel=%u, ch=%u\n",
               note_count, bpm, vel, ch);
        return;
    }

    /* ── midi stop ───────────────────────────────────────────────── */
    if (strcmp(sub, "stop") == 0) {
        actor_id_t player = actor_lookup(rt, "/sys/midi_player");
        if (player != ACTOR_ID_INVALID) {
            /* Send empty sequence to stop playback (player stays alive) */
            player_seq_payload_t seq;
            memset(&seq, 0, sizeof(seq));
            actor_send(rt, player, PLAYER_NEW_SEQ, &seq, sizeof(seq));
            printf("Player stopped\n");
        } else {
            printf("No player running\n");
        }
        return;
    }

    /* ── midi status ─────────────────────────────────────────────── */
    if (strcmp(sub, "status") == 0) {
        midi_hal_status_t st;
        if (!midi_hal_read_status(&st)) {
            printf("MIDI not configured\n");
            return;
        }
        printf("SC16IS752 Channel A (MIDI IN):\n");
        printf("  RXLVL = %u  (bytes in RX FIFO)\n", st.rxlvl);
        printf("  TXLVL = %u  (free in TX FIFO, Ch B)\n", st.txlvl);
        printf("  IER   = 0x%02X  (bit0=RHR irq %s)\n",
               st.ier, (st.ier & 0x01) ? "ENABLED" : "disabled");
        printf("  IIR   = 0x%02X  (bit0=%s, src=%u)\n",
               st.iir,
               (st.iir & 0x01) ? "no-irq" : "IRQ-PENDING",
               (st.iir >> 1) & 0x07);
        printf("  LSR   = 0x%02X  (bit0=data-ready:%s, bit1=overrun:%s)\n",
               st.lsr,
               (st.lsr & 0x01) ? "YES" : "no",
               (st.lsr & 0x02) ? "YES" : "no");
        return;
    }

    printf("Unknown MIDI command: %s (try 'midi help')\n", sub);
}

/* ── seq command ─────────────────────────────────────────────────── */

static void cmd_seq(runtime_t *rt, const char *args) {
    char sub[32];
    args = next_word(args, sub, sizeof(sub));

    if (sub[0] == '\0' || strcmp(sub, "help") == 0) {
        printf(
            "Sequencer commands:\n"
            "  seq start            Start playback\n"
            "  seq stop             Stop playback\n"
            "  seq pause            Pause/resume toggle\n"
            "  seq tempo <bpm>      Set tempo (BPM)\n"
            "  seq status           Show sequencer status\n"
            "  seq mute <track>     Mute track (0-7)\n"
            "  seq unmute <track>   Unmute track\n"
            "  seq solo <track>     Solo track\n"
            "  seq unsolo <track>   Unsolo track\n"
            "  seq switch <t> <s>   Switch track <t> to slot <s>\n"
            "  seq fx <t> transpose <semi> [cents]\n"
            "  seq fx <t> velocity <pct>   Scale velocity (1-200%%)\n"
            "  seq fx <t> humanize <range> Random velocity +/-range\n"
            "  seq fx <t> ccscale <cc> <min> <max>\n"
            "  seq fx <t> clear [slot]     Clear effects\n"
            "  seq fx <t> enable <slot>    Enable effect slot\n"
            "  seq fx <t> disable <slot>   Disable effect slot\n"
            "  seq demo             Load C major scale demo\n"
            "  seq demo2            Load polyrhythm demo (2 tracks)\n"
        );
        return;
    }

    /* Ensure sequencer exists */
    actor_id_t seq = actor_lookup(rt, "/sys/sequencer");
    if (seq == ACTOR_ID_INVALID) {
        /* Try to spawn it */
        seq = sequencer_init(rt);
        if (seq == ACTOR_ID_INVALID) {
            printf("Sequencer init failed (MIDI actor not found?)\n");
            return;
        }
        printf("Sequencer spawned\n");
    }

    if (strcmp(sub, "start") == 0) {
        actor_send(rt, seq, MSG_SEQ_START, NULL, 0);
        printf("Sequencer started\n");
        return;
    }

    if (strcmp(sub, "stop") == 0) {
        actor_send(rt, seq, MSG_SEQ_STOP, NULL, 0);
        printf("Sequencer stopped\n");
        return;
    }

    if (strcmp(sub, "pause") == 0) {
        actor_send(rt, seq, MSG_SEQ_PAUSE, NULL, 0);
        printf("Sequencer pause toggled\n");
        return;
    }

    if (strcmp(sub, "tempo") == 0) {
        char val[16];
        next_word(args, val, sizeof(val));
        if (val[0] == '\0') {
            printf("Usage: seq tempo <bpm>\n");
            return;
        }
        float bpm = (float)atof(val);
        if (bpm <= 0 || bpm > 300) {
            printf("BPM must be 1–300\n");
            return;
        }
        seq_tempo_payload_t tp = { .bpm_x100 = (uint32_t)(bpm * 100) };
        actor_send(rt, seq, MSG_SEQ_SET_TEMPO, &tp, sizeof(tp));
        printf("Tempo set to %.1f BPM\n", bpm);
        return;
    }

    if (strcmp(sub, "status") == 0) {
        actor_send(rt, seq, MSG_SEQ_STATUS, NULL, 0);
        /* Reply will print when received */
        printf("(status request sent)\n");
        return;
    }

    if (strcmp(sub, "mute") == 0 || strcmp(sub, "unmute") == 0) {
        char val[8];
        next_word(args, val, sizeof(val));
        if (val[0] == '\0') {
            printf("Usage: seq %s <track>\n", sub);
            return;
        }
        int t = atoi(val);
        if (t < 0 || t >= SEQ_MAX_TRACKS) {
            printf("Track must be 0–%d\n", SEQ_MAX_TRACKS - 1);
            return;
        }
        seq_mute_payload_t mp = { .track = (uint8_t)t,
                                  .muted = (strcmp(sub, "mute") == 0) };
        actor_send(rt, seq, MSG_SEQ_MUTE_TRACK, &mp, sizeof(mp));
        printf("Track %d %s\n", t, mp.muted ? "muted" : "unmuted");
        return;
    }

    if (strcmp(sub, "solo") == 0 || strcmp(sub, "unsolo") == 0) {
        char val[8];
        next_word(args, val, sizeof(val));
        if (val[0] == '\0') {
            printf("Usage: seq %s <track>\n", sub);
            return;
        }
        int t = atoi(val);
        if (t < 0 || t >= SEQ_MAX_TRACKS) {
            printf("Track must be 0–%d\n", SEQ_MAX_TRACKS - 1);
            return;
        }
        seq_solo_payload_t sp = { .track = (uint8_t)t,
                                  .soloed = (strcmp(sub, "solo") == 0) };
        actor_send(rt, seq, MSG_SEQ_SOLO_TRACK, &sp, sizeof(sp));
        printf("Track %d %s\n", t, sp.soloed ? "soloed" : "unsoloed");
        return;
    }

    if (strcmp(sub, "switch") == 0) {
        char tval[8], sval[8];
        args = next_word(args, tval, sizeof(tval));
        next_word(args, sval, sizeof(sval));
        if (tval[0] == '\0' || sval[0] == '\0') {
            printf("Usage: seq switch <track> <slot>\n");
            return;
        }
        int t = atoi(tval);
        int sl = atoi(sval);
        if (t < 0 || t >= SEQ_MAX_TRACKS) {
            printf("Track must be 0–%d\n", SEQ_MAX_TRACKS - 1);
            return;
        }
        if (sl < 0 || sl > 1) {
            printf("Slot must be 0 or 1\n");
            return;
        }
        seq_switch_slot_payload_t sw = { .track = (uint8_t)t,
                                         .slot = (uint8_t)sl };
        actor_send(rt, seq, MSG_SEQ_SWITCH_SLOT, &sw, sizeof(sw));
        printf("Track %d queued switch to slot %d\n", t, sl);
        return;
    }

    if (strcmp(sub, "fx") == 0) {
        char tval[8];
        args = next_word(args, tval, sizeof(tval));
        if (tval[0] == '\0') {
            printf("Usage: seq fx <track> <transpose|velocity|humanize|ccscale|clear|enable|disable> ...\n");
            return;
        }
        int t = atoi(tval);
        if (t < 0 || t >= SEQ_MAX_TRACKS) {
            printf("Track must be 0–%d\n", SEQ_MAX_TRACKS - 1);
            return;
        }

        char fxsub[16];
        args = next_word(args, fxsub, sizeof(fxsub));
        if (fxsub[0] == '\0') {
            printf("Usage: seq fx <track> <transpose|velocity|humanize|ccscale|clear|enable|disable>\n");
            return;
        }

        if (strcmp(fxsub, "transpose") == 0) {
            char sval[8], cval[8];
            args = next_word(args, sval, sizeof(sval));
            next_word(args, cval, sizeof(cval));
            if (sval[0] == '\0') {
                printf("Usage: seq fx <track> transpose <semitones> [cents]\n");
                return;
            }
            seq_set_fx_payload_t fp;
            memset(&fp, 0, sizeof(fp));
            fp.track = (uint8_t)t;
            /* Auto-assign to next available slot */
            /* Request status to find count — for simplicity just use msg directly */
            /* We send with slot = chain count (the sequencer will expand) */
            /* Actually, we can't query status synchronously, so just pick slot 0
             * or let user manage. For auto-assign, we track via a simple heuristic:
             * use a local counter approach — not ideal, but functional. */
            fp.slot = 0; /* default slot; user can clear and reassign */
            fp.effect.type = SEQ_FX_TRANSPOSE;
            fp.effect.enabled = true;
            fp.effect.params.transpose.semitones = (int8_t)atoi(sval);
            fp.effect.params.transpose.cents = cval[0] ? (int8_t)atoi(cval) : 0;
            actor_send(rt, seq, MSG_SEQ_SET_FX, &fp, sizeof(fp));
            printf("Track %d: transpose %+d semi %+d cents → slot %d\n",
                   t, fp.effect.params.transpose.semitones,
                   fp.effect.params.transpose.cents, fp.slot);
            return;
        }

        if (strcmp(fxsub, "velocity") == 0) {
            char pval[8];
            next_word(args, pval, sizeof(pval));
            if (pval[0] == '\0') {
                printf("Usage: seq fx <track> velocity <percent>\n");
                return;
            }
            seq_set_fx_payload_t fp;
            memset(&fp, 0, sizeof(fp));
            fp.track = (uint8_t)t;
            fp.slot = 1;
            fp.effect.type = SEQ_FX_VELOCITY_SCALE;
            fp.effect.enabled = true;
            fp.effect.params.velocity_scale.scale_pct = (uint8_t)atoi(pval);
            actor_send(rt, seq, MSG_SEQ_SET_FX, &fp, sizeof(fp));
            printf("Track %d: velocity scale %d%% → slot %d\n",
                   t, fp.effect.params.velocity_scale.scale_pct, fp.slot);
            return;
        }

        if (strcmp(fxsub, "humanize") == 0) {
            char rval[8];
            next_word(args, rval, sizeof(rval));
            if (rval[0] == '\0') {
                printf("Usage: seq fx <track> humanize <range>\n");
                return;
            }
            seq_set_fx_payload_t fp;
            memset(&fp, 0, sizeof(fp));
            fp.track = (uint8_t)t;
            fp.slot = 2;
            fp.effect.type = SEQ_FX_HUMANIZE;
            fp.effect.enabled = true;
            fp.effect.params.humanize.velocity_range = (uint8_t)atoi(rval);
            actor_send(rt, seq, MSG_SEQ_SET_FX, &fp, sizeof(fp));
            printf("Track %d: humanize ±%d → slot %d\n",
                   t, fp.effect.params.humanize.velocity_range, fp.slot);
            return;
        }

        if (strcmp(fxsub, "ccscale") == 0) {
            char ccval[8], minv[8], maxv[8];
            args = next_word(args, ccval, sizeof(ccval));
            args = next_word(args, minv, sizeof(minv));
            next_word(args, maxv, sizeof(maxv));
            if (ccval[0] == '\0' || minv[0] == '\0' || maxv[0] == '\0') {
                printf("Usage: seq fx <track> ccscale <cc> <min> <max>\n");
                return;
            }
            seq_set_fx_payload_t fp;
            memset(&fp, 0, sizeof(fp));
            fp.track = (uint8_t)t;
            fp.slot = 3;
            fp.effect.type = SEQ_FX_CC_SCALE;
            fp.effect.enabled = true;
            fp.effect.params.cc_scale.cc_number = (uint8_t)atoi(ccval);
            fp.effect.params.cc_scale.min_val = (uint8_t)atoi(minv);
            fp.effect.params.cc_scale.max_val = (uint8_t)atoi(maxv);
            actor_send(rt, seq, MSG_SEQ_SET_FX, &fp, sizeof(fp));
            printf("Track %d: CC%d scale %d–%d → slot %d\n",
                   t, fp.effect.params.cc_scale.cc_number,
                   fp.effect.params.cc_scale.min_val,
                   fp.effect.params.cc_scale.max_val, fp.slot);
            return;
        }

        if (strcmp(fxsub, "clear") == 0) {
            char sval[8];
            next_word(args, sval, sizeof(sval));
            seq_clear_fx_payload_t cp;
            memset(&cp, 0, sizeof(cp));
            cp.track = (uint8_t)t;
            cp.slot = sval[0] ? (uint8_t)atoi(sval) : 0xFF;
            actor_send(rt, seq, MSG_SEQ_CLEAR_FX, &cp, sizeof(cp));
            if (cp.slot == 0xFF)
                printf("Track %d: all effects cleared\n", t);
            else
                printf("Track %d: slot %d cleared\n", t, cp.slot);
            return;
        }

        if (strcmp(fxsub, "enable") == 0 || strcmp(fxsub, "disable") == 0) {
            char sval[8];
            next_word(args, sval, sizeof(sval));
            if (sval[0] == '\0') {
                printf("Usage: seq fx <track> %s <slot>\n", fxsub);
                return;
            }
            seq_enable_fx_payload_t ep;
            memset(&ep, 0, sizeof(ep));
            ep.track = (uint8_t)t;
            ep.slot = (uint8_t)atoi(sval);
            ep.enabled = (strcmp(fxsub, "enable") == 0);
            actor_send(rt, seq, MSG_SEQ_ENABLE_FX, &ep, sizeof(ep));
            printf("Track %d: slot %d %s\n", t, ep.slot,
                   ep.enabled ? "enabled" : "disabled");
            return;
        }

        printf("Unknown fx sub-command: %s\n", fxsub);
        return;
    }

    if (strcmp(sub, "demo") == 0) {
        /* C major scale as 8th notes */
        uint8_t notes[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
        int n = (int)(sizeof(notes) / sizeof(notes[0]));
        seq_event_t events[8];
        for (int i = 0; i < n; i++)
            events[i] = seq_note((tick_t)(i * SEQ_PPQN / 2), notes[i],
                                 100, SEQ_PPQN / 2 - 10, 0);

        seq_load_payload_t *p = seq_build_load_payload(
            0, 0, SEQ_PPQN * 4, "C major scale", events, (uint16_t)n);
        if (!p) {
            printf("Out of memory\n");
            return;
        }
        actor_send(rt, seq, MSG_SEQ_LOAD_PATTERN,
                   p, seq_load_payload_size((uint16_t)n));
        free(p);
        printf("Demo pattern loaded (C major scale, 2 bars 8th notes)\n");
        printf("Use 'seq start' to play, 'seq tempo 120' to set speed\n");
        return;
    }

    if (strcmp(sub, "demo2") == 0) {
        /* Montage split: bass below C4, piano above C4, both ch 0 */

        /* Track 0: 4-bar piano melody (above middle C, ch 0) */
        {
            seq_event_t events[] = {
                /* Bar 1: C E G E */
                seq_note(SEQ_PPQN * 0, 72, 90,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 1, 76, 80,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 2, 79, 85,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 3, 76, 75,  SEQ_PPQN - 10, 0),
                /* Bar 2: A G F E */
                seq_note(SEQ_PPQN * 4, 81, 90,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 5, 79, 80,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 6, 77, 85,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 7, 76, 75,  SEQ_PPQN - 10, 0),
                /* Bar 3: D F A F */
                seq_note(SEQ_PPQN * 8,  74, 90,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 9,  77, 80,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 10, 81, 85,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 11, 77, 75,  SEQ_PPQN - 10, 0),
                /* Bar 4: G F E D → resolve to C */
                seq_note(SEQ_PPQN * 12, 79, 90,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 13, 77, 80,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 14, 76, 85,  SEQ_PPQN - 10, 0),
                seq_note(SEQ_PPQN * 15, 74, 75,  SEQ_PPQN - 10, 0),
            };
            uint16_t n = (uint16_t)(sizeof(events) / sizeof(events[0]));
            seq_load_payload_t *p = seq_build_load_payload(
                0, 0, SEQ_PPQN * 16, "Piano", events, n);
            if (!p) { printf("Out of memory\n"); return; }
            actor_send(rt, seq, MSG_SEQ_LOAD_PATTERN,
                       p, seq_load_payload_size(n));
            free(p);
        }
        /* Track 1: 2-bar bass line (below middle C, ch 0) — polyrhythm */
        {
            seq_event_t events[] = {
                /* Bar 1: C2 whole, G2 half, A2 half */
                seq_note(0,            36, 110, SEQ_PPQN * 2 - 10, 0),  /* C2 */
                seq_note(SEQ_PPQN * 2, 43, 100, SEQ_PPQN - 10,     0),  /* G2 */
                seq_note(SEQ_PPQN * 3, 45, 100, SEQ_PPQN - 10,     0),  /* A2 */
                /* Bar 2: F2 whole, E2 half, D2 half */
                seq_note(SEQ_PPQN * 4, 41, 110, SEQ_PPQN * 2 - 10, 0),  /* F2 */
                seq_note(SEQ_PPQN * 6, 40, 100, SEQ_PPQN - 10,     0),  /* E2 */
                seq_note(SEQ_PPQN * 7, 38, 100, SEQ_PPQN - 10,     0),  /* D2 */
            };
            uint16_t n = (uint16_t)(sizeof(events) / sizeof(events[0]));
            seq_load_payload_t *p = seq_build_load_payload(
                1, 0, SEQ_PPQN * 8, "Bass", events, n);
            if (!p) { printf("Out of memory\n"); return; }
            actor_send(rt, seq, MSG_SEQ_LOAD_PATTERN,
                       p, seq_load_payload_size(n));
            free(p);
        }
        printf("Montage split demo loaded (all ch 0):\n"
               "  Track 0: 4-bar piano melody (C5-A5)\n"
               "  Track 1: 2-bar bass line    (C2-A2)\n"
               "Use 'seq start' to play, 'seq tempo 100' for tempo\n");
        return;
    }

    printf("Unknown seq command: %s (try 'seq help')\n", sub);
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
    } else if (strcmp(cmd, "midi") == 0) {
        cmd_midi(rt, rest, sh);
    } else if (strcmp(cmd, "seq") == 0) {
        cmd_seq(rt, rest);
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
