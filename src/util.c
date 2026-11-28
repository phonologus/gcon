/*
 * The C front end's utilities (util.h).
 */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("gcon: out of memory\n", stderr);
		exit(2);
	}
	return p;
}

void *xcalloc(size_t n, size_t size) {
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p) {
		fputs("gcon: out of memory\n", stderr);
		exit(2);
	}
	return p;
}

void *xrealloc(void *p, size_t n) {
	p = realloc(p, n ? n : 1);
	if (!p) {
		fputs("gcon: out of memory\n", stderr);
		exit(2);
	}
	return p;
}

char *xstrndup(const char *s, size_t n) {
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

/* ---- Buffers ---- */

void bufadd(Buf *b, const void *p, size_t n) {
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->b = xrealloc(b->b, b->cap);
	}
	if (n)
		memcpy(b->b + b->n, p, n);
	b->n += n;
	b->b[b->n] = 0;
}

void bufputc(Buf *b, int c) {
	char ch = (char)c;
	bufadd(b, &ch, 1);
}

void bufputs(Buf *b, const char *s) { bufadd(b, s, strlen(s)); }

void bufprintf(Buf *b, const char *fmt, ...) {
	va_list ap;
	char small[256];
	va_start(ap, fmt);
	int k = vsnprintf(small, sizeof small, fmt, ap);
	va_end(ap);
	if (k < (int)sizeof small) {
		bufadd(b, small, (size_t)k);
		return;
	}
	char *big = xmalloc((size_t)k + 1);
	va_start(ap, fmt);
	vsnprintf(big, (size_t)k + 1, fmt, ap);
	va_end(ap);
	bufadd(b, big, (size_t)k);
	free(big);
}

/* ---- UTF-8 (Go's unicode/utf8) ---- */

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

/* utf8decodelast decodes the last character of p, as Go's
 * DecodeLastRune, step for step. */
int32_t utf8decodelast(const uint8_t *p, size_t n, int *width) {
	if (n == 0) {
		*width = 0;
		return RuneError;
	}
	long end = (long)n, start = end - 1;
	if (p[start] < RuneSelf) {
		*width = 1;
		return p[start];
	}
	long lim = end - 4; /* UTFMax */
	if (lim < 0)
		lim = 0;
	for (start--; start >= lim; start--)
		if ((p[start] & 0xC0) != 0x80) /* RuneStart */
			break;
	if (start < 0)
		start = 0;
	int w;
	int32_t r = utf8decode(p + start, (size_t)(end - start), &w);
	if (start + w != end) {
		*width = 1;
		return RuneError;
	}
	*width = w;
	return r;
}

