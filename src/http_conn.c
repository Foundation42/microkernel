#include "microkernel/http.h"
#include "microkernel/mk_socket.h"
#include "microkernel/message.h"
#include "runtime_internal.h"
#include "url_parse.h"
#include "sha1.h"
#include "base64.h"
#include "ws_frame.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

/* ── Buffer helpers ────────────────────────────────────────────────── */

static void buf_consume(http_conn_t *conn, size_t n) {
    if (n >= conn->read_len) {
        conn->read_len = 0;
    } else {
        memmove(conn->read_buf, conn->read_buf + n, conn->read_len - n);
        conn->read_len -= n;
    }
}

/* Find \r\n in buffer. Returns offset or -1. */
static ssize_t find_crlf(http_conn_t *conn) {
    for (size_t i = 0; i + 1 < conn->read_len; i++) {
        if (conn->read_buf[i] == '\r' && conn->read_buf[i + 1] == '\n')
            return (ssize_t)i;
    }
    return -1;
}

/* Append to dynamic buffer */
static bool dyn_append(uint8_t **buf, size_t *size, size_t *cap,
                       const void *data, size_t len) {
    if (*size + len > *cap) {
        size_t new_cap = (*cap == 0) ? 1024 : *cap * 2;
        while (new_cap < *size + len) new_cap *= 2;
        uint8_t *new_buf = realloc(*buf, new_cap);
        if (!new_buf) return false;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *size, data, len);
    *size += len;
    return true;
}

/* ── Delivery helpers ──────────────────────────────────────────────── */

static void deliver_http_response(http_conn_t *conn, runtime_t *rt) {
    /* Build variable-size payload: [header struct][headers_buf][body_buf] */
    size_t total = sizeof(http_response_payload_t) +
                   conn->headers_size + conn->body_size;
    uint8_t *buf = malloc(total);
    if (!buf) return;

    http_response_payload_t *p = (http_response_payload_t *)buf;
    p->conn_id = conn->id;
    p->status_code = conn->status_code;
    p->headers_size = conn->headers_size;
    p->body_size = conn->body_size;

    if (conn->headers_buf && conn->headers_size > 0)
        memcpy(buf + sizeof(*p), conn->headers_buf, conn->headers_size);
    if (conn->body_buf && conn->body_size > 0)
        memcpy(buf + sizeof(*p) + conn->headers_size,
               conn->body_buf, conn->body_size);

    runtime_deliver_msg(rt, conn->owner, MSG_HTTP_RESPONSE, buf, total);
    free(buf);
}

static void deliver_http_error(http_conn_t *conn, runtime_t *rt,
                               const char *msg) {
    http_error_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.conn_id = conn->id;
    payload.error_code = -1;
    if (msg) snprintf(payload.message, sizeof(payload.message), "%s", msg);
    runtime_deliver_msg(rt, conn->owner, MSG_HTTP_ERROR,
                        &payload, sizeof(payload));
}

static void deliver_sse_open(http_conn_t *conn, runtime_t *rt) {
    sse_status_payload_t payload = {
        .conn_id = conn->id,
        .status_code = conn->status_code
    };
    runtime_deliver_msg(rt, conn->owner, MSG_SSE_OPEN,
                        &payload, sizeof(payload));
}

static void deliver_sse_event(http_conn_t *conn, runtime_t *rt) {
    size_t event_len = strlen(conn->sse_event) + 1; /* include null */
    size_t data_len = conn->sse_data_size;
    size_t total = sizeof(sse_event_payload_t) + event_len + data_len;

    uint8_t *buf = malloc(total);
    if (!buf) return;

    sse_event_payload_t *p = (sse_event_payload_t *)buf;
    p->conn_id = conn->id;
    p->event_size = event_len;
    p->data_size = data_len;

    memcpy(buf + sizeof(*p), conn->sse_event, event_len);
    if (conn->sse_data && data_len > 0)
        memcpy(buf + sizeof(*p) + event_len, conn->sse_data, data_len);

    runtime_deliver_msg(rt, conn->owner, MSG_SSE_EVENT, buf, total);
    free(buf);

    /* Reset SSE accumulator */
    strcpy(conn->sse_event, "message");
    conn->sse_data_size = 0;
}

