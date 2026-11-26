/* Memory, buffers, CRC-32 and UTF-8. */
#include "gcon-vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void nomem(void) {
	fputs("gcon-vm: out of memory\n", stderr);
	exit(2);
}

void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (p == NULL)
		nomem();
	return p;
}

void *xcalloc(size_t n, size_t size) {
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (p == NULL)
		nomem();
	return p;
}

static void bufgrow(Buf *b, size_t n) {
	if (b->cap - b->n >= n)
		return;
	size_t cap = b->cap ? b->cap : 64;
	while (cap - b->n < n)
		cap *= 2;
	b->b = realloc(b->b, cap);
	if (b->b == NULL)
		nomem();
	b->cap = cap;
}

void bufadd(Buf *b, const void *p, size_t n) {
	bufgrow(b, n);
	memcpy(b->b + b->n, p, n);
	b->n += n;
}

void bufputc(Buf *b, int c) {
	bufgrow(b, 1);
	b->b[b->n++] = (char)c;
}

void bufputs(Buf *b, const char *s) { bufadd(b, s, strlen(s)); }

void bufstr(Buf *b, Str s) { bufadd(b, s.s, s.n); }

void bufprintf(Buf *b, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	bufgrow(b, (size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(b->b + b->n, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->n += (size_t)n;
}

uint32_t crc32_ieee(const uint8_t *p, size_t n) {
	static uint32_t table[256];
	if (table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}
	uint32_t c = 0xFFFFFFFF;
	for (size_t i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFF;
}

/* utf8decode decodes the rune at p as Go's utf8.DecodeRune: an invalid
 * encoding (overlong, surrogate, beyond MaxRune, truncated) is RuneError
 * with width 1. */
int32_t utf8decode(const uint8_t *p, size_t n, int *width) {
	*width = 1;
	if (n == 0) {
		*width = 0;
		return RuneError;
	}
	uint8_t c = p[0];
	if (c < 0x80)
		return c;
	int w;
	int32_t r;
	uint8_t lo = 0x80, hi = 0xBF; /* the range of the second byte */
	if (c >= 0xC2 && c <= 0xDF)
		w = 2, r = c & 0x1F;
	else if (c >= 0xE0 && c <= 0xEF) {
		w = 3, r = c & 0x0F;
		if (c == 0xE0)
			lo = 0xA0;
		else if (c == 0xED)
			hi = 0x9F;
	} else if (c >= 0xF0 && c <= 0xF4) {
		w = 4, r = c & 0x07;
		if (c == 0xF0)
			lo = 0x90;
		else if (c == 0xF4)
			hi = 0x8F;
	} else
		return RuneError;
	if (n < (size_t)w || p[1] < lo || p[1] > hi)
		return RuneError;
	r = r << 6 | (p[1] & 0x3F);
	for (int i = 2; i < w; i++) {
		if (p[i] < 0x80 || p[i] > 0xBF)
			return RuneError;
		r = r << 6 | (p[i] & 0x3F);
	}
	*width = w;
	return r;
}

/* utf8encode writes r's encoding, as Go's utf8.EncodeRune: an invalid
 * rune (a surrogate, beyond MaxRune) as RuneError's. */
int utf8encode(uint8_t *p, int32_t r) {
	if (r >= 0 && r < 0x80) {
		p[0] = (uint8_t)r;
		return 1;
	}
	if (r >= 0 && r < 0x800) {
		p[0] = (uint8_t)(0xC0 | r >> 6);
		p[1] = (uint8_t)(0x80 | (r & 0x3F));
		return 2;
	}
	if (r < 0 || r > MaxRune || (r >= SurrLo && r <= SurrHi))
		r = RuneError;
	if (r < 0x10000) {
		p[0] = (uint8_t)(0xE0 | r >> 12);
		p[1] = (uint8_t)(0x80 | (r >> 6 & 0x3F));
		p[2] = (uint8_t)(0x80 | (r & 0x3F));
		return 3;
	}
	p[0] = (uint8_t)(0xF0 | r >> 18);
	p[1] = (uint8_t)(0x80 | (r >> 12 & 0x3F));
	p[2] = (uint8_t)(0x80 | (r >> 6 & 0x3F));
	p[3] = (uint8_t)(0x80 | (r & 0x3F));
	return 4;
}

bool isprint_rune(int32_t r) {
	if (r < 0x80)
		return r >= 0x20 && r < 0x7F;
	if (r > MaxRune)
		return false;
	int lo = 0, hi = gen_nnonprint;
	while (lo < hi) {
		int m = lo + (hi - lo) / 2;
		if ((uint32_t)r > gen_nonprint[m][1])
			lo = m + 1;
		else
			hi = m;
	}
	return !(lo < gen_nnonprint && (uint32_t)r >= gen_nonprint[lo][0]);
}
