#ifndef MICROKERNEL_MESSAGE_H
#define MICROKERNEL_MESSAGE_H

#include "types.h"

struct message {
    actor_id_t source;
    actor_id_t dest;
    msg_type_t type;
    size_t     payload_size;
    void      *payload;
    void     (*free_payload)(void *);
};

/* Create a message. Copies payload_size bytes from payload into a new
   allocation. If payload is NULL or payload_size is 0, the message
   carries no data. */
message_t *message_create(actor_id_t source, actor_id_t dest,
                          msg_type_t type, const void *payload,
                          size_t payload_size);

/* Destroy a message. Calls free_payload on payload if set, then frees
   the message struct itself. */
void message_destroy(message_t *msg);

#endif /* MICROKERNEL_MESSAGE_H */
