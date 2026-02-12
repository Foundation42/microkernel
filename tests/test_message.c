#include "test_framework.h"
#include "microkernel/message.h"

static int test_create_with_payload(void) {
    uint32_t data = 42;
    message_t *msg = message_create(1, 2, 100, &data, sizeof(data));
    ASSERT_NOT_NULL(msg);
    ASSERT_EQ(msg->source, (actor_id_t)1);
    ASSERT_EQ(msg->dest, (actor_id_t)2);
    ASSERT_EQ(msg->type, (msg_type_t)100);
    ASSERT_EQ(msg->payload_size, sizeof(data));
    ASSERT_NOT_NULL(msg->payload);
    ASSERT_EQ(*(uint32_t *)msg->payload, 42u);
    /* Payload is a copy, not the original */
    ASSERT(msg->payload != &data);
    message_destroy(msg);
    return 0;
}

static int test_create_without_payload(void) {
    message_t *msg = message_create(1, 2, 100, NULL, 0);
    ASSERT_NOT_NULL(msg);
    ASSERT_NULL(msg->payload);
    ASSERT_EQ(msg->payload_size, (size_t)0);
    ASSERT_NULL(msg->free_payload);
    message_destroy(msg);
    return 0;
}

static int test_destroy_null(void) {
    message_destroy(NULL); /* should not crash */
    return 0;
}

static int custom_free_called = 0;
static void custom_free(void *p) {
    custom_free_called = 1;
    free(p);
}

static int test_custom_free(void) {
    uint8_t data = 7;
    message_t *msg = message_create(1, 2, 0, &data, sizeof(data));
    ASSERT_NOT_NULL(msg);
    /* Override the default free with our custom one */
    msg->free_payload = custom_free;
    custom_free_called = 0;
    message_destroy(msg);
    ASSERT_EQ(custom_free_called, 1);
    return 0;
}

int main(void) {
    printf("test_message:\n");
    RUN_TEST(test_create_with_payload);
    RUN_TEST(test_create_without_payload);
    RUN_TEST(test_destroy_null);
    RUN_TEST(test_custom_free);
    TEST_REPORT();
}
