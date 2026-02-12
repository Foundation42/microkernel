#ifndef BASE64_H
#define BASE64_H

#include <stdint.h>
#include <stddef.h>

/* Base64 encode. Writes null-terminated string to out.
   out must be at least ((len + 2) / 3) * 4 + 1 bytes.
   Returns number of characters written (excluding null). */
size_t base64_encode(const uint8_t *data, size_t len, char *out);

#endif /* BASE64_H */
