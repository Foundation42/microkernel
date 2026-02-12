#include "test_framework.h"
#include "microkernel/wire.h"
#include "microkernel/message.h"

static int test_roundtrip_with_payload(void) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    message_t *orig = message_create(0x100000001ULL, 0x200000002ULL,
                                     42, data, sizeof(data));
    ASSERT_NOT_NULL(orig);

    size_t wire_size;
    void *buf = wire_serialize(orig, &wire_size);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(wire_size, WIRE_HEADER_SIZE + sizeof(data));

    message_t *decoded = wire_deserialize(buf, wire_size);
    ASSERT_NOT_NULL(decoded);
    ASSERT_EQ(decoded->source, orig->source);
    ASSERT_EQ(decoded->dest, orig->dest);
    ASSERT_EQ(decoded->type, orig->type);
    ASSERT_EQ(decoded->payload_size, orig->payload_size);
    ASSERT(memcmp(decoded->payload, orig->payload, sizeof(data)) == 0);

    message_destroy(decoded);
    free(buf);
    message_destroy(orig);
    return 0;
}

static int test_roundtrip_empty(void) {
    message_t *orig = message_create(0x100000001ULL, 0x200000002ULL,
                                     7, NULL, 0);
    ASSERT_NOT_NULL(orig);

    size_t wire_size;
    void *buf = wire_serialize(orig, &wire_size);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(wire_size, (size_t)WIRE_HEADER_SIZE);

    message_t *decoded = wire_deserialize(buf, wire_size);
    ASSERT_NOT_NULL(decoded);
    ASSERT_EQ(decoded->source, orig->source);
    ASSERT_EQ(decoded->dest, orig->dest);
    ASSERT_EQ(decoded->type, orig->type);
    ASSERT_EQ(decoded->payload_size, (size_t)0);
    ASSERT_NULL(decoded->payload);

    message_destroy(decoded);
    free(buf);
    message_destroy(orig);
    return 0;
}

static int test_truncated_returns_null(void) {
    /* Too small for header */
    uint8_t small[10] = {0};
    ASSERT_NULL(wire_deserialize(small, sizeof(small)));

    /* Header says payload_size=100 but buffer is only header-sized */
    uint8_t data[] = {1, 2, 3, 4};
    message_t *msg = message_create(1, 2, 3, data, sizeof(data));
    ASSERT_NOT_NULL(msg);

    size_t wire_size;
    void *buf = wire_serialize(msg, &wire_size);
    ASSERT_NOT_NULL(buf);

    /* Pass only the header, missing payload */
    ASSERT_NULL(wire_deserialize(buf, WIRE_HEADER_SIZE));

    free(buf);
    message_destroy(msg);
    return 0;
}

static int test_null_inputs(void) {
    size_t sz;
    ASSERT_NULL(wire_serialize(NULL, &sz));
    ASSERT_NULL(wire_serialize(NULL, NULL));
    ASSERT_NULL(wire_deserialize(NULL, 100));
    return 0;
}

int main(void) {
    printf("test_wire:\n");
    RUN_TEST(test_roundtrip_with_payload);
    RUN_TEST(test_roundtrip_empty);
    RUN_TEST(test_truncated_returns_null);
    RUN_TEST(test_null_inputs);
    TEST_REPORT();
}