static void deliver_sse_closed(http_conn_t *conn, runtime_t *rt) {
    sse_status_payload_t payload = {
        .conn_id = conn->id,
        .status_code = conn->status_code
    };
    runtime_deliver_msg(rt, conn->owner, MSG_SSE_CLOSED,
                        &payload, sizeof(payload));
}

static void deliver_ws_open(http_conn_t *conn, runtime_t *rt) {
    ws_status_payload_t payload = {
        .conn_id = conn->id,
        .close_code = 0
    };
    runtime_deliver_msg(rt, conn->owner, MSG_WS_OPEN,
                        &payload, sizeof(payload));
}

static void deliver_ws_message(http_conn_t *conn, runtime_t *rt,
                               bool is_binary, const void *data, size_t len) {
    size_t total = sizeof(ws_message_payload_t) + len;
    uint8_t *buf = malloc(total);
    if (!buf) return;

    ws_message_payload_t *p = (ws_message_payload_t *)buf;
    p->conn_id = conn->id;
    p->is_binary = is_binary;
    p->data_size = len;
    if (len > 0) memcpy(buf + sizeof(*p), data, len);

    runtime_deliver_msg(rt, conn->owner, MSG_WS_MESSAGE, buf, total);
    free(buf);
}

static void deliver_ws_closed(http_conn_t *conn, runtime_t *rt,
                              uint16_t code) {
    ws_status_payload_t payload = {
        .conn_id = conn->id,
        .close_code = code
    };
    runtime_deliver_msg(rt, conn->owner, MSG_WS_CLOSED,
                        &payload, sizeof(payload));
}

static void deliver_ws_error(http_conn_t *conn, runtime_t *rt) {
    ws_status_payload_t payload = {
        .conn_id = conn->id,
        .close_code = 0
    };
    runtime_deliver_msg(rt, conn->owner, MSG_WS_ERROR,
                        &payload, sizeof(payload));
}

/* ── Transition to error state ─────────────────────────────────────── */

static void conn_error(http_conn_t *conn, runtime_t *rt, const char *msg) {
    conn->state = HTTP_STATE_ERROR;
    if (conn->conn_type == HTTP_CONN_WS &&
        conn->state != HTTP_STATE_RECV_STATUS &&
        conn->state != HTTP_STATE_RECV_HEADERS) {
        deliver_ws_error(conn, rt);
    } else if (conn->conn_type == HTTP_CONN_SSE) {
        deliver_sse_closed(conn, rt);
    } else {
        deliver_http_error(conn, rt, msg);
    }
}

/* ── Request building ──────────────────────────────────────────────── */

static uint8_t *build_http_request(const char *method, const parsed_url_t *url,
                                   const char *const *headers, size_t n_headers,
                                   const void *body, size_t body_size,
                                   bool is_sse, size_t *out_size) {
    /* Estimate buffer size */
    size_t cap = 512 + body_size;
    for (size_t i = 0; i < n_headers; i++)
        cap += strlen(headers[i]) + 4;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;

    uint16_t port = url_effective_port(url);
    int pos;

    /* Request line */
    if ((port == 80 && strcmp(url->scheme, "http") == 0) ||
        (port == 443 && strcmp(url->scheme, "https") == 0)) {
        pos = snprintf((char *)buf, cap, "%s %s HTTP/1.1\r\n",
                       method, url->path);
    } else {
        pos = snprintf((char *)buf, cap, "%s %s HTTP/1.1\r\n",
                       method, url->path);
    }

    /* Host header */
    if (url->port != 0) {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "Host: %s:%u\r\n", url->host, url->port);
    } else {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "Host: %s\r\n", url->host);
    }

    /* User headers */
    for (size_t i = 0; i < n_headers; i++) {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "%s\r\n", headers[i]);
    }

    /* SSE-specific headers */
    if (is_sse) {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "Accept: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n");
    }

    /* Content-Length for body */
    if (body && body_size > 0) {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "Content-Length: %zu\r\n", body_size);
    }

    /* End of headers */
    pos += snprintf((char *)buf + pos, cap - (size_t)pos, "\r\n");

    /* Body */
    if (body && body_size > 0) {
        memcpy(buf + pos, body, body_size);
        pos += (int)body_size;
    }

    *out_size = (size_t)pos;
    return buf;
}

