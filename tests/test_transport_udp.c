#define _DEFAULT_SOURCE
#include "test_framework.h"
#include "microkernel/transport_udp.h"
#include "microkernel/wire.h"
#include "microkernel/message.h"
#include <unistd.h>

#define TEST_PORT 19878

static int test_send_recv_simple(void) {
    transport_t *server = transport_udp_bind("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_udp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    /* Send from client to server */
    message_t *msg = message_create(0x200000001ULL, 0x100000001ULL, 42, NULL, 0);
    ASSERT_NOT_NULL(msg);

    bool sent = client->send(client, msg);
    ASSERT(sent);
    message_destroy(msg);

    usleep(1000);

    /* Server should receive it (triggers peer lock-in) */
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
    transport_t *server = transport_udp_bind("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_udp_connect("127.0.0.1", TEST_PORT, 1);
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

static int test_nonblocking_recv_empty(void) {
    transport_t *server = transport_udp_bind("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    /* Recv on empty should return NULL, not block */
    message_t *empty = server->recv(server);
    ASSERT_NULL(empty);

    server->destroy(server);
    return 0;
}

static int test_oversized_rejected(void) {
    transport_t *server = transport_udp_bind("127.0.0.1", TEST_PORT, 2);
    ASSERT_NOT_NULL(server);

    transport_t *client = transport_udp_connect("127.0.0.1", TEST_PORT, 1);
    ASSERT_NOT_NULL(client);

    /* Create a message with payload that exceeds UDP_MAX_DGRAM when serialized */
    size_t big_size = 65507;  /* max dgram */
    uint8_t *big_data = calloc(1, big_size);
    ASSERT_NOT_NULL(big_data);

    message_t *msg = message_create(1, 2, 3, big_data, big_size);
    ASSERT_NOT_NULL(msg);

    /* total wire size = 28 (header) + 65507 = 65535 > UDP_MAX_DGRAM → rejected */
    bool sent = client->send(client, msg);
    ASSERT(!sent);

    message_destroy(msg);
    free(big_data);
    client->destroy(client);
    server->destroy(server);
    return 0;
}

int main(void) {
    printf("test_transport_udp:\n");
    RUN_TEST(test_send_recv_simple);
    RUN_TEST(test_send_recv_with_payload);
    RUN_TEST(test_nonblocking_recv_empty);
    RUN_TEST(test_oversized_rejected);
    TEST_REPORT();
}
