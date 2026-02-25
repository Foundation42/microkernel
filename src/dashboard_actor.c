#include "microkernel/dashboard.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <soc/soc_caps.h>
#if SOC_WIFI_SUPPORTED
#include <esp_netif.h>
#endif
#endif

#define MAX_DASHBOARD_ACTORS 16

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    timer_id_t timer_id;
    size_t     prev_actor_count; /* track how many actor rows were drawn */
    int        cols, rows;       /* console grid dimensions */
    bool       circular;         /* true = AMOLED circular display */
} dashboard_state_t;

/* ── Circle margin table ──────────────────────────────────────────────── */
/* Precomputed left margin (in columns) for each row on a 466×466 circular
   display. Based on circle geometry: radius=233px center at (233,233),
   font cell 8×16px → 58 cols × 29 rows. */

static const uint8_t s_circle_margin[CONSOLE_ROWS] = {
    21, 16, 13, 10, 8, 6, 5, 3, 2, 2, 1, 0, 0, 0, 0,  /* rows 0-14 */
    0, 0, 0, 1, 2, 2, 3, 5, 6, 8, 10, 13, 16, 21        /* rows 15-28 */
};

/* Get left margin for a given row */
static int row_margin(const dashboard_state_t *ds, int row) {
    if (!ds->circular)
        return 0;
    if (row < 0 || row >= CONSOLE_ROWS)
        return 0;
    return (int)s_circle_margin[row];
}

/* Available columns at a given row */
static int avail_cols(const dashboard_state_t *ds, int row) {
    return ds->cols - 2 * row_margin(ds, row);
}

/* ── Uptime ───────────────────────────────────────────────────────────── */

