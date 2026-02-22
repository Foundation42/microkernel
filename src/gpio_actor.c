#include "microkernel/gpio.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "payload_util.h"
#include "gpio_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/poll.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define GPIO_MAX_PINS 16
#define GPIO_MAX_SUBS 16
#define GPIO_BOOTSTRAP 1

/* GPIO modes (matches gpio.h doc) */
#define MODE_INPUT          0
#define MODE_OUTPUT         1
#define MODE_INPUT_PULLUP   2
#define MODE_INPUT_PULLDOWN 3

/* Edge types */
#define EDGE_RISING  0
#define EDGE_FALLING 1
#define EDGE_BOTH    2

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    int pin;
    int mode; /* -1 = unused */
} gpio_pin_entry_t;

typedef struct {
    int        pin;
    actor_id_t subscriber;
    int        edge;
} gpio_sub_entry_t;

typedef struct {
    gpio_pin_entry_t pins[GPIO_MAX_PINS];
    gpio_sub_entry_t subs[GPIO_MAX_SUBS];
} gpio_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static int parse_mode(const char *s) {
    if (!s[0]) return -1;
    if (strcmp(s, "output") == 0)         return MODE_OUTPUT;
    if (strcmp(s, "input") == 0)          return MODE_INPUT;
    if (strcmp(s, "input_pullup") == 0)   return MODE_INPUT_PULLUP;
    if (strcmp(s, "input_pulldown") == 0) return MODE_INPUT_PULLDOWN;
    return -1;
}

static int parse_edge(const char *s) {
    if (!s[0]) return EDGE_BOTH;
    if (strcmp(s, "rising") == 0)  return EDGE_RISING;
    if (strcmp(s, "falling") == 0) return EDGE_FALLING;
    if (strcmp(s, "both") == 0)    return EDGE_BOTH;
    return EDGE_BOTH;
}

static int find_pin_slot(gpio_state_t *s, int pin) {
    for (int i = 0; i < GPIO_MAX_PINS; i++)
        if (s->pins[i].mode >= 0 && s->pins[i].pin == pin)
            return i;
    return -1;
}

static void remove_pin_subs(gpio_state_t *s, int pin) {
    for (int i = 0; i < GPIO_MAX_SUBS; i++) {
        if (s->subs[i].subscriber != ACTOR_ID_INVALID && s->subs[i].pin == pin) {
            s->subs[i].subscriber = ACTOR_ID_INVALID;
        }
    }
}

static const char *edge_name(int edge) {
    switch (edge) {
    case EDGE_RISING:  return "rising";
    case EDGE_FALLING: return "falling";
    default:           return "both";
    }
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void handle_configure(gpio_state_t *s, runtime_t *rt, message_t *msg) {
    char pin_s[8] = "", mode_s[32] = "";
    payload_get(msg->payload, msg->payload_size, "pin", pin_s, sizeof(pin_s));
    payload_get(msg->payload, msg->payload_size, "mode", mode_s, sizeof(mode_s));

    int pin = atoi(pin_s);
    int mode = parse_mode(mode_s);
    if (mode < 0) {
        const char *err = "invalid mode";
        actor_send(rt, msg->source, MSG_GPIO_ERROR, err, strlen(err));
        return;
    }

    int idx = find_pin_slot(s, pin);
    if (idx >= 0) {
        /* Reconfiguring: if input→output, remove ISR + subscriptions */
        if (s->pins[idx].mode != MODE_OUTPUT && mode == MODE_OUTPUT) {
            gpio_hal_isr_remove(pin);
            remove_pin_subs(s, pin);
        }
        s->pins[idx].mode = mode;
    } else {
        /* Find free slot */
        idx = -1;
        for (int i = 0; i < GPIO_MAX_PINS; i++) {
            if (s->pins[i].mode < 0) { idx = i; break; }
        }
        if (idx < 0) {
            const char *err = "too many pins";
            actor_send(rt, msg->source, MSG_GPIO_ERROR, err, strlen(err));
            return;
        }
        s->pins[idx].pin = pin;
        s->pins[idx].mode = mode;
    }

    if (!gpio_hal_configure(pin, mode)) {
        const char *err = "hal configure failed";
        actor_send(rt, msg->source, MSG_GPIO_ERROR, err, strlen(err));
        return;
    }

    actor_send(rt, msg->source, MSG_GPIO_OK, NULL, 0);
}

static void handle_write(gpio_state_t *s, runtime_t *rt, message_t *msg) {
    (void)s;
    char pin_s[8] = "", val_s[8] = "";
    payload_get(msg->payload, msg->payload_size, "pin", pin_s, sizeof(pin_s));
    payload_get(msg->payload, msg->payload_size, "value", val_s, sizeof(val_s));

    int pin = atoi(pin_s);
    int val = atoi(val_s);

    if (!gpio_hal_write(pin, val)) {
        const char *err = "write failed";
        actor_send(rt, msg->source, MSG_GPIO_ERROR, err, strlen(err));
        return;
    }

    actor_send(rt, msg->source, MSG_GPIO_OK, NULL, 0);
}

static void handle_read(gpio_state_t *s, runtime_t *rt, message_t *msg) {
    (void)s;
    char pin_s[8] = "";
    payload_get(msg->payload, msg->payload_size, "pin", pin_s, sizeof(pin_s));

    int pin = atoi(pin_s);
    int val = gpio_hal_read(pin);

    if (val < 0) {
        const char *err = "read failed";
        actor_send(rt, msg->source, MSG_GPIO_ERROR, err, strlen(err));
        return;
    }

    char resp[32];
    int n = snprintf(resp, sizeof(resp), "pin=%d\nvalue=%d", pin, val);
    actor_send(rt, msg->source, MSG_GPIO_VALUE, resp, (size_t)n);
}

static void handle_subscribe(gpio_state_t *s, runtime_t *rt, message_t *msg) {
    char pin_s[8] = "", edge_s[16] = "";
    payload_get(msg->payload, msg->payload_size, "pin", pin_s, sizeof(pin_s));
    payload_get(msg->payload, msg->payload_size, "edge", edge_s, sizeof(edge_s));

    int pin = atoi(pin_s);
    int edge = parse_edge(edge_s);

    /* Idempotent: same subscriber + pin → update edge */
    int slot = -1;
    for (int i = 0; i < GPIO_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == msg->source && s->subs[i].pin == pin) {
            s->subs[i].edge = edge;
            actor_send(rt, msg->source, MSG_GPIO_OK, NULL, 0);
            return;
        }
        if (slot < 0 && s->subs[i].subscriber == ACTOR_ID_INVALID)
            slot = i;
    }

    if (slot < 0) {
        const char *err = "too many subscriptions";
        actor_send(rt, msg->source, MSG_GPIO_ERROR, err, strlen(err));
        return;
    }

    /* Install ISR if this is the first subscription for this pin */
    bool has_isr = false;
    for (int i = 0; i < GPIO_MAX_SUBS; i++) {
        if (s->subs[i].subscriber != ACTOR_ID_INVALID && s->subs[i].pin == pin) {
            has_isr = true;
            break;
        }
    }
    if (!has_isr)
        gpio_hal_isr_add(pin, EDGE_BOTH);

    s->subs[slot].pin = pin;
    s->subs[slot].subscriber = msg->source;
    s->subs[slot].edge = edge;

    actor_send(rt, msg->source, MSG_GPIO_OK, NULL, 0);
}

