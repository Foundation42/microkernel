#include "microkernel/mailbox.h"
#include "microkernel/message.h"
#include <stdlib.h>

/* Round up to the next power of 2 (minimum 2). */
static size_t next_pow2(size_t v) {
    if (v < 2) return 2;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

mailbox_t *mailbox_create(size_t capacity) {
    mailbox_t *mb = calloc(1, sizeof(*mb));
    if (!mb) return NULL;

    mb->capacity = next_pow2(capacity);
    mb->messages = calloc(mb->capacity, sizeof(message_t *));
    if (!mb->messages) {
        free(mb);
        return NULL;
    }
    return mb;
}

void mailbox_destroy(mailbox_t *mb) {
    if (!mb) return;
    /* Drain remaining messages */
    message_t *msg;
    while ((msg = mailbox_dequeue(mb)) != NULL) {
        message_destroy(msg);
    }
    free(mb->messages);
    free(mb);
}

bool mailbox_enqueue(mailbox_t *mb, message_t *msg) {
    size_t count = mb->head - mb->tail;
    if (count >= mb->capacity) return false;

    mb->messages[mb->head & (mb->capacity - 1)] = msg;
    mb->head++;
    return true;
}

message_t *mailbox_dequeue(mailbox_t *mb) {
    if (mb->head == mb->tail) return NULL;

    message_t *msg = mb->messages[mb->tail & (mb->capacity - 1)];
    mb->tail++;
    return msg;
}

bool mailbox_is_empty(const mailbox_t *mb) {
    return mb->head == mb->tail;
}

size_t mailbox_count(const mailbox_t *mb) {
    return mb->head - mb->tail;
}
