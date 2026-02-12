#ifndef MICROKERNEL_TRANSPORT_H
#define MICROKERNEL_TRANSPORT_H

#include "types.h"
#include "message.h"

typedef struct transport transport_t;

struct transport {
    node_id_t  peer_node;
    int        fd;              /* for poll(), -1 if N/A */
    bool     (*send)(transport_t *self, const message_t *msg);
    message_t *(*recv)(transport_t *self);
    bool     (*is_connected)(transport_t *self);
    void     (*destroy)(transport_t *self);
    void      *impl;            /* transport-specific state */
};

#endif /* MICROKERNEL_TRANSPORT_H */
