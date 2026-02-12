#define _POSIX_C_SOURCE 200112L
#include "microkernel/mk_socket.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* ── SSL_CTX singleton ─────────────────────────────────────────────── */

static SSL_CTX *g_ssl_ctx = NULL;
static pthread_once_t g_ssl_once = PTHREAD_ONCE_INIT;

static void init_ssl_ctx(void) {
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ssl_ctx) return;
    SSL_CTX_set_default_verify_paths(g_ssl_ctx);
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);
}

/* ── TLS socket context ────────────────────────────────────────────── */

typedef struct {
    int fd;
    SSL *ssl;
} tls_socket_ctx_t;

static ssize_t tls_read(mk_socket_t *self, uint8_t *buf, size_t len) {
    tls_socket_ctx_t *ctx = self->ctx;
    int n = SSL_read(ctx->ssl, buf, (int)(len > INT_MAX ? INT_MAX : len));
    if (n > 0) return n;

    int err = SSL_get_error(ctx->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) return 0; /* clean shutdown */
    errno = EIO;
    return -1;
}

static ssize_t tls_write(mk_socket_t *self, const uint8_t *buf, size_t len) {
    tls_socket_ctx_t *ctx = self->ctx;
    int n = SSL_write(ctx->ssl, buf, (int)(len > INT_MAX ? INT_MAX : len));
    if (n > 0) return n;

    int err = SSL_get_error(ctx->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    errno = EIO;
    return -1;
}

static void tls_close(mk_socket_t *self) {
    if (!self) return;
    tls_socket_ctx_t *ctx = self->ctx;
    if (ctx) {
        if (ctx->ssl) {
            SSL_shutdown(ctx->ssl);
            SSL_free(ctx->ssl);
        }
        if (ctx->fd >= 0) close(ctx->fd);
        free(ctx);
    }
    free(self);
}

static int tls_get_fd(mk_socket_t *self) {
    tls_socket_ctx_t *ctx = self->ctx;
    return ctx->fd;
}

/* ── Public API ────────────────────────────────────────────────────── */

mk_socket_t *mk_socket_tls_connect(const char *host, uint16_t port) {
    /* Initialize SSL_CTX once */
    pthread_once(&g_ssl_once, init_ssl_ctx);
    if (!g_ssl_ctx) return NULL;

    /* DNS + TCP connect (blocking) — same pattern as mk_socket_tcp_connect */
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

    /* SSL handshake (blocking) */
    SSL *ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        close(fd);
        return NULL;
    }

    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);  /* SNI */
    SSL_set1_host(ssl, host);             /* hostname verification */

    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    /* Set non-blocking after handshake */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Allocate socket + context */
    mk_socket_t *sock = calloc(1, sizeof(*sock));
    tls_socket_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!sock || !ctx) {
        free(sock);
        free(ctx);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    ctx->fd = fd;
    ctx->ssl = ssl;
    sock->read = tls_read;
    sock->write = tls_write;
    sock->close = tls_close;
    sock->get_fd = tls_get_fd;
    sock->ctx = ctx;

    return sock;
}
