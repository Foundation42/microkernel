#include "test_framework.h"
#include "microkernel/mailbox.h"
#include "microkernel/message.h"

static int test_create_destroy(void) {
    mailbox_t *mb = mailbox_create(4);
    ASSERT_NOT_NULL(mb);
    ASSERT(mailbox_is_empty(mb));
    ASSERT_EQ(mailbox_count(mb), (size_t)0);
    mailbox_destroy(mb);
    return 0;
}

static int test_enqueue_dequeue(void) {
    mailbox_t *mb = mailbox_create(4);
    message_t *msg = message_create(1, 2, 0, NULL, 0);
    ASSERT(mailbox_enqueue(mb, msg));
    ASSERT(!mailbox_is_empty(mb));
    ASSERT_EQ(mailbox_count(mb), (size_t)1);

    message_t *out = mailbox_dequeue(mb);
    ASSERT_EQ(out, msg);
    ASSERT(mailbox_is_empty(mb));
    message_destroy(out);
    mailbox_destroy(mb);
    return 0;
}

static int test_full(void) {
    mailbox_t *mb = mailbox_create(2); /* capacity rounds to 2 */
    message_t *m1 = message_create(0, 0, 0, NULL, 0);
    message_t *m2 = message_create(0, 0, 0, NULL, 0);
    message_t *m3 = message_create(0, 0, 0, NULL, 0);

    ASSERT(mailbox_enqueue(mb, m1));
    ASSERT(mailbox_enqueue(mb, m2));
    ASSERT(!mailbox_enqueue(mb, m3)); /* full */
    ASSERT_EQ(mailbox_count(mb), (size_t)2);

    message_destroy(m3);
    message_destroy(mailbox_dequeue(mb));
    message_destroy(mailbox_dequeue(mb));
    mailbox_destroy(mb);
    return 0;
}

static int test_empty_dequeue(void) {
    mailbox_t *mb = mailbox_create(4);
    ASSERT_NULL(mailbox_dequeue(mb));
    mailbox_destroy(mb);
    return 0;
}

static int test_fifo_order(void) {
    mailbox_t *mb = mailbox_create(8);
    message_t *msgs[4];
    for (int i = 0; i < 4; i++) {
        msgs[i] = message_create(0, 0, (msg_type_t)i, NULL, 0);
        ASSERT(mailbox_enqueue(mb, msgs[i]));
    }
    for (int i = 0; i < 4; i++) {
        message_t *out = mailbox_dequeue(mb);
        ASSERT_EQ(out->type, (msg_type_t)i);
        message_destroy(out);
    }
    mailbox_destroy(mb);
    return 0;
}

static int test_wraparound(void) {
    mailbox_t *mb = mailbox_create(4);
    /* Fill and drain multiple times to force wraparound */
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 4; i++) {
            message_t *m = message_create(0, 0, (msg_type_t)(round * 10 + i), NULL, 0);
            ASSERT(mailbox_enqueue(mb, m));
        }
        for (int i = 0; i < 4; i++) {
            message_t *out = mailbox_dequeue(mb);
            ASSERT_EQ(out->type, (msg_type_t)(round * 10 + i));
            message_destroy(out);
        }
        ASSERT(mailbox_is_empty(mb));
    }
    mailbox_destroy(mb);
    return 0;
}

static int test_destroy_with_messages(void) {
    mailbox_t *mb = mailbox_create(4);
    uint32_t data = 99;
    mailbox_enqueue(mb, message_create(0, 0, 0, &data, sizeof(data)));
    mailbox_enqueue(mb, message_create(0, 0, 0, &data, sizeof(data)));
    /* Destroying should clean up remaining messages */
    mailbox_destroy(mb);
    return 0;
}

int main(void) {
    printf("test_mailbox:\n");
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_enqueue_dequeue);
    RUN_TEST(test_full);
    RUN_TEST(test_empty_dequeue);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_destroy_with_messages);
    TEST_REPORT();
}