static uint8_t *build_ws_handshake(const parsed_url_t *url,
                                   char *accept_key_out,
                                   size_t *out_size) {
    /* Generate random 16-byte key, base64 encode */
    uint8_t raw_key[16];
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 16; i++) raw_key[i] = (uint8_t)(rand() & 0xFF);

    char key_b64[25];
    base64_encode(raw_key, 16, key_b64);

    /* Compute expected accept key: SHA-1(key + magic GUID) then base64 */
    static const char *ws_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", key_b64, ws_guid);

    uint8_t sha1_hash[20];
    sha1((const uint8_t *)concat, strlen(concat), sha1_hash);
    base64_encode(sha1_hash, 20, accept_key_out);

    /* Build request */
    size_t cap = 512;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;

    uint16_t port = url_effective_port(url);
    int pos;

    /* Use ws path, but speak HTTP */
    pos = snprintf((char *)buf, cap, "GET %s HTTP/1.1\r\n", url->path);

    if (url->port != 0) {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "Host: %s:%u\r\n", url->host, port);
    } else {
        pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                        "Host: %s\r\n", url->host);
    }

    pos += snprintf((char *)buf + pos, cap - (size_t)pos,
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Key: %s\r\n"
                    "\r\n", key_b64);

    *out_size = (size_t)pos;
    return buf;
}

/* ── Status line parsing ───────────────────────────────────────────── */

static bool parse_status_line(http_conn_t *conn) {
    ssize_t crlf = find_crlf(conn);
    if (crlf < 0) return false;

    /* Parse "HTTP/1.x NNN reason" */
    char *line = (char *)conn->read_buf;
    if (crlf < 12 || strncmp(line, "HTTP/1.", 7) != 0) {
        conn->state = HTTP_STATE_ERROR;
        return false;
    }

    /* Skip to status code */
    char *sp = memchr(line, ' ', (size_t)crlf);
    if (!sp) { conn->state = HTTP_STATE_ERROR; return false; }

    conn->status_code = (int)strtol(sp + 1, NULL, 10);
    if (conn->status_code < 100 || conn->status_code > 599) {
        conn->state = HTTP_STATE_ERROR;
        return false;
    }

    buf_consume(conn, (size_t)crlf + 2);
    conn->state = HTTP_STATE_RECV_HEADERS;
    conn->content_length = -1;
    conn->chunked = false;
    conn->upgrade_ws = false;
    return true;
}

/* ── Header parsing ────────────────────────────────────────────────── */

