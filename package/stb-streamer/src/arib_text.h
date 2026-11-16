/*
 * arib_text.h - ARIB STD-B24 text to UTF-8
 */
#ifndef ARIB_TEXT_H
#define ARIB_TEXT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decodes one ARIB STD-B24 string (the bytes that follow the length field of
 * a descriptor) into UTF-8. Characters this build does not map, and the
 * ARIB-only icons in rows 85..94, are dropped. The output is always
 * terminated. Returns the number of bytes written, without the terminator.
 */
size_t arib_text_to_utf8(const uint8_t *src, size_t len, char *dst, size_t cap);

#endif /* ARIB_TEXT_H */
