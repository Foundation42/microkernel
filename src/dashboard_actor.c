#include "microkernel/dashboard.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "microkernel/namespace.h"
#include "microkernel/display.h"
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

/* ── Dashboard colors ─────────────────────────────────────────────── */

#define COLOR_BG         RGB565(0x10, 0x10, 0x18)
#define COLOR_TEXT        RGB565(0xE0, 0xE0, 0xE0)
#define COLOR_LABEL       RGB565(0x80, 0x80, 0x90)
#define COLOR_ACCENT      RGB565(0x00, 0xD0, 0xF0)
#define COLOR_HEADER_BG   RGB565(0x00, 0x40, 0x60)
#define COLOR_BAR_FILL    RGB565(0x00, 0xC0, 0x60)
#define COLOR_BAR_EMPTY   RGB565(0x30, 0x30, 0x38)
#define COLOR_BAR_WARN    RGB565(0xF0, 0xA0, 0x00)

#define MAX_DASHBOARD_ACTORS 16

/* ── State ────────────────────────────────────────────────────────── */

typedef struct {
    timer_id_t timer_id;
} dashboard_state_t;

/* ── Uptime ───────────────────────────────────────────────────────── */

static uint64_t get_uptime_ms(void) {
#ifdef ESP_PLATFORM
    return (uint64_t)(esp_timer_get_time() / 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

/* ── Memory bar ───────────────────────────────────────────────────── */

#ifdef ESP_PLATFORM
static void render_memory_bar(runtime_t *rt, uint16_t row,
                              const char *label, size_t used, size_t total) {
    char line[64];
    snprintf(line, sizeof(line), "%-6s %3zuK/%3zuK",
             label, used / 1024, total / 1024);
    display_text(rt, DISPLAY_COL(2), DISPLAY_ROW(row),
                 COLOR_LABEL, COLOR_BG, line);

    /* Bar: columns 22–56 (35 cols = 280px) */
    uint16_t bar_x = DISPLAY_COL(22);
    uint16_t bar_w = DISPLAY_COL(35);
    uint16_t bar_y = DISPLAY_ROW(row) + 4;
    uint16_t bar_h = 8;

    if (total > 0) {
        uint16_t filled = (uint16_t)((uint32_t)bar_w * used / total);
        if (filled > bar_w) filled = bar_w;
        uint16_t pct = (uint16_t)((uint32_t)100 * used / total);
        uint16_t fill_color = (pct > 80) ? COLOR_BAR_WARN : COLOR_BAR_FILL;

        if (filled > 0)
            display_fill_rect(rt, bar_x, bar_y, filled, bar_h, fill_color);
        if (filled < bar_w)
            display_fill_rect(rt, bar_x + filled, bar_y,
                              bar_w - filled, bar_h, COLOR_BAR_EMPTY);
    } else {
        display_fill_rect(rt, bar_x, bar_y, bar_w, bar_h, COLOR_BAR_EMPTY);
    }
}
#endif /* ESP_PLATFORM */

/* ── Render frame ─────────────────────────────────────────────────── */

static void render_frame(runtime_t *rt) {
    char tmp[64];

    /* Header bar */
    display_fill_rect(rt, 0, 0, 466, 16, COLOR_HEADER_BG);
    display_text(rt, 8, 0, COLOR_ACCENT, COLOR_HEADER_BG, "MICROKERNEL");

    /* Node info */
    snprintf(tmp, sizeof(tmp), "  node: %-16s id: %u",
             mk_node_identity(), (unsigned)mk_node_id());
    display_text(rt, 0, DISPLAY_ROW(1), COLOR_TEXT, COLOR_BG, tmp);

    /* IP address (ESP32 WiFi only) */
#if defined(ESP_PLATFORM) && SOC_WIFI_SUPPORTED
    {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            snprintf(tmp, sizeof(tmp), "  ip: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            snprintf(tmp, sizeof(tmp), "  ip: not connected");
        }
        display_text(rt, 0, DISPLAY_ROW(2), COLOR_TEXT, COLOR_BG, tmp);
    }
#else
    display_text(rt, 0, DISPLAY_ROW(2), COLOR_LABEL, COLOR_BG,
                 "  ip: N/A (linux)");
#endif

    /* Uptime */
    {
        uint64_t ms = get_uptime_ms();
        uint32_t secs = (uint32_t)(ms / 1000);
        uint32_t mins = secs / 60;
        uint32_t hours = mins / 60;
        snprintf(tmp, sizeof(tmp), "  uptime: %02u:%02u:%02u",
                 (unsigned)(hours % 100), (unsigned)(mins % 60),
                 (unsigned)(secs % 60));
        display_text(rt, 0, DISPLAY_ROW(3), COLOR_TEXT, COLOR_BG, tmp);
    }

    /* Memory section */
    display_text(rt, DISPLAY_COL(1), DISPLAY_ROW(5),
                 COLOR_ACCENT, COLOR_BG, "MEMORY");

#ifdef ESP_PLATFORM
    {
        size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t dram_used = dram_total - dram_free;
        render_memory_bar(rt, 6, "DRAM", dram_used, dram_total);

        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        if (psram_total > 0) {
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t psram_used = psram_total - psram_free;
            render_memory_bar(rt, 7, "PSRAM", psram_used, psram_total);
        } else {
            display_text(rt, DISPLAY_COL(2), DISPLAY_ROW(7),
                         COLOR_LABEL, COLOR_BG, "PSRAM  N/A");
        }
    }
#else
    display_text(rt, DISPLAY_COL(2), DISPLAY_ROW(6),
                 COLOR_LABEL, COLOR_BG, "DRAM   (linux host)");
    display_text(rt, DISPLAY_COL(2), DISPLAY_ROW(7),
                 COLOR_LABEL, COLOR_BG, "PSRAM  N/A");
#endif

    /* Actor list */
    actor_info_t actors[MAX_DASHBOARD_ACTORS];
    size_t count = runtime_actor_info(rt, actors, MAX_DASHBOARD_ACTORS);

    snprintf(tmp, sizeof(tmp), "ACTORS (%zu)", count);
    display_text(rt, DISPLAY_COL(1), DISPLAY_ROW(9),
                 COLOR_ACCENT, COLOR_BG, tmp);

    for (size_t i = 0; i < count && i < 16; i++) {
        char name_buf[48];
        size_t nlen = actor_reverse_lookup_all(rt, actors[i].id,
                                               name_buf, sizeof(name_buf));
        if (nlen == 0)
            snprintf(name_buf, sizeof(name_buf), "(anon)");

        char line[64];
        const char *status = "???";
        switch (actors[i].status) {
        case ACTOR_IDLE:    status = "idle"; break;
        case ACTOR_READY:   status = "rdy";  break;
        case ACTOR_RUNNING: status = "run";  break;
        case ACTOR_STOPPED: status = "stop"; break;
        }
        snprintf(line, sizeof(line), " %3u %-4s %s",
                 (unsigned)actor_id_seq(actors[i].id), status, name_buf);
        display_text(rt, 0, DISPLAY_ROW(10 + (uint16_t)i),
                     COLOR_TEXT, COLOR_BG, line);
    }

    /* Clear remaining actor rows */
    for (size_t i = count; i < 16; i++) {
        display_fill_rect(rt, 0, DISPLAY_ROW(10 + (uint16_t)i),
                          466, 16, COLOR_BG);
    }
}

/* ── Dashboard behavior ───────────────────────────────────────────── */

static bool dashboard_behavior(runtime_t *rt, actor_t *self,
                                message_t *msg, void *state) {
    (void)self;
    dashboard_state_t *ds = state;

    switch (msg->type) {
    case 1: /* bootstrap */
        display_clear_screen(rt);
        render_frame(rt);
        ds->timer_id = actor_set_timer(rt, 1000, true);
        break;

    case MSG_TIMER:
        render_frame(rt);
        break;

    default:
        break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void dashboard_state_free(void *state) {
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t dashboard_actor_init(runtime_t *rt) {
    /* Verify display actor exists */
    actor_id_t display_id = actor_lookup(rt, "/node/hardware/display");
    if (display_id == ACTOR_ID_INVALID)
        return ACTOR_ID_INVALID;

    dashboard_state_t *ds = calloc(1, sizeof(*ds));
    if (!ds)
        return ACTOR_ID_INVALID;

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
