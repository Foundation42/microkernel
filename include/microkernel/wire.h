#ifndef MICROKERNEL_WIRE_H
#define MICROKERNEL_WIRE_H

#include "types.h"
#include "message.h"

/*
 * Wire format: 28-byte packed header + payload.
 * Host byte order (Unix sockets are same-machine).
 *
 * Offset  Size  Field
 * 0       8     source (actor_id_t)
 * 8       8     dest (actor_id_t)
 * 16      4     type (msg_type_t)
 * 20      4     payload_size (uint32_t)
 * 24      4     reserved (0)
 */

#define WIRE_HEADER_SIZE 28

typedef struct __attribute__((packed)) {
    actor_id_t source;
    actor_id_t dest;
    msg_type_t type;
    uint32_t   payload_size;
    uint32_t   reserved;
} wire_header_t;

_Static_assert(sizeof(wire_header_t) == WIRE_HEADER_SIZE,
               "wire_header_t must be exactly 28 bytes");

/* Serialize a message to wire format.
   Returns malloc'd buffer of (WIRE_HEADER_SIZE + payload_size) bytes.
   Sets *out_size to total buffer size. Returns NULL on failure. */
void *wire_serialize(const message_t *msg, size_t *out_size);

/* Deserialize wire bytes into a new message_t.
   buf must contain at least WIRE_HEADER_SIZE bytes.
   Returns NULL if buf_size is too small or allocation fails. */
message_t *wire_deserialize(const void *buf, size_t buf_size);

/* Network byte order variants for TCP/UDP (cross-machine).
   Same semantics as wire_serialize/wire_deserialize but encode
   multi-byte header fields in big-endian order. */
void *wire_serialize_net(const message_t *msg, size_t *out_size);
message_t *wire_deserialize_net(const void *buf, size_t buf_size);

#endif /* MICROKERNEL_WIRE_H */
