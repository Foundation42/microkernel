#include "microkernel/midi.h"
#include "microkernel/runtime.h"
#include "microkernel/actor.h"
#include "microkernel/message.h"
#include "microkernel/services.h"
#include "midi_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/poll.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define MIDI_MAX_SUBS   8
#define MIDI_BOOTSTRAP  1
#define SYSEX_BUF_MAX   256

/* ── MIDI byte classification ─────────────────────────────────────── */

/* Returns expected message length for a status byte, or 0 for special */
static int midi_msg_len(uint8_t status) {
    if (status < 0x80) return 0; /* data byte */

    switch (status & 0xF0) {
    case 0x80: return 3; /* Note Off */
    case 0x90: return 3; /* Note On */
    case 0xA0: return 3; /* Poly Aftertouch */
    case 0xB0: return 3; /* Control Change */
    case 0xC0: return 2; /* Program Change */
    case 0xD0: return 2; /* Channel Aftertouch */
    case 0xE0: return 3; /* Pitch Bend */
    }

    /* System messages */
    switch (status) {
    case 0xF0: return 0; /* SysEx start — variable length */
    case 0xF1: return 2; /* MTC Quarter Frame */
    case 0xF2: return 3; /* Song Position Pointer */
    case 0xF3: return 2; /* Song Select */
    case 0xF6: return 1; /* Tune Request */
    case 0xF7: return 0; /* SysEx end */
    default:
        if (status >= 0xF8)
            return 1;    /* Real-Time: single byte */
        return 0;        /* 0xF4, 0xF5: undefined */
    }
}

static uint8_t midi_filter_for_status(uint8_t status) {
    switch (status & 0xF0) {
    case 0x80: case 0x90: return MIDI_FILTER_NOTE;
    case 0xA0:            return MIDI_FILTER_AFTERTOUCH;
    case 0xB0:            return MIDI_FILTER_CC;
    case 0xC0:            return MIDI_FILTER_PROGRAM;
    case 0xD0:            return MIDI_FILTER_AFTERTOUCH;
    case 0xE0:            return MIDI_FILTER_PITCHBEND;
    }
    if (status >= 0xF8) return MIDI_FILTER_REALTIME;
    return 0;
}

/* ── Actor state ──────────────────────────────────────────────────── */

typedef struct {
    actor_id_t subscriber;
    uint8_t    channel;     /* 0–15 or 0xFF=all */
    uint8_t    msg_filter;  /* bitmask */
} midi_sub_entry_t;

typedef struct {
    bool             configured;
    midi_sub_entry_t subs[MIDI_MAX_SUBS];

    /* MIDI parser state */
    uint8_t  running_status;
    uint8_t  parse_buf[3];
    int      parse_pos;
    int      expected_len;

    /* SysEx assembly buffer */
    uint8_t  sysex_buf[SYSEX_BUF_MAX];
    int      sysex_len;
    bool     in_sysex;
} midi_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void reply_error(runtime_t *rt, actor_id_t dest, const char *err) {
    actor_send(rt, dest, MSG_MIDI_ERROR, err, strlen(err));
}

static bool matches_filter(midi_sub_entry_t *sub, uint8_t status) {
    /* Channel filter (only applies to channel messages 0x80–0xEF) */
    if (status >= 0x80 && status <= 0xEF) {
        uint8_t channel = status & 0x0F;
        if (sub->channel != 0xFF && sub->channel != channel)
            return false;
    }

    uint8_t filter = midi_filter_for_status(status);
    return (sub->msg_filter & filter) != 0;
}

static void dispatch_event(midi_state_t *s, runtime_t *rt,
                           uint8_t status, uint8_t d1, uint8_t d2) {
    midi_event_payload_t ev;
    ev.status  = status;
    ev.data1   = d1;
    ev.data2   = d2;
    ev.channel = (status >= 0x80 && status <= 0xEF)
                 ? (status & 0x0F) : 0xFF;

    for (int i = 0; i < MIDI_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == ACTOR_ID_INVALID) continue;
        if (!matches_filter(&s->subs[i], status)) continue;

        if (!actor_send(rt, s->subs[i].subscriber, MSG_MIDI_EVENT,
                        &ev, sizeof(ev))) {
            /* Dead subscriber — auto-remove */
            s->subs[i].subscriber = ACTOR_ID_INVALID;
        }
    }
}

static void dispatch_sysex(midi_state_t *s, runtime_t *rt) {
    size_t payload_sz = sizeof(midi_sysex_event_payload_t) + (size_t)s->sysex_len;
    uint8_t *buf = malloc(payload_sz);
    if (!buf) return;

    midi_sysex_event_payload_t *ev = (midi_sysex_event_payload_t *)buf;
    ev->length = (uint16_t)s->sysex_len;
    ev->_pad[0] = ev->_pad[1] = 0;
    memcpy(ev->data, s->sysex_buf, (size_t)s->sysex_len);

    for (int i = 0; i < MIDI_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == ACTOR_ID_INVALID) continue;
        if (!(s->subs[i].msg_filter & MIDI_FILTER_SYSEX)) continue;

        if (!actor_send(rt, s->subs[i].subscriber, MSG_MIDI_SYSEX_EVENT,
                        buf, payload_sz)) {
            s->subs[i].subscriber = ACTOR_ID_INVALID;
        }
    }

    free(buf);
}