static void handle_unsubscribe(gpio_state_t *s, runtime_t *rt, message_t *msg) {
    char pin_s[8] = "";
    payload_get(msg->payload, msg->payload_size, "pin", pin_s, sizeof(pin_s));
    int pin = atoi(pin_s);

    for (int i = 0; i < GPIO_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == msg->source && s->subs[i].pin == pin)
            s->subs[i].subscriber = ACTOR_ID_INVALID;
    }

    /* Remove ISR if no subscribers left on this pin */
    bool any = false;
    for (int i = 0; i < GPIO_MAX_SUBS; i++) {
        if (s->subs[i].subscriber != ACTOR_ID_INVALID && s->subs[i].pin == pin) {
            any = true;
            break;
        }
    }
    if (!any)
        gpio_hal_isr_remove(pin);

    actor_send(rt, msg->source, MSG_GPIO_OK, NULL, 0);
}

static void handle_isr_notification(gpio_state_t *s, runtime_t *rt) {
    uint8_t buf[32];
    int n = gpio_hal_drain_events(buf, (int)sizeof(buf));
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        int pin = buf[i];
        int value = gpio_hal_read(pin);
        if (value < 0) value = 0;

        int detected_edge = value ? EDGE_RISING : EDGE_FALLING;

        char payload[64];
        int plen = snprintf(payload, sizeof(payload),
                            "pin=%d\nvalue=%d\nedge=%s",
                            pin, value, edge_name(detected_edge));

        for (int j = 0; j < GPIO_MAX_SUBS; j++) {
            if (s->subs[j].subscriber == ACTOR_ID_INVALID) continue;
            if (s->subs[j].pin != pin) continue;

            /* Edge filter */
            if (s->subs[j].edge != EDGE_BOTH &&
                s->subs[j].edge != detected_edge)
                continue;

            if (!actor_send(rt, s->subs[j].subscriber, MSG_GPIO_EVENT,
                            payload, (size_t)plen)) {
                /* Dead subscriber — auto-remove */
                s->subs[j].subscriber = ACTOR_ID_INVALID;
            }
        }
    }
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool gpio_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    gpio_state_t *s = state;

    if (msg->type == GPIO_BOOTSTRAP) {
        /* Set up FD watch (requires behavior context) */
        int fd = gpio_hal_get_notify_fd();
        if (fd >= 0)
            actor_watch_fd(rt, fd, POLLIN);
        return true;
    }

    if (msg->type == MSG_FD_EVENT) {
        handle_isr_notification(s, rt);
        return true;
    }

    switch (msg->type) {
    case MSG_GPIO_CONFIGURE:   handle_configure(s, rt, msg);   break;
    case MSG_GPIO_WRITE:       handle_write(s, rt, msg);       break;
    case MSG_GPIO_READ:        handle_read(s, rt, msg);        break;
    case MSG_GPIO_SUBSCRIBE:   handle_subscribe(s, rt, msg);   break;
    case MSG_GPIO_UNSUBSCRIBE: handle_unsubscribe(s, rt, msg); break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void gpio_state_free(void *state) {
    gpio_hal_deinit();
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t gpio_actor_init(runtime_t *rt) {
    if (!gpio_hal_init())
        return ACTOR_ID_INVALID;

    gpio_state_t *s = calloc(1, sizeof(*s));
    if (!s) {
        gpio_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    for (int i = 0; i < GPIO_MAX_PINS; i++)
        s->pins[i].mode = -1;
    for (int i = 0; i < GPIO_MAX_SUBS; i++)
        s->subs[i].subscriber = ACTOR_ID_INVALID;

    actor_id_t id = actor_spawn(rt, gpio_behavior, s, gpio_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        gpio_hal_deinit();
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/gpio", id);

    /* Bootstrap triggers FD watch setup inside actor context */
    actor_send(rt, id, GPIO_BOOTSTRAP, NULL, 0);

    return id;
}
