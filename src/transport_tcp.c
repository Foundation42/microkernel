#include "microkernel/transport_tcp.h"
#include "microkernel/wire.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

typedef struct {
    int      listen_fd;     /* -1 for client */
    int      conn_fd;       /* connected socket, -1 until accept/connect */
    bool     is_server;
    uint8_t *read_buf;      /* partial read accumulation */
    size_t   read_pos;      /* bytes read so far */
    size_t   read_target;   /* bytes needed (WIRE_HEADER_SIZE, then header+payload) */
} tcp_impl_t;

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ── vtable implementations ────────────────────────────────────────── */

static bool try_accept(transport_t *self) {
    tcp_impl_t *impl = self->impl;
    if (impl->conn_fd >= 0) return true;
    if (impl->listen_fd < 0) return false;

    int fd = accept(impl->listen_fd, NULL, NULL);
    if (fd < 0) return false;

    set_nonblocking(fd);
    impl->conn_fd = fd;
    self->fd = fd;  /* update poll fd to connected socket */
    return true;
}

static bool tcp_send(transport_t *self, const message_t *msg) {
    tcp_impl_t *impl = self->impl;

    if (impl->is_server && impl->conn_fd < 0) {
        if (!try_accept(self)) return false;
    }
    if (impl->conn_fd < 0) return false;

    size_t wire_size;
    void *buf = wire_serialize_net(msg, &wire_size);
    if (!buf) return false;

    /* Write all bytes — loop on partial writes */
    size_t written = 0;
    while (written < wire_size) {
        ssize_t n = send(impl->conn_fd, (uint8_t *)buf + written,
                         wire_size - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return false;
        }
        written += (size_t)n;
    }

    free(buf);
    return true;
}

static message_t *tcp_recv(transport_t *self) {
    tcp_impl_t *impl = self->impl;

    if (impl->is_server && impl->conn_fd < 0) {
        if (!try_accept(self)) return NULL;
    }
    if (impl->conn_fd < 0) return NULL;

    /* Allocate read buffer on first call */
    if (!impl->read_buf) {
        impl->read_buf = malloc(WIRE_HEADER_SIZE);
        if (!impl->read_buf) return NULL;
        impl->read_pos = 0;
        impl->read_target = WIRE_HEADER_SIZE;
    }

    /* Read loop: try to fill current target */
    while (impl->read_pos < impl->read_target) {
        ssize_t n = recv(impl->conn_fd, impl->read_buf + impl->read_pos,
                         impl->read_target - impl->read_pos, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return NULL;
            if (errno == EINTR) continue;
            return NULL;  /* error */
        }
        if (n == 0) return NULL;  /* EOF */
        impl->read_pos += (size_t)n;
    }

    /* Have we just finished reading the header? */
    if (impl->read_target == WIRE_HEADER_SIZE) {
        const wire_header_t *hdr = (const wire_header_t *)impl->read_buf;
        uint32_t psz = ntohl(hdr->payload_size);

        if (psz == 0) {
            /* No payload — deserialize immediately */
            message_t *msg = wire_deserialize_net(impl->read_buf, WIRE_HEADER_SIZE);
            impl->read_pos = 0;
            impl->read_target = WIRE_HEADER_SIZE;
            return msg;
        }

        /* Grow buffer and continue reading payload */
        size_t total = WIRE_HEADER_SIZE + psz;
        uint8_t *new_buf = realloc(impl->read_buf, total);
        if (!new_buf) return NULL;
        impl->read_buf = new_buf;
        impl->read_target = total;

        /* Try to read payload bytes now (non-blocking) */
        while (impl->read_pos < impl->read_target) {
            ssize_t n = recv(impl->conn_fd, impl->read_buf + impl->read_pos,
                             impl->read_target - impl->read_pos, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return NULL;
                if (errno == EINTR) continue;
                return NULL;
            }
            if (n == 0) return NULL;
            impl->read_pos += (size_t)n;
        }
    }

    /* Full message accumulated — deserialize and reset */
    message_t *msg = wire_deserialize_net(impl->read_buf, impl->read_target);

    /* Reset for next message (shrink buffer back to header size) */
    uint8_t *reset_buf = realloc(impl->read_buf, WIRE_HEADER_SIZE);
    if (reset_buf) impl->read_buf = reset_buf;
    impl->read_pos = 0;
    impl->read_target = WIRE_HEADER_SIZE;

    return msg;
}

static bool tcp_is_connected(transport_t *self) {
    tcp_impl_t *impl = self->impl;
    return impl->conn_fd >= 0;
}

static void tcp_destroy(transport_t *self) {
    if (!self) return;
    tcp_impl_t *impl = self->impl;
    if (impl) {
        if (impl->conn_fd >= 0) close(impl->conn_fd);
        if (impl->listen_fd >= 0) close(impl->listen_fd);
        free(impl->read_buf);
        free(impl);
    }
    free(self);
}

/* ── Constructors ──────────────────────────────────────────────────── */

transport_t *transport_tcp_listen(const char *host, uint16_t port,
                                  node_id_t peer_node) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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

    if (listen(fd, 1) < 0) {
        close(fd);
        return NULL;
    }

    set_nonblocking(fd);

    transport_t *tp = calloc(1, sizeof(*tp));
    tcp_impl_t *impl = calloc(1, sizeof(*impl));
    if (!tp || !impl) {
        free(tp);
        free(impl);
        close(fd);
        return NULL;
    }

    impl->listen_fd = fd;
    impl->conn_fd = -1;
    impl->is_server = true;
    impl->read_target = WIRE_HEADER_SIZE;

    tp->peer_node = peer_node;
    tp->fd = fd;  /* listen fd for poll until accept */
    tp->send = tcp_send;
    tp->recv = tcp_recv;
    tp->is_connected = tcp_is_connected;
    tp->destroy = tcp_destroy;
    tp->impl = impl;

    return tp;
}

transport_t *transport_tcp_from_fd(int fd, node_id_t peer_node) {
    set_nonblocking(fd);

    transport_t *tp = calloc(1, sizeof(*tp));
    tcp_impl_t *impl = calloc(1, sizeof(*impl));
    if (!tp || !impl) {
        free(tp); free(impl);
        return NULL;
    }

    impl->listen_fd = -1;
    impl->conn_fd = fd;
    impl->is_server = false;
    impl->read_target = WIRE_HEADER_SIZE;

    tp->peer_node = peer_node;
    tp->fd = fd;
    tp->send = tcp_send;
    tp->recv = tcp_recv;
    tp->is_connected = tcp_is_connected;
    tp->destroy = tcp_destroy;
    tp->impl = impl;

    return tp;
}

transport_t *transport_tcp_connect(const char *host, uint16_t port,
                                   node_id_t peer_node) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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
    tcp_impl_t *impl = calloc(1, sizeof(*impl));
    if (!tp || !impl) {
        free(tp);
        free(impl);
        close(fd);
        return NULL;
    }

    impl->listen_fd = -1;
    impl->conn_fd = fd;
    impl->is_server = false;
    impl->read_target = WIRE_HEADER_SIZE;

    tp->peer_node = peer_node;
    tp->fd = fd;
    tp->send = tcp_send;
    tp->recv = tcp_recv;
    tp->is_connected = tcp_is_connected;
    tp->destroy = tcp_destroy;
    tp->impl = impl;

    return tp;
}
