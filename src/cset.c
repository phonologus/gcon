/*
 * Csets (gcon's cset.go): the ASCII characters and the non-ASCII bytes
 * in bits, code points from 0x80 in sorted, disjoint, non-adjacent
 * ranges, without surrogates. One universe, so csets form a Boolean
 * algebra.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

typedef uint32_t Range2[2];

Cset *CsetAll, *CsetASCII, *CsetDigits, *CsetLcase, *CsetUcase, *CsetLetters, *CsetUniv;

Cset *newcset(void) { return gcnew(sizeof(Cset), T_CSET); }

bool cset_has(const Cset *c, int b) { return c->bits[b >> 6] >> (b & 63) & 1; }
void cset_set(Cset *c, int b) { c->bits[b >> 6] |= (uint64_t)1 << (b & 63); }

bool cset_hasrune(const Cset *c, int32_t r) {
	int lo = 0, hi = c->nranges;
	while (lo < hi) {
		int m = lo + (hi - lo) / 2;
		if (c->ranges[m][1] < (uint32_t)r)
			lo = m + 1;
		else
			hi = m;
	}
	return lo < c->nranges && c->ranges[lo][0] <= (uint32_t)r;
}

int64_t cset_size(const Cset *c) {
	int64_t n = 0;
	for (int i = 0; i < 4; i++)
		n += __builtin_popcountll(c->bits[i]);
	for (int i = 0; i < c->nranges; i++)
		n += c->ranges[i][1] - c->ranges[i][0] + 1;
	return n;
}

bool cset_equal(const Cset *a, const Cset *b) {
	if (memcmp(a->bits, b->bits, sizeof a->bits) != 0 || a->nranges != b->nranges)
		return false;
	return a->nranges == 0 || memcmp(a->ranges, b->ranges, (size_t)a->nranges * sizeof(Range2)) == 0;
}

/* ---- Ranges ---- */

typedef struct RL {
	Range2 *r;
	int n, cap;
} RL;

static void rladd(RL *l, uint32_t lo, uint32_t hi) {
	if (l->n == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 8;
		l->r = realloc(l->r, (size_t)l->cap * sizeof(Range2));
	}
	l->r[l->n][0] = lo;
	l->r[l->n][1] = hi;
	l->n++;
}

static int rcmp(const void *a, const void *b) {
	uint32_t x = (*(const Range2 *)a)[0], y = (*(const Range2 *)b)[0];
	return (x > y) - (x < y);
}

/* normalize merges sorted ranges that overlap or touch. */
static RL normalize(Range2 *rs, int n) {
	RL out = {0};
	for (int i = 0; i < n; i++) {
		if (out.n > 0 && rs[i][0] <= out.r[out.n - 1][1] + 1) {
			if (rs[i][1] > out.r[out.n - 1][1])
				out.r[out.n - 1][1] = rs[i][1];
			continue;
		}
		rladd(&out, rs[i][0], rs[i][1]);
	}
	return out;
}

static RL unionr(Range2 *a, int na, Range2 *b, int nb) {
	Range2 *all = xmalloc((size_t)(na + nb + 1) * sizeof(Range2));
	memcpy(all, a, (size_t)na * sizeof(Range2));
	memcpy(all + na, b, (size_t)nb * sizeof(Range2));
	qsort(all, (size_t)(na + nb), sizeof(Range2), rcmp);
	RL r = normalize(all, na + nb);
	free(all);
	return r;
}

static RL interr(Range2 *a, int na, Range2 *b, int nb) {
	RL out = {0};
	for (int i = 0, j = 0; i < na && j < nb;) {
		uint32_t lo = a[i][0] > b[j][0] ? a[i][0] : b[j][0];
		uint32_t hi = a[i][1] < b[j][1] ? a[i][1] : b[j][1];
		if (lo <= hi)
			rladd(&out, lo, hi);
		if (a[i][1] < b[j][1])
			i++;
		else
			j++;
	}
	return out;
}

static Range2 universe[2] = {{0x80, SurrLo - 1}, {SurrHi + 1, MaxRune}};

