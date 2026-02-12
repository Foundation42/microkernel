#include "microkernel/transport_udp.h"
#include "microkernel/wire.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define UDP_MAX_DGRAM 65507

typedef struct {
    int                sock_fd;
    struct sockaddr_in peer_addr;
    bool               has_peer;
    uint8_t            recv_buf[UDP_MAX_DGRAM];
} udp_impl_t;

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ── vtable implementations ────────────────────────────────────────── */

static bool udp_send(transport_t *self, const message_t *msg) {
    udp_impl_t *impl = self->impl;

    size_t wire_size;
    void *buf = wire_serialize_net(msg, &wire_size);
    if (!buf) return false;

    if (wire_size > UDP_MAX_DGRAM) {
        free(buf);
        return false;
    }

    ssize_t n = send(impl->sock_fd, buf, wire_size, MSG_NOSIGNAL);
    free(buf);
    return n == (ssize_t)wire_size;
}

static message_t *udp_recv(transport_t *self) {
    udp_impl_t *impl = self->impl;

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t n = recvfrom(impl->sock_fd, impl->recv_buf, UDP_MAX_DGRAM,
                         MSG_DONTWAIT, (struct sockaddr *)&from_addr, &from_len);
    if (n <= 0) return NULL;

    /* On first recv from bind side: lock in the peer */
    if (!impl->has_peer) {
        impl->peer_addr = from_addr;
        impl->has_peer = true;
        /* connect() to filter incoming and enable send() */
        connect(impl->sock_fd, (struct sockaddr *)&from_addr, sizeof(from_addr));
    }

    return wire_deserialize_net(impl->recv_buf, (size_t)n);
}

static bool udp_is_connected(transport_t *self) {
    udp_impl_t *impl = self->impl;
    return impl->has_peer;
}

static void udp_destroy(transport_t *self) {
    if (!self) return;
    udp_impl_t *impl = self->impl;
    if (impl) {
        if (impl->sock_fd >= 0) close(impl->sock_fd);
        free(impl);
    }
    free(self);
}

/* ── Constructors ──────────────────────────────────────────────────── */

transport_t *transport_udp_bind(const char *host, uint16_t port,
                                node_id_t peer_node) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    set_nonblocking(fd);

    transport_t *tp = calloc(1, sizeof(*tp));
    udp_impl_t *impl = calloc(1, sizeof(*impl));
    if (!tp || !impl) {
        free(tp);
        free(impl);
        close(fd);
        return NULL;
    }

    impl->sock_fd = fd;
    impl->has_peer = false;

    tp->peer_node = peer_node;
    tp->fd = fd;
    tp->send = udp_send;
    tp->recv = udp_recv;
    tp->is_connected = udp_is_connected;
    tp->destroy = udp_destroy;
    tp->impl = impl;

    return tp;
}

transport_t *transport_udp_connect(const char *host, uint16_t port,
                                    node_id_t peer_node) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return NULL;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    set_nonblocking(fd);

    transport_t *tp = calloc(1, sizeof(*tp));
    udp_impl_t *impl = calloc(1, sizeof(*impl));
    if (!tp || !impl) {
        free(tp);
        free(impl);
        close(fd);
        return NULL;
    }

    impl->sock_fd = fd;
    impl->has_peer = true;
    impl->peer_addr = addr;

    tp->peer_node = peer_node;
    tp->fd = fd;
    tp->send = udp_send;
    tp->recv = udp_recv;
    tp->is_connected = udp_is_connected;
    tp->destroy = udp_destroy;
    tp->impl = impl;

    return tp;
}
