#ifndef WS_FRAME_H
#define WS_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/* WebSocket opcodes */
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT         0x1
#define WS_OPCODE_BINARY       0x2
#define WS_OPCODE_CLOSE        0x8
#define WS_OPCODE_PING         0x9
#define WS_OPCODE_PONG         0xA

typedef struct {
    uint8_t  opcode;
    bool     fin;
    bool     masked;
    uint64_t payload_length;
    uint8_t  mask_key[4];
    size_t   header_size;   /* total bytes consumed for the header */
} ws_frame_info_t;

/* Parse a frame header from buffer.
   Returns >0: header parsed (returns header_size), info populated.
   Returns  0: need more data.
   Returns -1: protocol error. */
int ws_frame_parse_header(const uint8_t *buf, size_t len,
                          ws_frame_info_t *info);

/* Apply/remove XOR mask in-place. offset is for continuation. */
void ws_frame_apply_mask(uint8_t *data, size_t len,
                         const uint8_t mask[4], size_t offset);

/* Build a masked client frame. Returns bytes written to out.
   out must be at least payload_len + 14 bytes. */
size_t ws_frame_build(uint8_t opcode, bool fin,
                      const uint8_t *payload, size_t payload_len,
                      uint8_t *out);

/* Build a close frame with status code and optional reason.
   Returns bytes written to out. */
size_t ws_frame_build_close(uint16_t code, const char *reason,
                            size_t reason_len, uint8_t *out);

/* Build an unmasked server frame. Returns bytes written to out.
   out must be at least payload_len + 10 bytes. */
size_t ws_frame_build_unmasked(uint8_t opcode, bool fin,
                               const uint8_t *payload, size_t payload_len,
                               uint8_t *out);

/* Build an unmasked close frame (server-side). */
size_t ws_frame_build_close_unmasked(uint16_t code, const char *reason,
                                     size_t reason_len, uint8_t *out);

#endif /* WS_FRAME_H */
