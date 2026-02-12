#include "test_framework.h"
#include "microkernel/wire.h"
#include "microkernel/message.h"
#include <arpa/inet.h>
#include <endian.h>

static int test_net_roundtrip_with_payload(void) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    message_t *orig = message_create(0x100000001ULL, 0x200000002ULL,
                                     42, data, sizeof(data));
    ASSERT_NOT_NULL(orig);

    size_t wire_size;
    void *buf = wire_serialize_net(orig, &wire_size);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(wire_size, WIRE_HEADER_SIZE + sizeof(data));

    message_t *decoded = wire_deserialize_net(buf, wire_size);
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

static int test_net_roundtrip_empty(void) {
    message_t *orig = message_create(0x100000001ULL, 0x200000002ULL,
                                     7, NULL, 0);
    ASSERT_NOT_NULL(orig);

    size_t wire_size;
    void *buf = wire_serialize_net(orig, &wire_size);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ(wire_size, (size_t)WIRE_HEADER_SIZE);

    message_t *decoded = wire_deserialize_net(buf, wire_size);
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

static int test_net_truncated_returns_null(void) {
    /* Too small for header */
    uint8_t small[10] = {0};
    ASSERT_NULL(wire_deserialize_net(small, sizeof(small)));

    /* Header says payload but buffer is only header-sized */
    uint8_t data[] = {1, 2, 3, 4};
    message_t *msg = message_create(1, 2, 3, data, sizeof(data));
    ASSERT_NOT_NULL(msg);

    size_t wire_size;
    void *buf = wire_serialize_net(msg, &wire_size);
    ASSERT_NOT_NULL(buf);

    /* Pass only the header, missing payload */
    ASSERT_NULL(wire_deserialize_net(buf, WIRE_HEADER_SIZE));

    free(buf);
    message_destroy(msg);
    return 0;
}

static int test_net_raw_bytes_big_endian(void) {
    message_t *msg = message_create(0x0102030405060708ULL,
                                    0x090A0B0C0D0E0F10ULL,
                                    0x11121314, NULL, 0);
    ASSERT_NOT_NULL(msg);

    size_t wire_size;
    uint8_t *buf = wire_serialize_net(msg, &wire_size);
    ASSERT_NOT_NULL(buf);

    /* Verify source is big-endian: 0x0102030405060708 */
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 0x02);
    ASSERT_EQ(buf[7], 0x08);

    /* Verify dest is big-endian: 0x090A0B0C0D0E0F10 */
    ASSERT_EQ(buf[8], 0x09);
    ASSERT_EQ(buf[9], 0x0A);
    ASSERT_EQ(buf[15], 0x10);

    /* Verify type is big-endian: 0x11121314 */
    ASSERT_EQ(buf[16], 0x11);
    ASSERT_EQ(buf[17], 0x12);
    ASSERT_EQ(buf[18], 0x13);
    ASSERT_EQ(buf[19], 0x14);

    /* Verify payload_size is big-endian: 0 */
    ASSERT_EQ(buf[20], 0x00);
    ASSERT_EQ(buf[21], 0x00);
    ASSERT_EQ(buf[22], 0x00);
    ASSERT_EQ(buf[23], 0x00);

    free(buf);
    message_destroy(msg);
    return 0;
}

int main(void) {
    printf("test_wire_net:\n");
    RUN_TEST(test_net_roundtrip_with_payload);
    RUN_TEST(test_net_roundtrip_empty);
    RUN_TEST(test_net_truncated_returns_null);
    RUN_TEST(test_net_raw_bytes_big_endian);
    TEST_REPORT();
}