static uint64_t get_uptime_ms(void) {
#ifdef ESP_PLATFORM
    return (uint64_t)(esp_timer_get_time() / 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* ── Memory bar (text-mode, single printf) ────────────────────────────── */

#ifdef ESP_PLATFORM
static void render_memory_bar(runtime_t *rt, int row, int margin,
                              int total_cols,
                              const char *label, size_t used, size_t total) {
    int cols = total_cols - 2 * margin;
    if (cols < 20) return;

    /* Build entire bar as one string to avoid per-char messages.
       Format: " DRAM  123K/456K [====       ]\e[K" */
    char line[256];
    int pos = 0;

    /* Position cursor + start color */
    pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                    "\033[%d;%dH\033[37m %-6s%3zuK/%3zuK ",
                    row + 1, margin + 1,
                    label, used / 1024, total / 1024);

    int bar_width = cols - 20 - 2; /* 20 for label+stats, 2 for [] */
    if (bar_width < 4) bar_width = 4;
    if (bar_width > 40) bar_width = 40;

    int filled = 0;
    int pct = 0;
    if (total > 0) {
        pct = (int)((uint64_t)100 * used / total);
        filled = (int)((uint64_t)bar_width * used / total);
        if (filled > bar_width) filled = bar_width;
    }

    const char *bar_color = (pct > 80) ? "\033[93m" : "\033[92m";
    pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%s[", bar_color);

    for (int i = 0; i < filled && pos < (int)sizeof(line) - 10; i++)
        line[pos++] = '=';

    pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "\033[90m");
    for (int i = filled; i < bar_width && pos < (int)sizeof(line) - 10; i++)
        line[pos++] = ' ';

    pos += snprintf(line + pos, sizeof(line) - (size_t)pos,
                    "\033[37m]\033[K\033[0m");

    mk_console_write(rt, line, (size_t)pos);
}
#endif

/* ── Render frame ────────────────────────────────────────────────────── */

static void render_frame(runtime_t *rt, dashboard_state_t *ds) {
    char tmp[128];

    /* No console_clear — overwrite in place.  Use \e[K (erase to EOL)
       at end of each line to clear stale content without causing
       a full-screen blank flash. */

    /* Row 3: Header — centered "MICROKERNEL" in bright cyan */
    {
        int m = row_margin(ds, 3);
        int cols = avail_cols(ds, 3);
        int pad = (cols - 13) / 2;
        if (pad < 0) pad = 0;
        mk_console_printf(rt, "\033[4;%dH%*s\033[96m MICROKERNEL \033[0m\033[K",
                       m + 1, pad, "");
    }

    /* Row 5: Node info */
    {
        int m = row_margin(ds, 5);
        snprintf(tmp, sizeof(tmp), " node: %-16s id: %u",
                 mk_node_identity(), (unsigned)mk_node_id());
        mk_console_printf(rt, "\033[6;%dH\033[37m%s\033[K\033[0m", m + 1, tmp);
    }

    /* Row 6: IP address */
    {
        int m = row_margin(ds, 6);
#if defined(ESP_PLATFORM) && SOC_WIFI_SUPPORTED
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            snprintf(tmp, sizeof(tmp), " ip: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            snprintf(tmp, sizeof(tmp), " ip: not connected");
        }
        mk_console_printf(rt, "\033[7;%dH\033[37m%s\033[K\033[0m", m + 1, tmp);
#else
        mk_console_printf(rt, "\033[7;%dH\033[90m ip: N/A (linux)\033[K\033[0m", m + 1);
        (void)m;
#endif
    }

    /* Row 7: Uptime */
    {
        int m = row_margin(ds, 7);
        uint64_t ms = get_uptime_ms();
        uint32_t secs = (uint32_t)(ms / 1000);
        uint32_t mins = secs / 60;
        uint32_t hours = mins / 60;
        snprintf(tmp, sizeof(tmp), " uptime: %02u:%02u:%02u",
                 (unsigned)(hours % 100), (unsigned)(mins % 60),
                 (unsigned)(secs % 60));
        mk_console_printf(rt, "\033[8;%dH\033[37m%s\033[K\033[0m", m + 1, tmp);
    }

    /* Row 9: Memory heading */
    {
        int m = row_margin(ds, 9);
        mk_console_printf(rt, "\033[10;%dH\033[96m MEMORY\033[K\033[0m", m + 1);
    }

    /* Row 10–11: Memory bars */
#ifdef ESP_PLATFORM
    {
        size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t dram_used = dram_total - dram_free;
        render_memory_bar(rt, 10, row_margin(ds, 10), ds->cols,
                          "DRAM", dram_used, dram_total);

        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        if (psram_total > 0) {
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t psram_used = psram_total - psram_free;
            render_memory_bar(rt, 11, row_margin(ds, 11), ds->cols,
                              "PSRAM", psram_used, psram_total);
        } else {
            mk_console_printf(rt, "\033[12;%dH\033[90m PSRAM  N/A\033[K\033[0m",
                           row_margin(ds, 11) + 1);
        }
    }
#else
    {
        int m10 = row_margin(ds, 10);
        int m11 = row_margin(ds, 11);
        mk_console_printf(rt, "\033[11;%dH\033[90m DRAM   (linux host)\033[K\033[0m", m10 + 1);
        mk_console_printf(rt, "\033[12;%dH\033[90m PSRAM  N/A\033[K\033[0m", m11 + 1);
    }
#endif

    /* Row 13: Actor list heading */
    actor_info_t actors[MAX_DASHBOARD_ACTORS];
    size_t count = runtime_actor_info(rt, actors, MAX_DASHBOARD_ACTORS);

    {
        int m = row_margin(ds, 13);
        snprintf(tmp, sizeof(tmp), " ACTORS (%zu)", count);
        mk_console_printf(rt, "\033[14;%dH\033[96m%s\033[K\033[0m", m + 1, tmp);
    }

    /* Rows 14–(rows-3): Actor entries */
    int max_actor_row = ds->rows - 3;  /* leave 3 rows at bottom */
    size_t max_display = (size_t)(max_actor_row > 14 ? max_actor_row - 14 : 0);
    if (max_display > MAX_DASHBOARD_ACTORS) max_display = MAX_DASHBOARD_ACTORS;
    if (count > max_display) count = max_display;

    for (size_t i = 0; i < count; i++) {
        int row = 14 + (int)i;
        if (row >= max_actor_row) break;
        int m = row_margin(ds, row);

        char name_buf[48];
        size_t nlen = actor_reverse_lookup_all(rt, actors[i].id,
                                               name_buf, sizeof(name_buf));
        if (nlen == 0)
            snprintf(name_buf, sizeof(name_buf), "(anon)");

        const char *status = "???";
        switch (actors[i].status) {
        case ACTOR_IDLE:    status = "idle"; break;
        case ACTOR_READY:   status = "rdy";  break;
        case ACTOR_RUNNING: status = "run";  break;
        case ACTOR_STOPPED: status = "stop"; break;
        }

        snprintf(tmp, sizeof(tmp), " %3u %-4s %s",
                 (unsigned)actor_id_seq(actors[i].id), status, name_buf);
        mk_console_printf(rt, "\033[%d;%dH\033[37m%s\033[K\033[0m",
                       row + 1, m + 1, tmp);
    }

    /* Clear stale actor rows from previous frame */
    for (size_t i = count; i < ds->prev_actor_count; i++) {
        int row = 14 + (int)i;
        if (row >= max_actor_row) break;
        int m = row_margin(ds, row);
        mk_console_printf(rt, "\033[%d;%dH\033[K", row + 1, m + 1);
    }
    ds->prev_actor_count = count;
}

/* ── Dashboard behavior ───────────────────────────────────────────────── */

static bool dashboard_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    dashboard_state_t *ds = state;

    switch (msg->type) {
    case 1: /* bootstrap */
        mk_console_clear(rt);
        render_frame(rt, ds);
        ds->timer_id = actor_set_timer(rt, 1000, true);
        break;

    case MSG_TIMER:
        render_frame(rt, ds);
        break;

    default:
        break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────────── */

static void dashboard_state_free(void *state) {
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────────── */

actor_id_t dashboard_actor_init(runtime_t *rt) {
    /* Verify console actor exists (which in turn requires display) */
    actor_id_t console_id = actor_lookup(rt, "/sys/console");
    if (console_id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    dashboard_state_t *ds = calloc(1, sizeof(*ds));
    if (!ds)
        return ACTOR_ID_INVALID;

    /* Query console dimensions to adapt layout */
    if (!mk_console_get_size(&ds->cols, &ds->rows)) {
        ds->cols = CONSOLE_COLS;
        ds->rows = CONSOLE_ROWS;
    }
    /* Circular display: AMOLED 1.43" has cols <= 58 */
    ds->circular = (ds->cols <= 58);

    actor_id_t id = actor_spawn(rt, dashboard_behavior, ds,
                                dashboard_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        free(ds);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/sys/dashboard", id);

    /* Bootstrap message triggers initial render */
    actor_send(rt, id, 1, NULL, 0);

    return id;
}
