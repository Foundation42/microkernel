#ifndef MICROKERNEL_SERVICES_H
#define MICROKERNEL_SERVICES_H

#include "types.h"

/* ── System message types (0xFF000000–0xFFFFFFFF reserved) ─────────── */

#define MSG_TIMER    ((msg_type_t)0xFF000001)
#define MSG_FD_EVENT ((msg_type_t)0xFF000002)
#define MSG_LOG      ((msg_type_t)0xFF000003)

/* Phase 3.5: HTTP / SSE / WebSocket message types */
#define MSG_HTTP_RESPONSE ((msg_type_t)0xFF000004)
#define MSG_HTTP_ERROR    ((msg_type_t)0xFF000005)
#define MSG_SSE_OPEN      ((msg_type_t)0xFF000006)
#define MSG_SSE_EVENT     ((msg_type_t)0xFF000007)
#define MSG_SSE_CLOSED    ((msg_type_t)0xFF000008)
#define MSG_WS_OPEN       ((msg_type_t)0xFF000009)
#define MSG_WS_MESSAGE    ((msg_type_t)0xFF00000A)
#define MSG_WS_CLOSED     ((msg_type_t)0xFF00000B)
#define MSG_WS_ERROR      ((msg_type_t)0xFF00000C)

/* Phase 5: Server-side HTTP message types */
#define MSG_HTTP_REQUEST      ((msg_type_t)0xFF00000D)
#define MSG_HTTP_LISTEN_ERROR ((msg_type_t)0xFF00000E)
#define MSG_HTTP_CONN_CLOSED  ((msg_type_t)0xFF00000F)

/* Phase 10: Supervision */
#define MSG_CHILD_EXIT        ((msg_type_t)0xFF000010)

/* Phase 11: Cross-node registry */
#define MSG_NAME_REGISTER     ((msg_type_t)0xFF000012)
#define MSG_NAME_UNREGISTER   ((msg_type_t)0xFF000013)

/* Phase 15.5: Cross-node path registry */
#define MSG_PATH_REGISTER     ((msg_type_t)0xFF00001B)
#define MSG_PATH_UNREGISTER   ((msg_type_t)0xFF00001C)

/* Phase 16: Capability advertisement */
#define MSG_CAPS_REQUEST      ((msg_type_t)0xFF00001D)
#define MSG_CAPS_REPLY        ((msg_type_t)0xFF00001E)

/* Phase 21: GPIO actor */
#define MSG_GPIO_CONFIGURE    ((msg_type_t)0xFF000020)
#define MSG_GPIO_WRITE        ((msg_type_t)0xFF000021)
#define MSG_GPIO_READ         ((msg_type_t)0xFF000022)
#define MSG_GPIO_SUBSCRIBE    ((msg_type_t)0xFF000023)
#define MSG_GPIO_UNSUBSCRIBE  ((msg_type_t)0xFF000024)
/* 0xFF000025–0xFF000027 reserved for ADC */
#define MSG_GPIO_OK           ((msg_type_t)0xFF000028)
#define MSG_GPIO_VALUE        ((msg_type_t)0xFF000029)
#define MSG_GPIO_ERROR        ((msg_type_t)0xFF00002A)
#define MSG_GPIO_EVENT        ((msg_type_t)0xFF00002B)

/* Phase 23: PWM actor */
#define MSG_PWM_CONFIGURE     ((msg_type_t)0xFF00002C)
#define MSG_PWM_SET_DUTY      ((msg_type_t)0xFF00002D)
#define MSG_PWM_OK            ((msg_type_t)0xFF00002E)
#define MSG_PWM_ERROR         ((msg_type_t)0xFF00002F)

/* Phase 22: I2C actor */
#define MSG_I2C_CONFIGURE     ((msg_type_t)0xFF000030)
#define MSG_I2C_WRITE         ((msg_type_t)0xFF000031)
#define MSG_I2C_READ          ((msg_type_t)0xFF000032)
#define MSG_I2C_WRITE_READ    ((msg_type_t)0xFF000033)
#define MSG_I2C_SCAN          ((msg_type_t)0xFF000034)
/* 0xFF000035–0xFF000037 reserved for I2C extensions */
#define MSG_I2C_OK            ((msg_type_t)0xFF000038)
#define MSG_I2C_DATA          ((msg_type_t)0xFF000039)
#define MSG_I2C_ERROR         ((msg_type_t)0xFF00003A)
#define MSG_I2C_SCAN_RESULT   ((msg_type_t)0xFF00003B)

