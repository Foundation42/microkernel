#ifndef MICROKERNEL_TRANSPORT_UDP_H
#define MICROKERNEL_TRANSPORT_UDP_H

#include "transport.h"

/* Create a bound (server-side) UDP transport on host:port.
   Binds immediately. Learns peer from first recvfrom, then connect()s to lock in. */
transport_t *transport_udp_bind(const char *host, uint16_t port,
                                node_id_t peer_node);

/* Create a connected (client-side) UDP transport to host:port.
   Connects immediately (sets default peer + filters incoming). */
transport_t *transport_udp_connect(const char *host, uint16_t port,
                                    node_id_t peer_node);

#endif /* MICROKERNEL_TRANSPORT_UDP_H */
