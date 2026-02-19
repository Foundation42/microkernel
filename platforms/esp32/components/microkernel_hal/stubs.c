/*
 * Linker stubs for functions referenced by runtime.c but not needed on ESP32.
 * HTTP connections are never created, so http_conn_drive is never called.
 */
#include "runtime_internal.h"

void http_conn_drive(http_conn_t *conn, short revents, runtime_t *rt) {
    (void)conn;
    (void)revents;
    (void)rt;
}
