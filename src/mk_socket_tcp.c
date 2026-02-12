#include "microkernel/mk_socket.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
