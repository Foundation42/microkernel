#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>
#include <stddef.h>

/* Compute SHA-1 hash. Output is 20 bytes. */
void sha1(const uint8_t *data, size_t len, uint8_t out[20]);

#endif /* SHA1_H */
