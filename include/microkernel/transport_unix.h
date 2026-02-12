#ifndef MICROKERNEL_TRANSPORT_UNIX_H
#define MICROKERNEL_TRANSPORT_UNIX_H

#include "transport.h"

/* Create a listening (server) transport on a Unix domain socket.
   Binds and listens immediately. Accept happens lazily on first recv/poll. */
transport_t *transport_unix_listen(const char *path, node_id_t peer_node);

/* Create a connecting (client) transport to a Unix domain socket.
   Connects immediately. */
transport_t *transport_unix_connect(const char *path, node_id_t peer_node);

#endif /* MICROKERNEL_TRANSPORT_UNIX_H */
