#include <stdint.h>

/* Host imports */
extern int32_t mk_send(int64_t dest, int32_t type, const void *payload, int32_t size);
extern int64_t mk_self(void);
extern int32_t mk_save_state(const void *key, int32_t key_len,
                              const void *data, int32_t data_len);
extern int32_t mk_load_state(const void *key, int32_t key_len,
                              void *buf, int32_t buf_cap);

#define MSG_SAVE_COUNT    210
#define MSG_GET_COUNT     211
#define MSG_COUNT_REPLY   212
#define MSG_INCREMENT     213
#define MSG_GET_VERSION   214
#define MSG_VERSION_REPLY 215

static int32_t count = 0;
static int32_t loaded = 0;

int32_t handle_message(int32_t msg_type, int64_t source_id,
                        const void *payload, int32_t payload_size) {
    (void)payload;
    (void)payload_size;

    /* On first message, try to restore saved count */
    if (!loaded) {
        loaded = 1;
        int32_t prev;
        int32_t rc = mk_load_state("count", 5, &prev, 4);
        if (rc == 4)
            count = prev;
    }

    if (msg_type == MSG_INCREMENT) {
        count += 10;  /* v2: increment by 10 */
        return 1;
    }
    if (msg_type == MSG_SAVE_COUNT) {
        mk_save_state("count", 5, &count, 4);
        return 1;
    }
    if (msg_type == MSG_GET_COUNT) {
        mk_send(source_id, MSG_COUNT_REPLY, &count, 4);
        return 1;
    }
    if (msg_type == MSG_GET_VERSION) {
        int32_t version = 2;  /* v2 */
        mk_send(source_id, MSG_VERSION_REPLY, &version, 4);
        return 1;
    }
    if (msg_type == 0) return 0;  /* stop signal */
    return 1;
}