static bool parse_header_line(http_conn_t *conn, runtime_t *rt) {
    ssize_t crlf = find_crlf(conn);
    if (crlf < 0) return false;

    if (crlf == 0) {
        /* Empty line = end of headers */
        buf_consume(conn, 2);

        /* Decide next state based on connection type and headers */
        if (conn->conn_type == HTTP_CONN_WS) {
            if (conn->status_code == 101 && conn->upgrade_ws) {
                conn->state = HTTP_STATE_WS_ACTIVE;
                deliver_ws_open(conn, rt);
            } else {
                conn->state = HTTP_STATE_ERROR;
                deliver_ws_error(conn, rt);
            }
        } else if (conn->conn_type == HTTP_CONN_SSE) {
            if (conn->status_code >= 200 && conn->status_code < 300) {
                conn->state = HTTP_STATE_BODY_STREAM;
                strcpy(conn->sse_event, "message");
                conn->sse_data_size = 0;
                deliver_sse_open(conn, rt);
            } else {
                /* Non-2xx for SSE = error */
                conn->state = HTTP_STATE_ERROR;
                deliver_http_error(conn, rt, "SSE connection failed");
            }
        } else {
            /* Regular HTTP */
            if (conn->chunked) {
                conn->state = HTTP_STATE_BODY_CHUNKED;
                conn->chunk_remaining = 0;
                conn->in_chunk_data = false;
            } else if (conn->content_length > 0) {
                conn->state = HTTP_STATE_BODY_CONTENT;
            } else if (conn->content_length == 0 ||
                       conn->status_code == 204 ||
                       conn->status_code == 304) {
                /* No body */
                conn->state = HTTP_STATE_DONE;
                deliver_http_response(conn, rt);
            } else {
                /* Unknown length: read until close */
                conn->state = HTTP_STATE_BODY_CONTENT;
            }
        }
        return true;
    }

    /* Parse header line */
    char *line = (char *)conn->read_buf;
    char *colon = memchr(line, ':', (size_t)crlf);

    if (colon) {
        size_t name_len = (size_t)(colon - line);

        /* Skip whitespace after colon */
        char *val = colon + 1;
        while (val < line + crlf && *val == ' ') val++;
        size_t val_len = (size_t)(line + crlf - val);

        /* Check for important headers (case-insensitive) */
        if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
            conn->content_length = strtol(val, NULL, 10);
        } else if (name_len == 17 &&
                   strncasecmp(line, "Transfer-Encoding", 17) == 0) {
            if (val_len >= 7 && strncasecmp(val, "chunked", 7) == 0)
                conn->chunked = true;
        } else if (name_len == 7 &&
                   strncasecmp(line, "Upgrade", 7) == 0) {
            if (val_len >= 9 && strncasecmp(val, "websocket", 9) == 0)
                conn->upgrade_ws = true;
        } else if (name_len == 20 &&
                   strncasecmp(line, "Sec-WebSocket-Accept", 20) == 0) {
            /* Validate WS accept key */
            if (conn->conn_type == HTTP_CONN_WS) {
                size_t expect_len = strlen(conn->ws_accept_key);
                if (val_len != expect_len ||
                    memcmp(val, conn->ws_accept_key, expect_len) != 0) {
                    conn->state = HTTP_STATE_ERROR;
                    buf_consume(conn, (size_t)crlf + 2);
                    return true;
                }
            }
        }

        /* Accumulate header: "Name: Value\0" */
        size_t header_line_len = (size_t)crlf + 1; /* include null terminator */
        dyn_append((uint8_t **)&conn->headers_buf, &conn->headers_size,
                   &conn->headers_cap, line, (size_t)crlf);
        /* Null-terminate this header entry */
        char nul = '\0';
        dyn_append((uint8_t **)&conn->headers_buf, &conn->headers_size,
                   &conn->headers_cap, &nul, 1);
        (void)header_line_len;
    }

    buf_consume(conn, (size_t)crlf + 2);
    return true;
}

/* ── Body reading (Content-Length) ─────────────────────────────────── */

static bool consume_body_content(http_conn_t *conn, runtime_t *rt) {
    if (conn->read_len == 0) return false;

    size_t avail = conn->read_len;

    if (conn->content_length >= 0) {
        size_t remaining = (size_t)conn->content_length - conn->body_size;
        if (avail > remaining) avail = remaining;
    }

    if (avail > 0) {
        dyn_append(&conn->body_buf, &conn->body_size, &conn->body_cap,
                   conn->read_buf, avail);
        buf_consume(conn, avail);
    }

    if (conn->content_length >= 0 &&
        conn->body_size >= (size_t)conn->content_length) {
        conn->state = HTTP_STATE_DONE;
        deliver_http_response(conn, rt);
    }

    return avail > 0;
}

/* ── Chunked transfer ──────────────────────────────────────────────── */

static bool consume_body_chunked(http_conn_t *conn, runtime_t *rt) {
    if (conn->read_len == 0) return false;

    if (conn->in_chunk_data) {
        /* Reading chunk data */
        size_t avail = conn->read_len;
        if (avail > conn->chunk_remaining) avail = conn->chunk_remaining;

        if (avail > 0) {
            dyn_append(&conn->body_buf, &conn->body_size, &conn->body_cap,
                       conn->read_buf, avail);
            buf_consume(conn, avail);
            conn->chunk_remaining -= avail;
        }

        if (conn->chunk_remaining == 0) {
            /* Expect trailing \r\n after chunk data */
            if (conn->read_len >= 2 &&
                conn->read_buf[0] == '\r' && conn->read_buf[1] == '\n') {
                buf_consume(conn, 2);
                conn->in_chunk_data = false;
                return true;
            }
            /* Need more data for \r\n */
            return avail > 0;
        }
        return avail > 0;
    }

    /* Reading chunk size line */
    ssize_t crlf = find_crlf(conn);
    if (crlf < 0) return false;

    /* Parse hex chunk size */
    char size_str[32];
    size_t copy = (size_t)crlf < sizeof(size_str) - 1 ?
                  (size_t)crlf : sizeof(size_str) - 1;
    memcpy(size_str, conn->read_buf, copy);
    size_str[copy] = '\0';

    /* Ignore chunk extensions (after semicolon) */
    char *semi = strchr(size_str, ';');
    if (semi) *semi = '\0';

    char *endptr;
    unsigned long chunk_size = strtoul(size_str, &endptr, 16);
    buf_consume(conn, (size_t)crlf + 2);

    if (chunk_size == 0) {
        /* Last chunk — skip trailing headers/CRLF */
        conn->state = HTTP_STATE_DONE;
        deliver_http_response(conn, rt);
        return true;
    }

    conn->chunk_remaining = chunk_size;
    conn->in_chunk_data = true;
    return true;
}

