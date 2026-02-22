/* Lightweight counter v2 — zero linear memory (no pointers, no strings).
   Works on memory-constrained targets like ESP32-C6 (512KB SRAM). */
#include <stdint.h>

extern int32_t mk_send_u32(int64_t dest, int32_t type, int32_t value);

#define MSG_GET_COUNT     211
#define MSG_COUNT_REPLY   212
#define MSG_INCREMENT     213
#define MSG_GET_VERSION   214
#define MSG_VERSION_REPLY 215

static int32_t count = 0;

int32_t handle_message(int32_t msg_type, int64_t source_id,
                        const void *payload, int32_t payload_size) {
    (void)payload;
    (void)payload_size;

    if (msg_type == MSG_INCREMENT) {
        count += 10;  /* v2: increment by 10 */
        return 1;
    }
    if (msg_type == MSG_GET_COUNT) {
        mk_send_u32(source_id, MSG_COUNT_REPLY, count);
        return 1;
    }
    if (msg_type == MSG_GET_VERSION) {
        mk_send_u32(source_id, MSG_VERSION_REPLY, 2);
        return 1;
    }
    if (msg_type == 0) return 0;
    return 1;
}
