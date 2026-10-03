#ifndef STRUTIL_H
#define STRUTIL_H

#include <string.h>
#include <stddef.h>

/* Copy src into dst (capacity cap bytes). Always NUL-terminated; truncates if needed. */
static inline void xeno_copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) return;
	size_t n = strlen(src);
	if (n >= cap) n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

#endif // STRUTIL_H