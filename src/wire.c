#include "microkernel/wire.h"
#include <stdlib.h>
#include <string.h>

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
