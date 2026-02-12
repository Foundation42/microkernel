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

/* Wrap an already-connected, already-nonblocking fd into mk_socket_t.
   Used by the server accept path. Returns NULL on alloc failure. */
mk_socket_t *mk_socket_tcp_wrap(int fd);

#ifdef HAVE_OPENSSL
/* Create a TLS socket connected to host:port.
   Blocks on connect + TLS handshake, then sets non-blocking for I/O.
   Verifies peer certificate against system CA store.
   Returns NULL on failure. Caller must call sock->close(sock). */
mk_socket_t *mk_socket_tls_connect(const char *host, uint16_t port);
#endif

#endif /* MICROKERNEL_MK_SOCKET_H */
