#define _POSIX_C_SOURCE 200112L
#include "microkernel/mk_socket.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef struct {
    int fd;
} tcp_socket_ctx_t;

static ssize_t tcp_read(mk_socket_t *self, uint8_t *buf, size_t len) {
    tcp_socket_ctx_t *ctx = self->ctx;
    return recv(ctx->fd, buf, len, MSG_DONTWAIT);
}

static ssize_t tcp_write(mk_socket_t *self, const uint8_t *buf, size_t len) {
    tcp_socket_ctx_t *ctx = self->ctx;
    return send(ctx->fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void tcp_close(mk_socket_t *self) {
    if (!self) return;
    tcp_socket_ctx_t *ctx = self->ctx;
    if (ctx) {
        if (ctx->fd >= 0) close(ctx->fd);
        free(ctx);
    }
    free(self);
}

static int tcp_get_fd(mk_socket_t *self) {
    tcp_socket_ctx_t *ctx = self->ctx;
    return ctx->fd;
}

mk_socket_t *mk_socket_tcp_connect(const char *host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return NULL;
    }

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0) {
        close(fd);
        return NULL;
    }

    /* Set non-blocking after connect */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    mk_socket_t *sock = calloc(1, sizeof(*sock));
    tcp_socket_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!sock || !ctx) {
        free(sock);
        free(ctx);
        close(fd);
        return NULL;
    }

    ctx->fd = fd;
    sock->read = tcp_read;
    sock->write = tcp_write;
    sock->close = tcp_close;
    sock->get_fd = tcp_get_fd;
    sock->ctx = ctx;

    return sock;
}

mk_socket_t *mk_socket_tcp_wrap(int fd) {
    mk_socket_t *sock = calloc(1, sizeof(*sock));
    tcp_socket_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!sock || !ctx) {
        free(sock);
        free(ctx);
        return NULL;
    }

    ctx->fd = fd;
    sock->read = tcp_read;
    sock->write = tcp_write;
    sock->close = tcp_close;
    sock->get_fd = tcp_get_fd;
    sock->ctx = ctx;

    return sock;
}
