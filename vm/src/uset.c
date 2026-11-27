/*
 * Usets: sets of characters drawn from the ASCII characters, the 128
 * non-ASCII bytes and the code points from 0x80 (less the surrogates):
 * one universe, so usets form a Boolean algebra (design/design.md, "Csets
 * and usets"). The ASCII characters and the bytes are in bits, as in a
 * cset; code points in sorted, disjoint, non-adjacent ranges.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>

typedef uint32_t Range2[2];

Uset *UsetUniv;

Uset *newuset(void) { return gcnew(sizeof(Uset), T_USET); }

bool uset_hasrune(const Uset *c, int32_t r) {
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

int64_t uset_size(const Uset *c) {
	int64_t n = 0;
	for (int i = 0; i < 4; i++)
		n += __builtin_popcountll(c->bits[i]);
	for (int i = 0; i < c->nranges; i++)
		n += c->ranges[i][1] - c->ranges[i][0] + 1;
	return n;
}

bool uset_equal(const Uset *a, const Uset *b) {
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
static void setranges(Uset *c, RL l) {
	c->ranges = NULL;
	if (l.n) {
		c->ranges = gcnew((size_t)l.n * sizeof(Range2), T_RAW);
		memcpy(c->ranges, l.r, (size_t)l.n * sizeof(Range2));
	}
	c->nranges = l.n;
	free(l.r);
}

/* ---- Operations ---- */

Uset *usetop(int op, const Uset *a, const Uset *b) {
	Uset *r = newuset();
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

Uset *ucomplement(const Uset *c) {
	Uset *r = newuset();
	for (int i = 0; i < 4; i++)
		r->bits[i] = ~c->bits[i];
	setranges(r, complementr(c->ranges, c->nranges));
	return r;
}

/* usetofcset returns the uset with c's members: a cset's universe, the
 * 256 bytes, is a part of a uset's. */
Uset *usetofcset(const Cset *c) {
	Uset *r = newuset();
	memcpy(r->bits, c->bits, sizeof r->bits);
	return r;
}

/* usetof returns the uset of the characters of s read as UTF-8: valid
 * sequences are code points, and each byte that is not part of one is
 * that byte. */
Uset *usetof(String s) {
	Uset *c = newuset();
	const uint8_t *p = (const uint8_t *)s.s;
	int64_t n = s.n;
	RL rs = {0};
	for (int64_t i = 0; i < n;) {
		if (p[i] < 0x80) {
			c->bits[p[i] >> 6] |= (uint64_t)1 << (p[i] & 63);
			i++;
			continue;
		}
		int w;
		int32_t r = utf8decode(p + i, (size_t)(n - i), &w);
		if (r == RuneError && w == 1)
			c->bits[p[i] >> 6] |= (uint64_t)1 << (p[i] & 63);
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

/* uset_string returns c's canonical string: its members in string
 * order, a code point as its UTF-8 encoding and a byte as itself. Read
 * as UTF-8 it is exactly the members: a lone lead byte is followed only
 * by members whose first byte is at least as large, never a
 * continuation byte. */
static String usetstr(const Uset *c) {
	SB b = {0};
	for (int x = 0; x < 0x80; x++)
		if (uset_hasbyte(c, x))
			sbputc(&b, x);
	int next = 0x80;
	for (int i = 0; i < c->nranges; i++)
		for (uint32_t x = c->ranges[i][0]; x <= c->ranges[i][1]; x++) {
			uint8_t e[4];
			int k = utf8encode(e, (int32_t)x);
			for (; next <= e[0]; next++)
				if (uset_hasbyte(c, next))
					sbputc(&b, next);
			sbadd(&b, e, (size_t)k);
		}
	for (; next < 256; next++)
		if (uset_hasbyte(c, next))
			sbputc(&b, next);
	return sbstring(&b);
}

String uset_string(const Uset *c) {
	if (!c->str) {
		String *s = gcnew(sizeof *s, T_STRHDR);
		*s = usetstr(c);
		((Uset *)c)->str = s;
	}
	return *(String *)c->str;
}

/* umatch reports whether the character of s at byte offset off is in c,
 * and its width: an ASCII byte is tested in the bits with no decoding;
 * from 0x80, a valid sequence is a code point and any other byte that
 * byte. */
bool umatch(const Uset *c, String s, int64_t off, int *w) {
	uint8_t b = (uint8_t)s.s[off];
	if (b < 0x80) {
		*w = 1;
		return uset_hasbyte(c, b);
	}
	int32_t r = utf8decode((const uint8_t *)s.s + off, (size_t)(s.n - off), w);
	if (r == RuneError && *w == 1)
		return uset_hasbyte(c, b);
	return uset_hasrune(c, r);
}

/* uset_isuniv reports whether c is &uset. */
bool uset_isuniv(const Uset *c) {
	for (int i = 0; i < 4; i++)
		if (c->bits[i] != ~(uint64_t)0)
			return false;
	return c->nranges == 2 && c->ranges[0][0] == 0x80 && c->ranges[0][1] == SurrLo - 1 &&
	       c->ranges[1][0] == SurrHi + 1 && c->ranges[1][1] == MaxRune;
}

void uset_init(void) {
	UsetUniv = ucomplement(newuset());
	gcroot((void **)&UsetUniv);
}

/* ---- Members ---- */

/* charstr returns the one-character string of a member: a byte (below
 * 0x100, from bits) or a code point's encoding. */
static String charstr(uint32_t x, bool byte) {
	uint8_t e[4];
	int k = 1;
	if (byte)
		e[0] = (uint8_t)x;
	else
		k = utf8encode(e, (int32_t)x);
	return strcopy((const char *)e, k);
}

/* uset_next gives c's members one at a time in canonical order (as
 * uset_string lays them out), without making that string: the ASCII
 * members; then the bytes from 0x80 merged with the code points, a byte
 * before the code points whose encodings begin with a byte at least as
 * large; then the remaining bytes. */
bool uset_next(const Uset *c, UsetIter *it, String *out) {
	while (it->next < 0x80) {
		int b = it->next++;
		if (uset_hasbyte(c, b)) {
			*out = charstr((uint32_t)b, true);
			return true;
		}
	}
	while (it->ri < c->nranges) {
		if (it->x == 0)
			it->x = c->ranges[it->ri][0];
		uint8_t e[4];
		utf8encode(e, (int32_t)it->x);
		if (it->next <= e[0]) {
			int b = it->next++;
			if (uset_hasbyte(c, b)) {
				*out = charstr((uint32_t)b, true);
				return true;
			}
			continue;
		}
		*out = charstr(it->x, false);
		if (it->x == c->ranges[it->ri][1])
			it->ri++, it->x = 0;
		else
			it->x++;
		return true;
	}
	while (it->next < 256) {
		int b = it->next++;
		if (uset_hasbyte(c, b)) {
			*out = charstr((uint32_t)b, true);
			return true;
		}
	}
	return false;
}

/* uset_nth returns c's member number k (0-based) in an order of its
 * own: the bytes in bits, then the code points. */
String uset_nth(const Uset *c, int64_t k) {
	for (int b = 0; b < 256; b++)
		if (uset_hasbyte(c, b) && k-- == 0)
			return charstr((uint32_t)b, true);
	for (int i = 0; i < c->nranges; i++) {
		int64_t m = (int64_t)c->ranges[i][1] - c->ranges[i][0] + 1;
		if (k < m)
			return charstr(c->ranges[i][0] + (uint32_t)k, false);
		k -= m;
	}
	return mkstr("", 0);
}
