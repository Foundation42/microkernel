#ifndef MICROKERNEL_MAILBOX_H
#define MICROKERNEL_MAILBOX_H

#include "types.h"

struct mailbox {
    message_t **messages;    /* Ring buffer of message pointers */
    size_t      capacity;    /* Power of 2 */
    size_t      head;        /* Producer index */
    size_t      tail;        /* Consumer index */
};

/* Create a mailbox. capacity is rounded up to the next power of 2. */
mailbox_t *mailbox_create(size_t capacity);

/* Destroy a mailbox. Drains and destroys any remaining messages. */
void mailbox_destroy(mailbox_t *mb);

/* Enqueue a message. Returns false if the mailbox is full. */
bool mailbox_enqueue(mailbox_t *mb, message_t *msg);

/* Dequeue a message. Returns NULL if the mailbox is empty. */
message_t *mailbox_dequeue(mailbox_t *mb);

/* Check if the mailbox is empty. */
bool mailbox_is_empty(const mailbox_t *mb);

/* Return the number of queued messages. */
size_t mailbox_count(const mailbox_t *mb);

#endif /* MICROKERNEL_MAILBOX_H */
