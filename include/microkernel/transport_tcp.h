#ifndef MICROKERNEL_TRANSPORT_TCP_H
#define MICROKERNEL_TRANSPORT_TCP_H

#include "transport.h"

/* Create a listening (server) TCP transport on host:port.
   Binds and listens immediately. Accept happens lazily on first recv/send. */
transport_t *transport_tcp_listen(const char *host, uint16_t port,
                                  node_id_t peer_node);

/* Create a connecting (client) TCP transport to host:port.
   Connects immediately. */
transport_t *transport_tcp_connect(const char *host, uint16_t port,
                                   node_id_t peer_node);

/* Wrap an already-connected TCP fd as a transport. */
transport_t *transport_tcp_from_fd(int fd, node_id_t peer_node);

#endif /* MICROKERNEL_TRANSPORT_TCP_H */
