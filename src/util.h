/*
 * The C front end's utilities: memory, growable buffers and arrays,
 * UTF-8 (as Go's unicode/utf8, which the Go front end used), and the
 * small big-integer arithmetic literals need.
 *
 * The front end allocates with malloc and never frees what a run needs
 * (a translation is short-lived); it does not use the runtime's
 * collector.
 */
#ifndef GCON_UTIL_H
#define GCON_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrndup(const char *s, size_t n);

/* A growable byte buffer; b is NUL-terminated after each append. */
typedef struct Buf {
	char *b;
	size_t n, cap;
} Buf;

void bufadd(Buf *b, const void *p, size_t n);
void bufputc(Buf *b, int c);
void bufputs(Buf *b, const char *s);
void bufprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* A growable array of T: VEC(T) v = {0}; VPUSH(v, x); v.v[i], v.n. */
#define VEC(T) struct { T *v; size_t n, cap; }
#define VPUSH(a, x)                                                          \
	do {                                                                 \
		if ((a).n == (a).cap) {                                      \
			(a).cap = (a).cap ? 2 * (a).cap : 8;                 \
			(a).v = xrealloc((a).v, (a).cap * sizeof *(a).v);    \
		}                                                            \
		(a).v[(a).n++] = (x);                                        \
	} while (0)

/* UTF-8, as Go's: an invalid sequence decodes as RuneError with width 1
 * (width 0 for no input). */
enum { RuneError = 0xFFFD, RuneSelf = 0x80, MaxRune = 0x10FFFF };
int32_t utf8decode(const uint8_t *p, size_t n, int *width);
int32_t utf8decodelast(const uint8_t *p, size_t n, int *width);
int utf8encode(uint8_t *p, int32_t r); /* at most 4 bytes; returns the width */

/* Non-negative big integers, as little-endian base 2^32 limbs. */
typedef struct BigNat {
	uint32_t *d;
	int n; /* limbs in use; no leading zero limbs (0 is n == 0) */
} BigNat;

bool bignat_parse(const char *digits, size_t n, int base, BigNat *out); /* false: a bad digit */
bool bignat_int64(const BigNat *x, int64_t *out);                       /* false: too big */
char *bignat_decimal(const BigNat *x);                                  /* malloc'd */
size_t bignat_bytes(const BigNat *x, uint8_t **out);                    /* big-endian, malloc'd */

#endif