/* ── SSE parsing ───────────────────────────────────────────────────── */

static bool process_sse_line(http_conn_t *conn, runtime_t *rt,
                             const char *line, size_t len) {
    if (len == 0) {
        /* Blank line = dispatch event (if we have data) */
        if (conn->sse_data_size > 0) {
            /* Remove trailing newline from data if present */
            if (conn->sse_data_size > 0 &&
                conn->sse_data[conn->sse_data_size - 1] == '\n') {
                conn->sse_data_size--;
            }
            /* Null-terminate data */
            char nul = '\0';
            dyn_append((uint8_t **)&conn->sse_data, &conn->sse_data_size,
                       &conn->sse_data_cap, &nul, 1);
            conn->sse_data_size--; /* don't count null in size */
            deliver_sse_event(conn, rt);
        }
        return true;
    }

    /* Comment line */
    if (line[0] == ':') return true;

    /* Parse "field: value" or "field:value" or just "field" */
    const char *colon = memchr(line, ':', len);
    const char *field = line;
    size_t field_len;
    const char *value = "";
    size_t value_len = 0;

    if (colon) {
        field_len = (size_t)(colon - line);
        value = colon + 1;
        value_len = len - field_len - 1;
        /* Skip single leading space after colon */
        if (value_len > 0 && value[0] == ' ') {
            value++;
            value_len--;
        }
    } else {
        field_len = len;
    }

    if (field_len == 5 && memcmp(field, "event", 5) == 0) {
        size_t copy = value_len < sizeof(conn->sse_event) - 1 ?
                      value_len : sizeof(conn->sse_event) - 1;
        memcpy(conn->sse_event, value, copy);
        conn->sse_event[copy] = '\0';
    } else if (field_len == 4 && memcmp(field, "data", 4) == 0) {
        if (conn->sse_data_size > 0) {
            /* Append newline between data lines */
            dyn_append((uint8_t **)&conn->sse_data, &conn->sse_data_size,
                       &conn->sse_data_cap, "\n", 1);
        }
        dyn_append((uint8_t **)&conn->sse_data, &conn->sse_data_size,
                   &conn->sse_data_cap, value, value_len);
    }
    /* Ignore "id", "retry", and unknown fields */

    return true;
}

static bool process_sse_data(http_conn_t *conn, runtime_t *rt) {
    /* Look for \n (SSE lines are \n-terminated, may also use \r\n) */
    for (size_t i = 0; i < conn->read_len; i++) {
        if (conn->read_buf[i] == '\n') {
            /* Check for \r\n */
            size_t line_len = i;
            if (line_len > 0 && conn->read_buf[line_len - 1] == '\r')
                line_len--;

            process_sse_line(conn, rt, (char *)conn->read_buf, line_len);
            buf_consume(conn, i + 1);
            return true;
        }
    }
    return false;
}

/* ── WebSocket data processing ─────────────────────────────────────── */

