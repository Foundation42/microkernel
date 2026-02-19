/*
 * mk_socket_tls_esp.c — TLS socket via ESP-IDF esp-tls (mbedTLS backend)
 *
 * Implements mk_socket_t vtable for HTTPS/WSS on ESP32.
 * Uses the bundled CA certificate store (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
 */

#include "microkernel/mk_socket.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>

typedef struct {
    esp_tls_t *tls;
    int fd;   /* cached raw TCP fd for poll() */
} tls_esp_ctx_t;

static ssize_t tls_esp_read(mk_socket_t *self, uint8_t *buf, size_t len) {
    tls_esp_ctx_t *ctx = self->ctx;
    ssize_t n = esp_tls_conn_read(ctx->tls, buf, len);
    if (n > 0) return n;
    if (n == 0) return 0; /* clean shutdown */
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    errno = EIO;
    return -1;
}

static ssize_t tls_esp_write(mk_socket_t *self, const uint8_t *buf, size_t len) {
    tls_esp_ctx_t *ctx = self->ctx;
    ssize_t n = esp_tls_conn_write(ctx->tls, buf, len);
    if (n > 0) return n;
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    errno = EIO;
    return -1;
}

static void tls_esp_close(mk_socket_t *self) {
    if (!self) return;
    tls_esp_ctx_t *ctx = self->ctx;
    if (ctx) {
        if (ctx->tls) {
            /* esp_tls_conn_destroy handles SSL shutdown + socket close */
            esp_tls_conn_destroy(ctx->tls);
        }
        free(ctx);
    }
    free(self);
}

static int tls_esp_get_fd(mk_socket_t *self) {
    tls_esp_ctx_t *ctx = self->ctx;
    return ctx->fd;
}

mk_socket_t *mk_socket_tls_connect(const char *host, uint16_t port) {
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return NULL;

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    int ret = esp_tls_conn_new_sync(host, (int)strlen(host), (int)port, &cfg, tls);
    if (ret != 1) {
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    /* Get the underlying fd for poll() */
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    /* Set non-blocking after handshake */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    mk_socket_t *sock = calloc(1, sizeof(*sock));
    tls_esp_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!sock || !ctx) {
        free(sock);
        free(ctx);
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    ctx->tls = tls;
    ctx->fd = fd;
    sock->read = tls_esp_read;
    sock->write = tls_esp_write;
    sock->close = tls_esp_close;
    sock->get_fd = tls_esp_get_fd;
    sock->ctx = ctx;

    return sock;
}
