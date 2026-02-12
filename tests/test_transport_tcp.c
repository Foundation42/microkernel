#define _DEFAULT_SOURCE
#include "test_framework.h"
#include "microkernel/transport_tcp.h"
#include "microkernel/message.h"
#include <unistd.h>

#define TEST_PORT 19876

static int test_send_recv_simple(void) {
    transport_t *server = transport_tcp_listen("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_tcp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    /* Send from client to server */
    message_t *msg = message_create(0x200000001ULL, 0x100000001ULL, 42, NULL, 0);
    ASSERT_NOT_NULL(msg);

    bool sent = client->send(client, msg);
    ASSERT(sent);
    message_destroy(msg);

    usleep(1000);

    /* Server should receive it (triggers accept + recv) */
    message_t *recv_msg = server->recv(server);
    ASSERT_NOT_NULL(recv_msg);
    ASSERT_EQ(recv_msg->source, 0x200000001ULL);
    ASSERT_EQ(recv_msg->dest, 0x100000001ULL);
    ASSERT_EQ(recv_msg->type, (msg_type_t)42);
    ASSERT_EQ(recv_msg->payload_size, (size_t)0);
    message_destroy(recv_msg);

    client->destroy(client);
    server->destroy(server);
    return 0;
}

static int test_send_recv_with_payload(void) {
    transport_t *server = transport_tcp_listen("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_tcp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    message_t *msg = message_create(0x200000001ULL, 0x100000001ULL, 99,
                                    data, sizeof(data));
    ASSERT_NOT_NULL(msg);

    bool sent = client->send(client, msg);
    ASSERT(sent);
    message_destroy(msg);

    usleep(1000);

    message_t *recv_msg = server->recv(server);
    ASSERT_NOT_NULL(recv_msg);
    ASSERT_EQ(recv_msg->type, (msg_type_t)99);
    ASSERT_EQ(recv_msg->payload_size, sizeof(data));
    ASSERT(memcmp(recv_msg->payload, data, sizeof(data)) == 0);
    message_destroy(recv_msg);

    client->destroy(client);
    server->destroy(server);
    return 0;
}

static int test_fifo_order(void) {
    transport_t *server = transport_tcp_listen("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_tcp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    /* Send 3 messages with different types */
    for (msg_type_t t = 1; t <= 3; t++) {
        message_t *msg = message_create(0x200000001ULL, 0x100000001ULL,
                                        t, &t, sizeof(t));
        ASSERT_NOT_NULL(msg);
        ASSERT(client->send(client, msg));
        message_destroy(msg);
    }

    usleep(1000);

    /* Receive in FIFO order */
    for (msg_type_t t = 1; t <= 3; t++) {
        message_t *recv_msg = server->recv(server);
        ASSERT_NOT_NULL(recv_msg);
        ASSERT_EQ(recv_msg->type, t);
        message_destroy(recv_msg);
    }

    client->destroy(client);
    server->destroy(server);
    return 0;
}

static int test_nonblocking_recv_empty(void) {
    transport_t *server = transport_tcp_listen("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_tcp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    /* Trigger accept by sending+receiving one message first */
    message_t *msg = message_create(0x200000001ULL, 0x100000001ULL, 1, NULL, 0);
    ASSERT_NOT_NULL(msg);
    ASSERT(client->send(client, msg));
    message_destroy(msg);
    usleep(1000);

    message_t *first = server->recv(server);
    ASSERT_NOT_NULL(first);
    message_destroy(first);

    /* Now recv on empty should return NULL, not block */
    message_t *empty = server->recv(server);
    ASSERT_NULL(empty);

    client->destroy(client);
    server->destroy(server);
    return 0;
}

static int test_destroy_cleanup(void) {
    transport_t *server = transport_tcp_listen("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_tcp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    /* Destroy should not crash and should close fds */
    client->destroy(client);
    server->destroy(server);
    return 0;
}

int main(void) {
    printf("test_transport_tcp:\n");
    RUN_TEST(test_send_recv_simple);
    RUN_TEST(test_send_recv_with_payload);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_nonblocking_recv_empty);
    RUN_TEST(test_destroy_cleanup);
    TEST_REPORT();
}