static bool send_ws_frame(http_conn_t *conn, uint8_t opcode,
                          const uint8_t *payload, size_t len) {
    uint8_t frame[14 + 125]; /* small inline buffer for control frames */
    uint8_t *buf;
    bool heap = false;

    if (len + 14 > sizeof(frame)) {
        buf = malloc(len + 14);
        if (!buf) return false;
        heap = true;
    } else {
        buf = frame;
    }

    size_t frame_size = ws_frame_build(opcode, true, payload, len, buf);

    /* Blocking write */
    size_t written = 0;
    while (written < frame_size) {
        ssize_t n = conn->sock->write(conn->sock,
                                       buf + written, frame_size - written);
        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Spin briefly — for small frames this should resolve quickly */
            continue;
        } else {
            if (heap) free(buf);
            return false;
        }
    }

    if (heap) free(buf);
    return true;
}

static bool process_ws_data(http_conn_t *conn, runtime_t *rt) {
    if (conn->read_len < 2) return false;

    ws_frame_info_t info;
    int hdr_len = ws_frame_parse_header(conn->read_buf, conn->read_len, &info);
    if (hdr_len < 0) {
        conn->state = HTTP_STATE_ERROR;
        deliver_ws_error(conn, rt);
        return false;
    }
    if (hdr_len == 0) return false; /* need more header data */

    /* Check if we have the full payload */
    size_t total_frame = info.header_size + info.payload_length;
    if (conn->read_len < total_frame) return false;

    /* Extract payload */
    uint8_t *payload = conn->read_buf + info.header_size;
    size_t plen = (size_t)info.payload_length;

    /* Unmask if needed (server frames usually aren't masked, but handle it) */
    if (info.masked) {
        ws_frame_apply_mask(payload, plen, info.mask_key, 0);
    }

    switch (info.opcode) {
    case WS_OPCODE_TEXT:
        deliver_ws_message(conn, rt, false, payload, plen);
        break;
    case WS_OPCODE_BINARY:
        deliver_ws_message(conn, rt, true, payload, plen);
        break;
    case WS_OPCODE_CLOSE: {
        uint16_t code = 1000;
        if (plen >= 2) {
            code = ((uint16_t)payload[0] << 8) | payload[1];
        }
        /* Send close response */
        uint8_t close_frame[128];
        size_t close_len = ws_frame_build_close(code, NULL, 0, close_frame);
        conn->sock->write(conn->sock, close_frame, close_len);
        conn->state = HTTP_STATE_DONE;
        deliver_ws_closed(conn, rt, code);
        buf_consume(conn, total_frame);
        return true;
    }
    case WS_OPCODE_PING:
        /* Auto-respond with pong */
        send_ws_frame(conn, WS_OPCODE_PONG, payload, plen);
        break;
    case WS_OPCODE_PONG:
        /* Ignore pong */
        break;
    default:
        break;
    }

    buf_consume(conn, total_frame);
    return true;
}

/* ── Main driver ───────────────────────────────────────────────────── */

void http_conn_drive(http_conn_t *conn, short revents, runtime_t *rt) {
    if (conn->state == HTTP_STATE_DONE || conn->state == HTTP_STATE_ERROR)
        return;

    /* Handle POLLOUT for sending request */
    if ((revents & POLLOUT) && conn->state == HTTP_STATE_SENDING) {
        while (conn->send_pos < conn->send_size) {
            ssize_t n = conn->sock->write(conn->sock,
                                           conn->send_buf + conn->send_pos,
                                           conn->send_size - conn->send_pos);
            if (n > 0) {
                conn->send_pos += (size_t)n;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return; /* try again next poll */
            } else {
                conn_error(conn, rt, "write error");
                return;
            }
        }
        /* Request fully sent */
        free(conn->send_buf);
        conn->send_buf = NULL;
        conn->send_size = 0;
        conn->send_pos = 0;
        conn->state = HTTP_STATE_RECV_STATUS;
    }

    /* Handle POLLIN for receiving */
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        /* Read into buffer */
        size_t space = HTTP_READ_BUF_SIZE - conn->read_len;
        if (space > 0) {
            ssize_t n = conn->sock->read(conn->sock,
                                          conn->read_buf + conn->read_len,
                                          space);
            if (n > 0) {
                conn->read_len += (size_t)n;
            } else if (n == 0) {
                /* EOF */
                if (conn->state == HTTP_STATE_BODY_CONTENT &&
                    conn->content_length < 0) {
                    /* Reading until close */
                    conn->state = HTTP_STATE_DONE;
                    deliver_http_response(conn, rt);
                    return;
                } else if (conn->state == HTTP_STATE_BODY_STREAM) {
                    conn->state = HTTP_STATE_DONE;
                    deliver_sse_closed(conn, rt);
                    return;
                } else if (conn->state == HTTP_STATE_WS_ACTIVE) {
                    conn->state = HTTP_STATE_DONE;
                    deliver_ws_closed(conn, rt, 1006);
                    return;
                } else {
                    conn_error(conn, rt, "unexpected EOF");
                    return;
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                conn_error(conn, rt, "read error");
                return;
            }
        }

        /* Process buffered data */
        bool progress = true;
        while (progress && conn->read_len > 0) {
            progress = false;
            switch (conn->state) {
            case HTTP_STATE_RECV_STATUS:
                progress = parse_status_line(conn);
                break;
            case HTTP_STATE_RECV_HEADERS:
                progress = parse_header_line(conn, rt);
                break;
            case HTTP_STATE_BODY_CONTENT:
                progress = consume_body_content(conn, rt);
                break;
            case HTTP_STATE_BODY_CHUNKED:
                progress = consume_body_chunked(conn, rt);
                break;
            case HTTP_STATE_BODY_STREAM:
                progress = process_sse_data(conn, rt);
                break;
            case HTTP_STATE_WS_ACTIVE:
                progress = process_ws_data(conn, rt);
                break;
            default:
                progress = false;
                break;
            }
        }
    }
}

