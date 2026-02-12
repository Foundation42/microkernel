#include "test_framework.h"
#include "url_parse.h"

static int test_parse_http_with_port(void) {
    parsed_url_t u;
    ASSERT(url_parse("http://127.0.0.1:8080/api/v1", &u));
    ASSERT(strcmp(u.scheme, "http") == 0);
    ASSERT(strcmp(u.host, "127.0.0.1") == 0);
    ASSERT_EQ(u.port, 8080);
    ASSERT(strcmp(u.path, "/api/v1") == 0);
    ASSERT_EQ(url_effective_port(&u), 8080);
    ASSERT(!url_is_tls(&u));
    return 0;
}

static int test_parse_http_default_port(void) {
    parsed_url_t u;
    ASSERT(url_parse("http://example.com/path", &u));
    ASSERT(strcmp(u.scheme, "http") == 0);
    ASSERT(strcmp(u.host, "example.com") == 0);
    ASSERT_EQ(u.port, 0);
    ASSERT(strcmp(u.path, "/path") == 0);
    ASSERT_EQ(url_effective_port(&u), 80);
    return 0;
}

static int test_parse_https(void) {
    parsed_url_t u;
    ASSERT(url_parse("https://secure.example.com/login", &u));
    ASSERT(strcmp(u.scheme, "https") == 0);
    ASSERT(strcmp(u.host, "secure.example.com") == 0);
    ASSERT_EQ(url_effective_port(&u), 443);
    ASSERT(url_is_tls(&u));
    return 0;
}

static int test_parse_ws(void) {
    parsed_url_t u;
    ASSERT(url_parse("ws://localhost:9000/chat", &u));
    ASSERT(strcmp(u.scheme, "ws") == 0);
    ASSERT(strcmp(u.host, "localhost") == 0);
    ASSERT_EQ(u.port, 9000);
    ASSERT(strcmp(u.path, "/chat") == 0);
    ASSERT(!url_is_tls(&u));
    return 0;
}

static int test_parse_wss(void) {
    parsed_url_t u;
    ASSERT(url_parse("wss://ws.example.com/stream", &u));
    ASSERT(strcmp(u.scheme, "wss") == 0);
    ASSERT(url_is_tls(&u));
    ASSERT_EQ(url_effective_port(&u), 443);
    return 0;
}

static int test_parse_no_path(void) {
    parsed_url_t u;
    ASSERT(url_parse("http://example.com", &u));
    ASSERT(strcmp(u.path, "/") == 0);
    return 0;
}

static int test_parse_root_path(void) {
    parsed_url_t u;
    ASSERT(url_parse("http://example.com/", &u));
    ASSERT(strcmp(u.path, "/") == 0);
    return 0;
}

static int test_parse_invalid_no_scheme(void) {
    parsed_url_t u;
    ASSERT(!url_parse("example.com/path", &u));
    return 0;
}

static int test_parse_invalid_unknown_scheme(void) {
    parsed_url_t u;
    ASSERT(!url_parse("ftp://files.example.com", &u));
    return 0;
}

static int test_parse_invalid_no_host(void) {
    parsed_url_t u;
    ASSERT(!url_parse("http:///path", &u));
    return 0;
}

static int test_parse_null(void) {
    parsed_url_t u;
    ASSERT(!url_parse(NULL, &u));
    ASSERT(!url_parse("http://example.com", NULL));
    return 0;
}

int main(void) {
    printf("test_url_parse:\n");
    RUN_TEST(test_parse_http_with_port);
    RUN_TEST(test_parse_http_default_port);
    RUN_TEST(test_parse_https);
    RUN_TEST(test_parse_ws);
    RUN_TEST(test_parse_wss);
    RUN_TEST(test_parse_no_path);
    RUN_TEST(test_parse_root_path);
    RUN_TEST(test_parse_invalid_no_scheme);
    RUN_TEST(test_parse_invalid_unknown_scheme);
    RUN_TEST(test_parse_invalid_no_host);
    RUN_TEST(test_parse_null);
    TEST_REPORT();
}
