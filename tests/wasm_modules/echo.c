#include <stdint.h>

/* Host imports */
extern int32_t mk_send(int64_t dest, int32_t type, const void *payload, int32_t size);
extern int64_t mk_self(void);
extern void    mk_log(int32_t level, const void *text, int32_t len);
extern int32_t mk_sleep_ms(int32_t ms);
extern int32_t mk_recv(uint32_t *type, void *buf, int32_t buf_size, uint32_t *size);

#define MSG_PING       200
#define MSG_PONG       201
#define MSG_GET_SELF   202
#define MSG_SELF_REPLY 203
#define MSG_SLEEP_TEST 204
#define MSG_RECV_TEST  205
#define MSG_RECV_REPLY 206
#define MSG_SLEEP_ERR  207

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
    if (msg_type == MSG_SLEEP_TEST) {
        int32_t rc = mk_sleep_ms(50);
        if (rc < 0) {
            /* No fiber — send error reply */
            const char *err = "no_fiber";
            mk_send(source_id, MSG_SLEEP_ERR, err, 8);
            return 1;
        }
        /* Resumed after sleep */
        const char *msg = "slept";
        mk_send(source_id, MSG_PONG, msg, 5);
        return 1;
    }
    if (msg_type == MSG_RECV_TEST) {
        /* Block waiting for next message via mk_recv */
        uint32_t recv_type;
        char buf[64];
        uint32_t recv_size;
        int32_t rc = mk_recv(&recv_type, buf, 64, &recv_size);
        if (rc < 0) {
            const char *err = "no_fiber";
            mk_send(source_id, MSG_RECV_REPLY, err, 8);
            return 1;
        }
        /* Echo back what we received */
        int32_t copy = recv_size < 64 ? (int32_t)recv_size : 64;
        mk_send(source_id, MSG_RECV_REPLY, buf, copy);
        return 1;
    }
    if (msg_type == 0) return 0;  /* stop signal */
    return 1;
}
