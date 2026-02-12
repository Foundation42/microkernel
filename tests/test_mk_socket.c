#define _GNU_SOURCE
#include "test_framework.h"
#include "microkernel/mk_socket.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>

#define TEST_PORT 19879

/* Simple echo server: accept one connection, echo back, close */
static pid_t start_echo_server(void) {
    pid_t pid = fork();
    if (pid != 0) return pid;

    /* Child */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };
    bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(lfd, 1);

    int cfd = accept(lfd, NULL, NULL);
    if (cfd >= 0) {
        char buf[256];
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n > 0) send(cfd, buf, (size_t)n, 0);
        close(cfd);
    }
    close(lfd);
    _exit(0);
}

static int test_socket_connect(void) {
    pid_t server = start_echo_server();
    usleep(50000); /* 50ms for server to start */

    mk_socket_t *sock = mk_socket_tcp_connect("127.0.0.1", TEST_PORT);
    ASSERT_NOT_NULL(sock);
    ASSERT(sock->get_fd(sock) >= 0);

    sock->close(sock);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_socket_connect_fail(void) {
    /* Try to connect to a port nothing listens on */
    mk_socket_t *sock = mk_socket_tcp_connect("127.0.0.1", 19899);
    ASSERT_NULL(sock);
    return 0;
}

static int test_socket_write_read(void) {
    pid_t server = start_echo_server();
    usleep(50000);

    mk_socket_t *sock = mk_socket_tcp_connect("127.0.0.1", TEST_PORT);
    ASSERT_NOT_NULL(sock);

    const char *msg = "hello";
    ssize_t nw = sock->write(sock, (const uint8_t *)msg, 5);
    ASSERT(nw == 5);

    /* Give server time to echo */
    usleep(50000);

    uint8_t buf[64];
    ssize_t nr = sock->read(sock, buf, sizeof(buf));
    ASSERT(nr == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);

    sock->close(sock);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_socket_nonblocking_read(void) {
    pid_t server = start_echo_server();
    usleep(50000);

    mk_socket_t *sock = mk_socket_tcp_connect("127.0.0.1", TEST_PORT);
    ASSERT_NOT_NULL(sock);

    /* Read with no data available should return -1 with EAGAIN */
    uint8_t buf[64];
    ssize_t n = sock->read(sock, buf, sizeof(buf));
    ASSERT(n < 0);
    ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

    sock->close(sock);
    waitpid(server, NULL, 0);
    return 0;
}

static int test_socket_get_fd(void) {
    pid_t server = start_echo_server();
    usleep(50000);

    mk_socket_t *sock = mk_socket_tcp_connect("127.0.0.1", TEST_PORT);
    ASSERT_NOT_NULL(sock);

    int fd = sock->get_fd(sock);
    ASSERT(fd >= 0);

    /* Verify it's a valid non-blocking fd */
    int flags = fcntl(fd, F_GETFL, 0);
    ASSERT(flags >= 0);
    ASSERT(flags & O_NONBLOCK);

    sock->close(sock);
    waitpid(server, NULL, 0);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("test_mk_socket:\n");
    RUN_TEST(test_socket_connect);
    RUN_TEST(test_socket_connect_fail);
    RUN_TEST(test_socket_write_read);
    RUN_TEST(test_socket_nonblocking_read);
    RUN_TEST(test_socket_get_fd);
    TEST_REPORT();
}