int utf8encode(uint8_t *p, int32_t r) {
	if (r < 0 || r > MaxRune || (r >= 0xD800 && r <= 0xDFFF))
		r = RuneError;
	if (r < 0x80) {
		p[0] = (uint8_t)r;
		return 1;
	}
	if (r < 0x800) {
		p[0] = (uint8_t)(0xC0 | r >> 6);
		p[1] = (uint8_t)(0x80 | (r & 0x3F));
		return 2;
	}
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

/* ---- Big integers ---- */

static int digitval(int c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 10;
	return 99;
}

bool bignat_parse(const char *digits, size_t n, int base, BigNat *out) {
	BigNat x = {xmalloc(sizeof(uint32_t) * (n / 8 + 2)), 0};
	size_t cap = n / 8 + 2;
	for (size_t i = 0; i < n; i++) {
		int v = digitval((unsigned char)digits[i]);
		if (v >= base) {
			free(x.d);
			return false;
		}
		uint64_t carry = (uint64_t)v;
		for (int k = 0; k < x.n; k++) {
			uint64_t t = (uint64_t)x.d[k] * (uint64_t)base + carry;
			x.d[k] = (uint32_t)t;
			carry = t >> 32;
		}
		if (carry) {
			if ((size_t)x.n == cap) {
				cap *= 2;
				x.d = xrealloc(x.d, cap * sizeof(uint32_t));
			}
			x.d[x.n++] = (uint32_t)carry;
		}
	}
	*out = x;
	return true;
}

bool bignat_int64(const BigNat *x, int64_t *out) {
	if (x->n > 2)
		return false;
	uint64_t v = 0;
	for (int k = x->n - 1; k >= 0; k--)
		v = v << 32 | x->d[k];
	if (v > (uint64_t)INT64_MAX)
		return false;
	*out = (int64_t)v;
	return true;
}

char *bignat_decimal(const BigNat *x) {
	if (x->n == 0)
		return xstrndup("0", 1);
	uint32_t *t = xmalloc(sizeof *t * (size_t)x->n);
	memcpy(t, x->d, sizeof *t * (size_t)x->n);
	int n = x->n;
	Buf rev = {0};
	while (n > 0) { /* divide by 10^9, collecting nine digits at a time */
		uint64_t rem = 0;
		for (int k = n - 1; k >= 0; k--) {
			uint64_t cur = rem << 32 | t[k];
			t[k] = (uint32_t)(cur / 1000000000u);
			rem = cur % 1000000000u;
		}
		while (n > 0 && t[n - 1] == 0)
			n--;
		for (int i = 0; i < 9 && (n > 0 || rem > 0); i++) {
			bufputc(&rev, '0' + (int)(rem % 10));
			rem /= 10;
		}
	}
	free(t);
	char *s = xmalloc(rev.n + 1);
	for (size_t i = 0; i < rev.n; i++)
		s[i] = rev.b[rev.n - 1 - i];
	s[rev.n] = 0;
	free(rev.b);
	return s;
}

size_t bignat_bytes(const BigNat *x, uint8_t **out) {
	size_t n = (size_t)x->n * 4;
	uint8_t *p = xmalloc(n ? n : 1);
	for (int k = 0; k < x->n; k++)
		for (int j = 0; j < 4; j++)
			p[n - 1 - (size_t)(4 * k + j)] = (uint8_t)(x->d[k] >> (8 * j));
	size_t skip = 0;
	while (skip < n && p[skip] == 0)
		skip++;
	memmove(p, p + skip, n - skip);
	*out = p;
	return n - skip;
}

/* ------------------------------------------------------------------ */
/* StrMap */

struct StrEntry {
	struct StrEntry *next;
	char *key;
	size_t n;
	void *val;
};

static size_t strhash(const char *key, size_t n) {
	uint64_t h = 1469598103934665603u; /* FNV-1a */
	for (size_t i = 0; i < n; i++)
		h = (h ^ (unsigned char)key[i]) * 1099511628211u;
	return (size_t)h;
}

void *strmap_get(const StrMap *m, const char *key, size_t n) {
	if (!m->nbins)
		return NULL;
	for (struct StrEntry *e = m->bins[strhash(key, n) % m->nbins]; e; e = e->next)
		if (e->n == n && memcmp(e->key, key, n) == 0)
			return e->val;
	return NULL;
}

void strmap_put(StrMap *m, const char *key, size_t n, void *val) {
	if (m->nbins) {
		for (struct StrEntry *e = m->bins[strhash(key, n) % m->nbins]; e; e = e->next)
			if (e->n == n && memcmp(e->key, key, n) == 0) {
				e->val = val;
				return;
			}
	}
	if (m->n >= m->nbins) { /* grow: load factor 1 */
		size_t nb = m->nbins ? 2 * m->nbins : 16;
		struct StrEntry **bins = xcalloc(nb, sizeof *bins);
		for (size_t i = 0; i < m->nbins; i++)
			for (struct StrEntry *e = m->bins[i], *next; e; e = next) {
				next = e->next;
				size_t h = strhash(e->key, e->n) % nb;
				e->next = bins[h];
				bins[h] = e;
			}
		free(m->bins);
		m->bins = bins;
		m->nbins = nb;
	}
	struct StrEntry *e = xmalloc(sizeof *e);
	e->key = xstrndup(key, n);
	e->n = n;
	e->val = val;
	size_t h = strhash(key, n) % m->nbins;
	e->next = m->bins[h];
	m->bins[h] = e;
	m->n++;
}

void strmap_free(StrMap *m) {
	for (size_t i = 0; i < m->nbins; i++)
		for (struct StrEntry *e = m->bins[i], *next; e; e = next) {
			next = e->next;
			free(e->key);
			free(e);
		}
	free(m->bins);
	memset(m, 0, sizeof *m);
}

void strmap_each(const StrMap *m, void (*f)(void *ctx, const char *key, size_t n, void *val), void *ctx) {
	for (size_t i = 0; i < m->nbins; i++)
		for (struct StrEntry *e = m->bins[i]; e; e = e->next)
			f(ctx, e->key, e->n, e->val);
}
