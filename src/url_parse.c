#include "url_parse.h"
#include <string.h>
#include <stdlib.h>

bool url_parse(const char *url, parsed_url_t *out) {
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));

    /* Parse scheme */
    const char *sep = strstr(url, "://");
    if (!sep) return false;

    size_t scheme_len = (size_t)(sep - url);
    if (scheme_len == 0 || scheme_len >= sizeof(out->scheme)) return false;
    memcpy(out->scheme, url, scheme_len);
    out->scheme[scheme_len] = '\0';

    /* Validate scheme */
    if (strcmp(out->scheme, "http") != 0 &&
        strcmp(out->scheme, "https") != 0 &&
        strcmp(out->scheme, "ws") != 0 &&
        strcmp(out->scheme, "wss") != 0) {
        return false;
    }

    const char *p = sep + 3; /* skip "://" */

    /* Find where host[:port] ends */
    const char *path_start = strchr(p, '/');
    const char *host_end = path_start ? path_start : p + strlen(p);

    /* Look for port separator */
    const char *colon = NULL;
    for (const char *c = p; c < host_end; c++) {
        if (*c == ':') { colon = c; break; }
    }

    if (colon) {
        size_t host_len = (size_t)(colon - p);
        if (host_len == 0 || host_len >= sizeof(out->host)) return false;
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';

        char *endptr;
        long port = strtol(colon + 1, &endptr, 10);
        if (endptr == colon + 1 || port <= 0 || port > 65535) return false;
        if (endptr != host_end) return false;
        out->port = (uint16_t)port;
    } else {
        size_t host_len = (size_t)(host_end - p);
        if (host_len == 0 || host_len >= sizeof(out->host)) return false;
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';
    }

    /* Parse path */
    if (path_start && *path_start) {
        size_t path_len = strlen(path_start);
        if (path_len >= sizeof(out->path)) return false;
        memcpy(out->path, path_start, path_len);
        out->path[path_len] = '\0';
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    return true;
}

uint16_t url_effective_port(const parsed_url_t *url) {
    if (url->port != 0) return url->port;
    if (strcmp(url->scheme, "https") == 0 || strcmp(url->scheme, "wss") == 0)
        return 443;
    return 80;
}

bool url_is_tls(const parsed_url_t *url) {
    return strcmp(url->scheme, "https") == 0 ||
           strcmp(url->scheme, "wss") == 0;
}
