#ifndef URL_PARSE_H
#define URL_PARSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char     scheme[8];     /* "http", "https", "ws", "wss" */
    char     host[256];
    uint16_t port;          /* 0 = use default for scheme */
    char     path[1024];    /* starts with '/', defaults to "/" */
} parsed_url_t;

/* Parse a URL into components. Returns true on success. */
bool url_parse(const char *url, parsed_url_t *out);

/* Get the effective port (applying scheme defaults). */
uint16_t url_effective_port(const parsed_url_t *url);

/* Returns true if the scheme implies TLS. */
bool url_is_tls(const parsed_url_t *url);

#endif /* URL_PARSE_H */
