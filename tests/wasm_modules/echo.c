#include <stdint.h>

/* Host imports */
extern int32_t mk_send(int64_t dest, int32_t type, const void *payload, int32_t size);
extern int64_t mk_self(void);
extern void    mk_log(int32_t level, const void *text, int32_t len);

#define MSG_PING       200
#define MSG_PONG       201
#define MSG_GET_SELF   202
#define MSG_SELF_REPLY 203

int32_t handle_message(int32_t msg_type, int64_t source_id,
                        const void *payload, int32_t payload_size) {
    if (msg_type == MSG_PING) {
        mk_send(source_id, MSG_PONG, payload, payload_size);
        return 1;
    }
    if (msg_type == MSG_GET_SELF) {
        int64_t self_id = mk_self();
        mk_send(source_id, MSG_SELF_REPLY, &self_id, 8);
        return 1;
    }
    if (msg_type == 0) return 0;  /* stop signal */
    return 1;
}