/* ── Connection allocation ─────────────────────────────────────────── */

static http_conn_t *alloc_conn(runtime_t *rt) {
    http_conn_t *conns = runtime_get_http_conns(rt);
    size_t max = runtime_get_max_http_conns();
    for (size_t i = 0; i < max; i++) {
        if (conns[i].id == HTTP_CONN_ID_INVALID) {
            memset(&conns[i], 0, sizeof(http_conn_t));
            conns[i].id = runtime_alloc_http_conn_id(rt);
            conns[i].owner = runtime_current_actor_id(rt);
            conns[i].content_length = -1;
            return &conns[i];
        }
    }
    return NULL;
}

/* ── Actor APIs ────────────────────────────────────────────────────── */

http_conn_id_t actor_http_fetch(runtime_t *rt, const char *method,
                                const char *url, const char *const *headers,
                                size_t n_headers, const void *body,
                                size_t body_size) {
    parsed_url_t parsed;
    if (!url_parse(url, &parsed)) return HTTP_CONN_ID_INVALID;

    if (url_is_tls(&parsed)) return HTTP_CONN_ID_INVALID; /* TLS not yet */

    uint16_t port = url_effective_port(&parsed);
    mk_socket_t *sock = mk_socket_tcp_connect(parsed.host, port);
    if (!sock) return HTTP_CONN_ID_INVALID;

    http_conn_t *conn = alloc_conn(rt);
    if (!conn) {
        sock->close(sock);
        return HTTP_CONN_ID_INVALID;
    }

    conn->sock = sock;
    conn->conn_type = HTTP_CONN_HTTP;

    /* Build request */
    conn->send_buf = build_http_request(method, &parsed, headers, n_headers,
                                        body, body_size, false,
                                        &conn->send_size);
    if (!conn->send_buf) {
        sock->close(sock);
        conn->id = HTTP_CONN_ID_INVALID;
        return HTTP_CONN_ID_INVALID;
    }
    conn->send_pos = 0;
    conn->state = HTTP_STATE_SENDING;

    return conn->id;
}

http_conn_id_t actor_http_get(runtime_t *rt, const char *url) {
    return actor_http_fetch(rt, "GET", url, NULL, 0, NULL, 0);
}

http_conn_id_t actor_sse_connect(runtime_t *rt, const char *url) {
    parsed_url_t parsed;
    if (!url_parse(url, &parsed)) return HTTP_CONN_ID_INVALID;

    if (url_is_tls(&parsed)) return HTTP_CONN_ID_INVALID;

    uint16_t port = url_effective_port(&parsed);
    mk_socket_t *sock = mk_socket_tcp_connect(parsed.host, port);
    if (!sock) return HTTP_CONN_ID_INVALID;

    http_conn_t *conn = alloc_conn(rt);
    if (!conn) {
        sock->close(sock);
        return HTTP_CONN_ID_INVALID;
    }

    conn->sock = sock;
    conn->conn_type = HTTP_CONN_SSE;

    conn->send_buf = build_http_request("GET", &parsed, NULL, 0,
                                        NULL, 0, true, &conn->send_size);
    if (!conn->send_buf) {
        sock->close(sock);
        conn->id = HTTP_CONN_ID_INVALID;
        return HTTP_CONN_ID_INVALID;
    }
    conn->send_pos = 0;
    conn->state = HTTP_STATE_SENDING;

    return conn->id;
}

