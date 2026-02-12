#include "microkernel/message.h"
#include <stdlib.h>
#include <string.h>

message_t *message_create(actor_id_t source, actor_id_t dest,
                          msg_type_t type, const void *payload,
                          size_t payload_size) {
    message_t *msg = calloc(1, sizeof(*msg));
    if (!msg) return NULL;

    msg->source = source;
    msg->dest = dest;
    msg->type = type;

    if (payload && payload_size > 0) {
        msg->payload = malloc(payload_size);
        if (!msg->payload) {
            free(msg);
            return NULL;
        }
        memcpy(msg->payload, payload, payload_size);
        msg->payload_size = payload_size;
        msg->free_payload = free;
    }

    return msg;
}

void message_destroy(message_t *msg) {
    if (!msg) return;
    if (msg->free_payload && msg->payload) {
        msg->free_payload(msg->payload);
    }
    free(msg);
}