static RL complementr(Range2 *rs, int n) {
	RL inv = {0};
	uint32_t next = 0x80;
	for (int i = 0; i < n; i++) {
		if (rs[i][0] > next)
			rladd(&inv, next, rs[i][0] - 1);
		next = rs[i][1] + 1;
	}
	if (next <= MaxRune)
		rladd(&inv, next, MaxRune);
	RL r = interr(universe, 2, inv.r, inv.n);
	free(inv.r);
	return r;
}

/* setranges gives c the ranges l, copied to the heap; l is freed. */
static void setranges(Cset *c, RL l) {
	c->ranges = NULL;
	if (l.n) {
		c->ranges = gcnew((size_t)l.n * sizeof(Range2), T_RAW);
		memcpy(c->ranges, l.r, (size_t)l.n * sizeof(Range2));
	}
	c->nranges = l.n;
	free(l.r);
}

/* ---- Operations ---- */

Cset *csetop(int op, const Cset *a, const Cset *b) {
	Cset *r = newcset();
	for (int i = 0; i < 4; i++)
		r->bits[i] = op == '+' ? a->bits[i] | b->bits[i]
			   : op == '*' ? a->bits[i] & b->bits[i]
				       : a->bits[i] & ~b->bits[i];
	switch (op) {
	case '+':
		setranges(r, unionr(a->ranges, a->nranges, b->ranges, b->nranges));
		break;
	case '*':
		setranges(r, interr(a->ranges, a->nranges, b->ranges, b->nranges));
		break;
	default: {
		RL cb = complementr(b->ranges, b->nranges);
		setranges(r, interr(a->ranges, a->nranges, cb.r, cb.n));
		free(cb.r);
	}
	}
	return r;
}

Cset *complement(const Cset *c) {
	Cset *r = newcset();
	for (int i = 0; i < 4; i++)
		r->bits[i] = ~c->bits[i];
	setranges(r, complementr(c->ranges, c->nranges));
	return r;
}

/* csetfrom returns the cset of the characters of s, read in unit u. */
Cset *csetfrom(const char *s, int64_t n, int u) {
	Cset *c = newcset();
	const uint8_t *p = (const uint8_t *)s;
	if (u == BytesUnit) {
		for (int64_t i = 0; i < n; i++)
			cset_set(c, p[i]);
		return c;
	}
	RL rs = {0};
	for (int64_t i = 0; i < n;) {
		if (p[i] < 0x80) {
			cset_set(c, p[i]);
			i++;
			continue;
		}
		int w;
		int32_t r = utf8decode(p + i, (size_t)(n - i), &w);
		if (r == RuneError && w == 1)
			cset_set(c, p[i]);
		else
			rladd(&rs, (uint32_t)r, (uint32_t)r);
		i += w;
	}
	if (rs.n > 0) {
		qsort(rs.r, (size_t)rs.n, sizeof(Range2), rcmp);
		RL nr = normalize(rs.r, rs.n);
		RL in = interr(universe, 2, nr.r, nr.n);
		setranges(c, in);
		free(rs.r);
		free(nr.r);
	}
	return c;
}

Cset *csetof(String s) { return csetfrom(s.s, s.n, s.unit); }

/* cset_string returns c's canonical string: its members in order, text
 * if c has a code point, else bytes. */
static String csetstr(const Cset *c);

String cset_string(const Cset *c) {
	if (!c->str) {
		String *s = gcnew(sizeof *s, T_STRHDR);
		*s = csetstr(c);
		((Cset *)c)->str = s;
	}
	return *(String *)c->str;
}

static String csetstr(const Cset *c) {
	Buf b = {0};
	if (c->nranges == 0) {
		for (int x = 0; x < 256; x++)
			if (cset_has(c, x))
				bufputc(&b, x);
		return bufstring(&b, BytesUnit);
	}
	for (int x = 0; x < 0x80; x++)
		if (cset_has(c, x))
			bufputc(&b, x);
	int next = 0x80;
	for (int i = 0; i < c->nranges; i++)
		for (uint32_t x = c->ranges[i][0]; x <= c->ranges[i][1]; x++) {
			uint8_t e[4];
			int k = utf8encode(e, (int32_t)x);
			for (; next <= e[0]; next++)
				if (cset_has(c, next))
					bufputc(&b, next);
			bufadd(&b, e, (size_t)k);
		}
	for (; next < 256; next++)
		if (cset_has(c, next))
			bufputc(&b, next);
	return bufstring(&b, TextUnit);
}