http_conn_id_t actor_ws_connect(runtime_t *rt, const char *url) {
    parsed_url_t parsed;
    if (!url_parse(url, &parsed)) return HTTP_CONN_ID_INVALID;

    /* ws:// uses plain TCP, wss:// needs TLS (not yet supported) */
    if (strcmp(parsed.scheme, "wss") == 0) return HTTP_CONN_ID_INVALID;

    uint16_t port = url_effective_port(&parsed);
    mk_socket_t *sock = mk_socket_tcp_connect(parsed.host, port);
    if (!sock) return HTTP_CONN_ID_INVALID;

    http_conn_t *conn = alloc_conn(rt);
    if (!conn) {
        sock->close(sock);
        return HTTP_CONN_ID_INVALID;
    }

    conn->sock = sock;
    conn->conn_type = HTTP_CONN_WS;

    conn->send_buf = build_ws_handshake(&parsed, conn->ws_accept_key,
                                        &conn->send_size);
    if (!conn->send_buf) {
        sock->close(sock);
        conn->id = HTTP_CONN_ID_INVALID;
        return HTTP_CONN_ID_INVALID;
    }
    conn->send_pos = 0;
    conn->state = HTTP_STATE_SENDING;

    return conn->id;
}

static http_conn_t *find_conn(runtime_t *rt, http_conn_id_t id) {
    http_conn_t *conns = runtime_get_http_conns(rt);
    size_t max = runtime_get_max_http_conns();
    for (size_t i = 0; i < max; i++) {
        if (conns[i].id == id) return &conns[i];
    }
    return NULL;
}

bool actor_ws_send_text(runtime_t *rt, http_conn_id_t id,
                        const char *text, size_t len) {
    http_conn_t *conn = find_conn(rt, id);
    if (!conn || conn->state != HTTP_STATE_WS_ACTIVE) return false;
    return send_ws_frame(conn, WS_OPCODE_TEXT, (const uint8_t *)text, len);
}

bool actor_ws_send_binary(runtime_t *rt, http_conn_id_t id,
                          const void *data, size_t len) {
    http_conn_t *conn = find_conn(rt, id);
    if (!conn || conn->state != HTTP_STATE_WS_ACTIVE) return false;
    return send_ws_frame(conn, WS_OPCODE_BINARY, data, len);
}

bool actor_ws_close(runtime_t *rt, http_conn_id_t id,
                    uint16_t code, const char *reason) {
    http_conn_t *conn = find_conn(rt, id);
    if (!conn || conn->state != HTTP_STATE_WS_ACTIVE) return false;

    uint8_t frame[128];
    size_t reason_len = reason ? strlen(reason) : 0;
    size_t frame_size = ws_frame_build_close(code, reason, reason_len, frame);

    /* Blocking write of close frame */
    size_t written = 0;
    while (written < frame_size) {
        ssize_t n = conn->sock->write(conn->sock,
                                       frame + written, frame_size - written);
        if (n > 0) written += (size_t)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else break;
    }

    conn->state = HTTP_STATE_DONE;
    return true;
}

void actor_http_close(runtime_t *rt, http_conn_id_t id) {
    http_conn_t *conn = find_conn(rt, id);
    if (!conn) return;

    if (conn->sock) {
        conn->sock->close(conn->sock);
        conn->sock = NULL;
    }
    free(conn->send_buf);
    conn->send_buf = NULL;
    free(conn->headers_buf);
    conn->headers_buf = NULL;
    free(conn->body_buf);
    conn->body_buf = NULL;
    free(conn->sse_data);
    conn->sse_data = NULL;
    memset(conn, 0, sizeof(*conn));
}
