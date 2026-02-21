#include "microkernel/namespace.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include <esp_mac.h>

static char s_node_name[32];
static int  s_initialized = 0;

const char *mk_node_identity(void) {
    if (s_initialized) return s_node_name;
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_node_name, sizeof(s_node_name),
             "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_initialized = 1;
    return s_node_name;
}

#else /* Linux / POSIX */
#include "sha1.h"
#include <stdlib.h>
#include <unistd.h>

static char s_node_name[32];
static int  s_initialized = 0;

const char *mk_node_identity(void) {
    if (s_initialized) return s_node_name;
    const char *env = getenv("MK_NODE_NAME");
    if (env && env[0]) {
        snprintf(s_node_name, sizeof(s_node_name), "%s", env);
        s_initialized = 1;
        return s_node_name;
    }
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        snprintf(s_node_name, sizeof(s_node_name), "linux-unknown");
        s_initialized = 1;
        return s_node_name;
    }
    hostname[sizeof(hostname) - 1] = '\0';
    uint8_t hash[20];
    sha1((const uint8_t *)hostname, strlen(hostname), hash);
    snprintf(s_node_name, sizeof(s_node_name),
             "linux-%02x%02x", hash[18], hash[19]);
    s_initialized = 1;
    return s_node_name;
}
#endif
