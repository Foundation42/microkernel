#ifndef MICROKERNEL_MK_SOCKET_H
#define MICROKERNEL_MK_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct mk_socket mk_socket_t;

struct mk_socket {
    ssize_t (*read)(mk_socket_t *self, uint8_t *buf, size_t len);
    ssize_t (*write)(mk_socket_t *self, const uint8_t *buf, size_t len);
    void    (*close)(mk_socket_t *self);
    int     (*get_fd)(mk_socket_t *self);
    void    *ctx;
};

/* Create a TCP socket connected to host:port.
   Blocks on connect, then sets non-blocking for I/O.
   Returns NULL on failure. Caller must call sock->close(sock). */
mk_socket_t *mk_socket_tcp_connect(const char *host, uint16_t port);

#endif /* MICROKERNEL_MK_SOCKET_H */