/* Phase 23: LED actor */
#define MSG_LED_CONFIGURE     ((msg_type_t)0xFF000040)
#define MSG_LED_SET_PIXEL     ((msg_type_t)0xFF000041)
#define MSG_LED_SET_ALL       ((msg_type_t)0xFF000042)
#define MSG_LED_SET_BRIGHTNESS ((msg_type_t)0xFF000043)
#define MSG_LED_CLEAR         ((msg_type_t)0xFF000044)
#define MSG_LED_SHOW          ((msg_type_t)0xFF000045)
/* 0xFF000046–0xFF00004B reserved for LED extensions */
#define MSG_LED_OK            ((msg_type_t)0xFF00004C)
#define MSG_LED_ERROR         ((msg_type_t)0xFF00004D)

/* Phase 24: Display actor */
#define MSG_DISPLAY_DRAW       ((msg_type_t)0xFF000051)
#define MSG_DISPLAY_FILL       ((msg_type_t)0xFF000052)
#define MSG_DISPLAY_CLEAR      ((msg_type_t)0xFF000053)
#define MSG_DISPLAY_BRIGHTNESS ((msg_type_t)0xFF000054)
#define MSG_DISPLAY_POWER      ((msg_type_t)0xFF000055)
#define MSG_DISPLAY_TEXT       ((msg_type_t)0xFF000056)
/* 0xFF000057–0xFF00005B reserved for display extensions */
#define MSG_DISPLAY_OK         ((msg_type_t)0xFF00005C)
#define MSG_DISPLAY_ERROR      ((msg_type_t)0xFF00005D)

/* ── Timer payload ─────────────────────────────────────────────────── */

typedef struct {
    timer_id_t id;
    uint64_t   expirations; /* number of expirations (>1 if overrun) */
} timer_payload_t;

/* ── FD event payload ──────────────────────────────────────────────── */

typedef struct {
    int      fd;
    uint32_t events; /* POLLIN, POLLOUT, etc. */
} fd_event_payload_t;

/* ── Log levels ────────────────────────────────────────────────────── */

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

typedef struct {
    int        level;
    actor_id_t source;
    char       text[256];
} log_payload_t;

/* ── Timer API ─────────────────────────────────────────────────────── */

timer_id_t actor_set_timer(runtime_t *rt, uint64_t interval_ms, bool periodic);
bool       actor_cancel_timer(runtime_t *rt, timer_id_t id);

/* ── FD watcher API ────────────────────────────────────────────────── */

bool actor_watch_fd(runtime_t *rt, int fd, uint32_t events);
bool actor_unwatch_fd(runtime_t *rt, int fd);

/* ── Cross-node registry payloads ──────────────────────────────────── */

typedef struct {
    char       name[64];
    actor_id_t actor_id;
} name_register_payload_t;

typedef struct {
    char name[64];
} name_unregister_payload_t;

/* Path registry payloads (NS_PATH_MAX-sized path field) */
typedef struct {
    char       path[128];   /* NS_PATH_MAX */
    actor_id_t actor_id;
} path_register_payload_t;

typedef struct {
    char path[128];         /* NS_PATH_MAX */
} path_unregister_payload_t;

/* ── Name registry API ─────────────────────────────────────────────── */

bool       actor_register_name(runtime_t *rt, const char *name, actor_id_t id);
actor_id_t actor_lookup(runtime_t *rt, const char *name);

/* Send message by name — lookup + send in one call */
bool actor_send_named(runtime_t *rt, const char *name, msg_type_t type,
                      const void *payload, size_t payload_size);

/* Reverse lookup: find first name for actor ID (flat registry + namespace paths).
   Returns name length (0 if not found). */
size_t actor_reverse_lookup(runtime_t *rt, actor_id_t id,
                            char *buf, size_t buf_size);

/* Reverse lookup ALL names for an actor (comma+space separated).
   Returns total bytes written (0 if none found). */
size_t actor_reverse_lookup_all(runtime_t *rt, actor_id_t id,
                                char *buf, size_t buf_size);

/* ── Logging API ───────────────────────────────────────────────────── */

void runtime_enable_logging(runtime_t *rt);
void runtime_set_log_level(runtime_t *rt, int level);
void actor_log(runtime_t *rt, int level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* MICROKERNEL_SERVICES_H */