/* bytesbits returns the bytes that match c in a bytes subject. */
static void bytesbits(const Cset *c, uint64_t out[4]) {
	memcpy(out, c->bits, sizeof c->bits);
	for (int i = 0; i < c->nranges; i++) {
		uint32_t lo = c->ranges[i][0], hi = c->ranges[i][1];
		uint8_t e[4];
		if (hi - lo + 1 >= 64) {
			for (int x = 0x80; x <= 0xBF; x++)
				out[x >> 6] |= (uint64_t)1 << (x & 63);
			utf8encode(e, (int32_t)lo);
			int l = e[0];
			utf8encode(e, (int32_t)hi);
			for (int x = l; x <= e[0]; x++)
				out[x >> 6] |= (uint64_t)1 << (x & 63);
			continue;
		}
		for (uint32_t x = lo; x <= hi; x++) {
			int k = utf8encode(e, (int32_t)x);
			for (int j = 0; j < k; j++)
				out[e[j] >> 6] |= (uint64_t)1 << (e[j] & 63);
		}
	}
}

Cset *cset_asbytes(const Cset *c) {
	if (c->nranges == 0)
		return (Cset *)c;
	Cset *r = newcset();
	bytesbits(c, r->bits);
	return r;
}

/* matchat reports whether the character of s at byte offset off is in
 * c, and its width. */
bool matchat(const Cset *c, String s, int64_t off, int *w) {
	uint8_t b = (uint8_t)s.s[off];
	if (s.unit == BytesUnit) {
		*w = 1;
		if (c->nranges == 0)
			return cset_has(c, b);
		uint64_t bb[4];
		bytesbits(c, bb); /* TODO: cache */
		return bb[b >> 6] >> (b & 63) & 1;
	}
	if (b < 0x80) {
		*w = 1;
		return cset_has(c, b);
	}
	int32_t r = utf8decode((const uint8_t *)s.s + off, (size_t)(s.n - off), w);
	if (r == RuneError && *w == 1)
		return cset_has(c, b);
	return cset_hasrune(c, r);
}

static bool bitsonly(const Cset *c, uint64_t b0, uint64_t b1, uint64_t b2, uint64_t b3) {
	return c->nranges == 0 && c->bits[0] == b0 && c->bits[1] == b1 && c->bits[2] == b2 &&
	       c->bits[3] == b3;
}

const char *csetname_(const Cset *c) {
	const uint64_t all = ~(uint64_t)0;
	const uint64_t digits = (uint64_t)0x3FF << 48;
	const uint64_t ucase = (uint64_t)0x3FFFFFF << 1, lcase = (uint64_t)0x3FFFFFF << 33;
	if (bitsonly(c, 0, ucase | lcase, 0, 0)) return "&letters";
	if (bitsonly(c, 0, lcase, 0, 0)) return "&lcase";
	if (bitsonly(c, 0, ucase, 0, 0)) return "&ucase";
	if (bitsonly(c, digits, 0, 0, 0)) return "&digits";
	if (bitsonly(c, all, all, 0, 0)) return "&ascii";
	if (bitsonly(c, all, all, all, all)) return "&cset";
	if (cset_equal(c, CsetUniv)) return "&uset";
	return NULL;
}

void cset_init(void) {
	CsetAll = newcset();
	CsetASCII = newcset();
	for (int i = 0; i < 4; i++)
		CsetAll->bits[i] = ~(uint64_t)0;
	CsetASCII->bits[0] = CsetASCII->bits[1] = ~(uint64_t)0;
	CsetDigits = csetfrom("0123456789", 10, TextUnit);
	CsetLcase = csetfrom("abcdefghijklmnopqrstuvwxyz", 26, TextUnit);
	CsetUcase = csetfrom("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26, TextUnit);
	CsetLetters = csetop('+', CsetLcase, CsetUcase);
	CsetUniv = complement(newcset());
	Cset **statics[] = {&CsetAll, &CsetASCII, &CsetDigits, &CsetLcase, &CsetUcase, &CsetLetters, &CsetUniv};
	for (size_t i = 0; i < sizeof statics / sizeof statics[0]; i++)
		gcroot((void **)statics[i]);
}