/* ── MIDI byte parser ─────────────────────────────────────────────── */

static void parse_byte(midi_state_t *s, runtime_t *rt, uint8_t byte) {
    /* Real-Time messages can appear anywhere — handle immediately */
    if (byte >= 0xF8) {
        dispatch_event(s, rt, byte, 0, 0);
        return;
    }

    /* SysEx accumulation */
    if (s->in_sysex) {
        if (byte == 0xF7) {
            /* End of SysEx */
            if (s->sysex_len < SYSEX_BUF_MAX)
                s->sysex_buf[s->sysex_len++] = byte;
            dispatch_sysex(s, rt);
            s->in_sysex = false;
            s->sysex_len = 0;
            s->running_status = 0; /* SysEx cancels running status */
            return;
        }
        if (byte >= 0x80) {
            /* Non-realtime status byte during SysEx — SysEx aborted */
            s->in_sysex = false;
            s->sysex_len = 0;
            /* Fall through to handle this byte as new status */
        } else {
            /* Data byte — accumulate */
            if (s->sysex_len < SYSEX_BUF_MAX)
                s->sysex_buf[s->sysex_len++] = byte;
            return;
        }
    }

    /* SysEx start */
    if (byte == 0xF0) {
        s->in_sysex = true;
        s->sysex_len = 0;
        s->sysex_buf[s->sysex_len++] = byte;
        s->running_status = 0;
        s->parse_pos = 0;
        return;
    }

    /* Status byte */
    if (byte >= 0x80) {
        int len = midi_msg_len(byte);
        if (len == 1) {
            /* Single-byte message (Tune Request) */
            dispatch_event(s, rt, byte, 0, 0);
            if (byte >= 0xF0 && byte <= 0xF7)
                s->running_status = 0; /* System Common cancels running status */
            s->parse_pos = 0;
            return;
        }
        if (len == 0) {
            /* Undefined or SysEx end outside SysEx — ignore */
            s->parse_pos = 0;
            return;
        }
        /* 2 or 3 byte message — start accumulating */
        s->parse_buf[0] = byte;
        s->parse_pos = 1;
        s->expected_len = len;
        /* Update running status only for channel messages */
        if (byte >= 0x80 && byte <= 0xEF)
            s->running_status = byte;
        else
            s->running_status = 0; /* System Common cancels running status */
        return;
    }

    /* Data byte (< 0x80) */
    if (s->parse_pos == 0) {
        /* No status yet — try running status */
        if (s->running_status == 0)
            return; /* No running status — discard */
        s->parse_buf[0] = s->running_status;
        s->parse_pos = 1;
        s->expected_len = midi_msg_len(s->running_status);
    }

    s->parse_buf[s->parse_pos++] = byte;

    if (s->parse_pos >= s->expected_len) {
        /* Complete message */
        dispatch_event(s, rt,
                       s->parse_buf[0],
                       s->parse_buf[1],
                       s->expected_len >= 3 ? s->parse_buf[2] : 0);
        s->parse_pos = 0;
    }
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void handle_configure(midi_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(midi_config_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const midi_config_payload_t *cfg = (const midi_config_payload_t *)msg->payload;

    if (s->configured)
        midi_hal_deconfigure();

    if (!midi_hal_configure(cfg->i2c_port, cfg->i2c_addr,
                            cfg->sda_pin, cfg->scl_pin,
                            cfg->irq_pin, cfg->i2c_freq_hz)) {
        reply_error(rt, msg->source, "hal configure failed");
        return;
    }

    s->configured = true;

    /* Reset parser state */
    s->running_status = 0;
    s->parse_pos = 0;
    s->expected_len = 0;
    s->in_sysex = false;
    s->sysex_len = 0;

    actor_send(rt, msg->source, MSG_MIDI_OK, NULL, 0);
}

static void handle_send(midi_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    if (msg->payload_size < sizeof(midi_send_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const midi_send_payload_t *req = (const midi_send_payload_t *)msg->payload;
    uint8_t status = req->status;

    if (status < 0x80) {
        reply_error(rt, msg->source, "invalid status byte");
        return;
    }

    int len = midi_msg_len(status);
    if (len <= 0) {
        reply_error(rt, msg->source, "unsupported status byte");
        return;
    }

    uint8_t buf[3];
    buf[0] = status;
    buf[1] = req->data1;
    buf[2] = req->data2;

    if (midi_hal_tx(buf, (size_t)len) != 0) {
        reply_error(rt, msg->source, "tx failed");
        return;
    }

    actor_send(rt, msg->source, MSG_MIDI_OK, NULL, 0);
}

static void handle_send_sysex(midi_state_t *s, runtime_t *rt, message_t *msg) {
    if (!s->configured) {
        reply_error(rt, msg->source, "not configured");
        return;
    }

    if (msg->payload_size < sizeof(midi_sysex_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const midi_sysex_payload_t *req = (const midi_sysex_payload_t *)msg->payload;
    uint16_t len = req->length;

    if (msg->payload_size < sizeof(midi_sysex_payload_t) + len) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    if (len < 2 || req->data[0] != 0xF0 || req->data[len - 1] != 0xF7) {
        reply_error(rt, msg->source, "invalid sysex framing");
        return;
    }

    if (midi_hal_tx(req->data, len) != 0) {
        reply_error(rt, msg->source, "tx failed");
        return;
    }

    actor_send(rt, msg->source, MSG_MIDI_OK, NULL, 0);
}

static void handle_subscribe(midi_state_t *s, runtime_t *rt, message_t *msg) {
    if (msg->payload_size < sizeof(midi_subscribe_payload_t)) {
        reply_error(rt, msg->source, "payload too small");
        return;
    }

    const midi_subscribe_payload_t *req =
        (const midi_subscribe_payload_t *)msg->payload;

    /* Idempotent: same subscriber → update filter */
    for (int i = 0; i < MIDI_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == msg->source) {
            s->subs[i].channel = req->channel;
            s->subs[i].msg_filter = req->msg_filter;
            actor_send(rt, msg->source, MSG_MIDI_OK, NULL, 0);
            return;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MIDI_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == ACTOR_ID_INVALID) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        reply_error(rt, msg->source, "too many subscriptions");
        return;
    }

    s->subs[slot].subscriber = msg->source;
    s->subs[slot].channel = req->channel;
    s->subs[slot].msg_filter = req->msg_filter;

    actor_send(rt, msg->source, MSG_MIDI_OK, NULL, 0);
}

static void handle_unsubscribe(midi_state_t *s, runtime_t *rt, message_t *msg) {
    for (int i = 0; i < MIDI_MAX_SUBS; i++) {
        if (s->subs[i].subscriber == msg->source)
            s->subs[i].subscriber = ACTOR_ID_INVALID;
    }
    actor_send(rt, msg->source, MSG_MIDI_OK, NULL, 0);
}

static void handle_rx_notification(midi_state_t *s, runtime_t *rt) {
    uint8_t buf[64];
    int n = midi_hal_drain_rx(buf, (int)sizeof(buf));
    if (n <= 0) return;

    for (int i = 0; i < n; i++)
        parse_byte(s, rt, buf[i]);
}

/* ── Actor behavior ───────────────────────────────────────────────── */

static bool midi_behavior(runtime_t *rt, actor_t *self,
                           message_t *msg, void *state) {
    (void)self;
    midi_state_t *s = state;

    if (msg->type == MIDI_BOOTSTRAP) {
        int fd = midi_hal_get_notify_fd();
        if (fd >= 0)
            actor_watch_fd(rt, fd, POLLIN);
        return true;
    }

    if (msg->type == MSG_FD_EVENT) {
        handle_rx_notification(s, rt);
        return true;
    }

    switch (msg->type) {
    case MSG_MIDI_CONFIGURE:   handle_configure(s, rt, msg);    break;
    case MSG_MIDI_SEND:        handle_send(s, rt, msg);         break;
    case MSG_MIDI_SEND_SYSEX:  handle_send_sysex(s, rt, msg);  break;
    case MSG_MIDI_SUBSCRIBE:   handle_subscribe(s, rt, msg);    break;
    case MSG_MIDI_UNSUBSCRIBE: handle_unsubscribe(s, rt, msg);  break;
    default: break;
    }

    return true;
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void midi_state_free(void *state) {
    midi_state_t *s = state;
    if (s->configured)
        midi_hal_deconfigure();
    midi_hal_deinit();
    free(state);
}

/* ── Init ─────────────────────────────────────────────────────────── */

actor_id_t midi_actor_init(runtime_t *rt) {
    if (!midi_hal_init())
        return ACTOR_ID_INVALID;

    midi_state_t *s = calloc(1, sizeof(*s));
    if (!s) {
        midi_hal_deinit();
        return ACTOR_ID_INVALID;
    }

    for (int i = 0; i < MIDI_MAX_SUBS; i++)
        s->subs[i].subscriber = ACTOR_ID_INVALID;

    actor_id_t id = actor_spawn(rt, midi_behavior, s, midi_state_free, 32);
    if (id == ACTOR_ID_INVALID) {
        midi_hal_deinit();
        free(s);
        return ACTOR_ID_INVALID;
    }

    actor_register_name(rt, "/node/hardware/midi", id);

    /* Bootstrap triggers FD watch setup inside actor context */
    actor_send(rt, id, MIDI_BOOTSTRAP, NULL, 0);

    return id;
}
