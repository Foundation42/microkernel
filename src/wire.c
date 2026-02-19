#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "microkernel/wire.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/* Portable 64-bit byte swap using htonl() — a no-op where <endian.h> macros exist */
#ifndef htobe64
static inline uint64_t htobe64(uint64_t x) {
    uint32_t hi = htonl((uint32_t)(x >> 32));
    uint32_t lo = htonl((uint32_t)(x & 0xFFFFFFFF));
    return ((uint64_t)lo << 32) | hi;
}
#endif
#ifndef be64toh
static inline uint64_t be64toh(uint64_t x) {
    return htobe64(x); /* self-inverse */
}
#endif

void *wire_serialize(const message_t *msg, size_t *out_size) {
    if (!msg || !out_size) return NULL;

    uint32_t psz = (uint32_t)msg->payload_size;
    size_t total = WIRE_HEADER_SIZE + psz;

    uint8_t *buf = malloc(total);
    if (!buf) return NULL;

    wire_header_t *hdr = (wire_header_t *)buf;
    hdr->source       = msg->source;
    hdr->dest         = msg->dest;
    hdr->type         = msg->type;
    hdr->payload_size = psz;
    hdr->reserved     = 0;

    if (psz > 0 && msg->payload) {
        memcpy(buf + WIRE_HEADER_SIZE, msg->payload, psz);
    }

    *out_size = total;
    return buf;
}

message_t *wire_deserialize(const void *buf, size_t buf_size) {
    if (!buf || buf_size < WIRE_HEADER_SIZE) return NULL;

    const wire_header_t *hdr = (const wire_header_t *)buf;

    if (buf_size < WIRE_HEADER_SIZE + hdr->payload_size) return NULL;

    const void *payload = NULL;
    if (hdr->payload_size > 0) {
        payload = (const uint8_t *)buf + WIRE_HEADER_SIZE;
    }

    return message_create(hdr->source, hdr->dest, hdr->type,
                          payload, hdr->payload_size);
}

/* ── Network byte order variants ──────────────────────────────────── */

void *wire_serialize_net(const message_t *msg, size_t *out_size) {
    if (!msg || !out_size) return NULL;

    uint32_t psz = (uint32_t)msg->payload_size;
    size_t total = WIRE_HEADER_SIZE + psz;

    uint8_t *buf = malloc(total);
    if (!buf) return NULL;

    wire_header_t *hdr = (wire_header_t *)buf;
    hdr->source       = htobe64(msg->source);
    hdr->dest         = htobe64(msg->dest);
    hdr->type         = htonl(msg->type);
    hdr->payload_size = htonl(psz);
    hdr->reserved     = 0;

    if (psz > 0 && msg->payload) {
        memcpy(buf + WIRE_HEADER_SIZE, msg->payload, psz);
    }

    *out_size = total;
    return buf;
}

message_t *wire_deserialize_net(const void *buf, size_t buf_size) {
    if (!buf || buf_size < WIRE_HEADER_SIZE) return NULL;

    const wire_header_t *hdr = (const wire_header_t *)buf;

    actor_id_t source = be64toh(hdr->source);
    actor_id_t dest   = be64toh(hdr->dest);
    msg_type_t type   = ntohl(hdr->type);
    uint32_t   psz    = ntohl(hdr->payload_size);

    if (buf_size < WIRE_HEADER_SIZE + psz) return NULL;

    const void *payload = NULL;
    if (psz > 0) {
        payload = (const uint8_t *)buf + WIRE_HEADER_SIZE;
    }

    return message_create(source, dest, type, payload, psz);
}
