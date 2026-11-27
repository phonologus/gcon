/*
 * Literals (lit.h), transcribed from the Go reference's internal/lit.
 */
#include "lit.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LitError *errorf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static LitError *errorf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	LitError *e = xmalloc(sizeof *e);
	e->msg = xmalloc((size_t)k + 1);
	e->len = (size_t)k;
	va_start(ap, fmt);
	vsnprintf(e->msg, (size_t)k + 1, fmt, ap);
	va_end(ap);
	return e;
}

/* errtok: the message prefix, then the token's text (all of it: it may
 * hold NUL bytes). */
static LitError *errtok(const char *prefix, const char *tok, size_t n) {
	Buf b = {0};
	bufputs(&b, prefix);
	bufadd(&b, tok, n);
	LitError *e = xmalloc(sizeof *e);
	e->msg = b.b;
	e->len = b.n;
	return e;
}

/* esc: the character after a backslash for the one-character escapes
 * (icont's esctab), 0 if it stands for itself. \8 and \9 are backspace
 * and tab: esctab's entries for them follow its octal digits. */
static int esc(unsigned char e) {
	switch (e) {
	case 'b': case 'B': return '\b';
	case 'd': case 'D': return 0177;
	case 'e': case 'E': return 033;
	case 'f': case 'F': return '\f';
	case 'l': case 'L': case 'n': case 'N': return '\n';
	case 'r': case 'R': return '\r';
	case 't': case 'T': return '\t';
	case 'v': case 'V': return '\v';
	case '8': return 010;
	case '9': return 011;
	}
	return -1;
}

static bool isOctal(unsigned char c) { return '0' <= c && c <= '7'; }

static bool hexVal(unsigned char c, int *v) {
	if ('0' <= c && c <= '9')
		*v = c - '0';
	else if ('a' <= c && c <= 'f')
		*v = c - 'a' + 10;
	else if ('A' <= c && c <= 'F')
		*v = c - 'A' + 10;
	else
		return false;
	return true;
}

static bool isSpace(unsigned char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* A decoded character of a literal: its bytes, and whether it was
 * written as an escape. */
typedef struct Piece {
	uint8_t b[4];
	uint8_t n;
	bool esc;
} Piece;

typedef VEC(Piece) Pieces;

static void add(Pieces *ps, const void *p, size_t n, bool e) {
	Piece pc = {.n = (uint8_t)n, .esc = e};
	memcpy(pc.b, p, n);
	VPUSH(*ps, pc);
}

static void add1(Pieces *ps, int c, bool e) {
	uint8_t b = (uint8_t)c;
	add(ps, &b, 1, e);
}

/* decode decodes a string or cset literal token into pieces. */
static LitError *decode(const char *tok, size_t tn, Pieces *out) {
	if (tn < 2 || (tok[0] != '"' && tok[0] != '\'') || tok[tn - 1] != tok[0]) {
		return errtok("malformed literal ", tok, tn);
	}
	const uint8_t *src = (const uint8_t *)tok + 1;
	size_t n = tn - 2;
	Pieces ps = {0};
	long cont = -1;         /* ps.n at a '_' followed only by blanks, else -1 */
	for (size_t i = 0; i < n;) {
		uint8_t c = src[i];
		if (c == '\n') {
			/* A '_' that was the last non-blank before the newline
			 * joins the next line, from its first non-blank, dropping
			 * the '_'. */
			if (cont >= 0)
				ps.n = (size_t)cont;
			i++;
			while (i < n && isSpace(src[i]))
				i++;
			cont = -1;
			continue;
		}
		if (c == '\\' && i + 1 < n) {
			cont = -1;
			i++;
			uint8_t e = src[i];
			i++;
			if (isOctal(e)) {
				int v = e - '0';
				for (int k = 1; k < 3 && i < n && isOctal(src[i]); k++) {
					v = v << 3 | (src[i] - '0');
					i++;
				}
				v &= 0377;
				add1(&ps, v, true);
			} else if (e == 'x' || e == 'X') {
				/* up to two hex digits; none gives NUL, as in icont */
				int v = 0, d;
				for (int k = 0; k < 2 && i < n; k++) {
					if (!hexVal(src[i], &d))
						break;
					v = v << 4 | d;
					i++;
				}
				add1(&ps, v, true);
			} else if (e == '^' && i < n) {
				add1(&ps, src[i] & 037, true);
				i++;
			} else if (e == 'u' || e == 'U') {
				int min = 4, max = 4, d;
				if (e == 'U')
					min = 1, max = 6;
				int32_t v = 0;
				int k = 0;
				while (k < max && i < n) {
					if (!hexVal(src[i], &d))
						break;
					v = v << 4 | d;
					i++;
					k++;
				}
				if (k < min) {
					free(ps.v);
					return errorf("\\%c escape needs %d hex digits", e, min);
				}
				if (v > MaxRune || (0xD800 <= v && v <= 0xDFFF)) {
					free(ps.v);
					return errorf("invalid code point U+%X", (unsigned)v);
				}
				uint8_t b[4];
				add(&ps, b, (size_t)utf8encode(b, v), true);
			} else {
				int x = esc(e);
				if (x >= 0)
					add1(&ps, x, true);
				else if (e >= 0x80) {
					i--; /* an escaped non-ASCII character */
					int w;
					utf8decode(src + i, n - i, &w); /* a UTF-8 character is one piece */
					add(&ps, src + i, (size_t)w, true);
					i += (size_t)w;
				} else
					add1(&ps, e, true);
			}
			continue;
		}
		if (c >= 0x80) {
			int w;
			utf8decode(src + i, n - i, &w); /* a UTF-8 character is one piece */
			add(&ps, src + i, (size_t)w, false);
			i += (size_t)w;
			cont = -1;
			continue;
		}
		if (c == '_')
			cont = (long)ps.n;
		else if (!isSpace(c))
			cont = -1;
		add1(&ps, c, false);
		i++;
	}
	*out = ps;
	return NULL;
}

LitError *lit_string(const char *tok, size_t n, char **out, size_t *outn) {
	Pieces ps;
	LitError *err = decode(tok, n, &ps);
	if (err)
		return err;
	Buf b = {0};
	bufadd(&b, "", 0);
	for (size_t i = 0; i < ps.n; i++)
		bufadd(&b, ps.v[i].b, ps.v[i].n);
	free(ps.v);
	*out = b.b;
	*outn = b.n;
	return NULL;
}

/* lit_cset: a range's ends are the pieces on either side of an
 * unescaped "..", and each must be one byte: a \u escape or a UTF-8
 * character is several members of a cset, so it cannot end a range. */
LitError *lit_cset(const char *tok, size_t n, LitCset *out) {
	Pieces ps;
	LitError *err = decode(tok, n, &ps);
	if (err)
		return err;
#define DOT(i) ((i) < ps.n && ps.v[i].n == 1 && ps.v[i].b[0] == '.' && !ps.v[i].esc)
	VEC(char *) segs = {0};
	VEC(size_t) lens = {0};
	VEC(LitRange) ranges = {0};
	Buf b = {0};
	bufadd(&b, "", 0);
	for (size_t i = 0; i < ps.n; i++) {
		if (DOT(i) && DOT(i + 1)) {
			if (DOT(i + 2))
				return errtok("three dots in cset literal ", tok, n);
			if (i == 0 || i + 2 >= ps.n)
				return errtok("\"..\" needs a character on each side in ", tok, n);
			Piece lo = ps.v[i - 1], hi = ps.v[i + 2];
			if (lo.n != 1 || hi.n != 1)
				return errtok("range end is not a single byte in ", tok, n);
			if (lo.b[0] > hi.b[0])
				return errtok("descending range in ", tok, n);
			VPUSH(ranges, ((LitRange){lo.b[0], hi.b[0]}));
			VPUSH(segs, b.b);
			VPUSH(lens, b.n);
			b = (Buf){0};
			bufadd(&b, "", 0);
			i++;
			continue;
		}
		bufadd(&b, ps.v[i].b, ps.v[i].n);
	}
#undef DOT
	VPUSH(segs, b.b);
	VPUSH(lens, b.n);
	free(ps.v);
	*out = (LitCset){segs.v, lens.v, segs.n, ranges.v, ranges.n};
	return NULL;
}

/* A uset literal's piece: a code point (several bytes: raw UTF-8, or a
 * \u or \U escape from 0x80) or a single byte (ASCII, or a byte from
 * 0x80). */
static void upiece(Piece p, int32_t *v, bool *cp) {
	*cp = p.n > 1;
	if (*cp) {
		int w;
		*v = utf8decode(p.b, p.n, &w);
	} else
		*v = p.b[0];
}

static void ubit(LitUset *u, int b) { u->bits[b >> 6] |= (uint64_t)1 << (b & 63); }

LitError *lit_uset(const char *tok, size_t n, LitUset *out) {
	if (n < 3 || tok[0] != 'u')
		return errtok("malformed literal ", tok, n);
	Pieces ps;
	LitError *err = decode(tok + 1, n - 1, &ps);
	if (err)
		return err;
#define DOT(i) ((i) < ps.n && ps.v[i].n == 1 && ps.v[i].b[0] == '.' && !ps.v[i].esc)
	memset(out, 0, sizeof *out);
	VEC(LitUrange) rs = {0};
	for (size_t i = 0; i < ps.n; i++) {
		int32_t v;
		bool cp;
		if (DOT(i) && DOT(i + 1)) {
			if (DOT(i + 2))
				return errtok("three dots in cset literal ", tok, n);
			if (i == 0 || i + 2 >= ps.n)
				return errtok("\"..\" needs a character on each side in ", tok, n);
			int32_t lo, hi;
			bool locp, hicp;
			upiece(ps.v[i - 1], &lo, &locp);
			upiece(ps.v[i + 2], &hi, &hicp);
			/* ASCII is either; otherwise both code points or both bytes */
			if ((locp && !hicp && hi >= 0x80) || (hicp && !locp && lo >= 0x80))
				return errtok("range mixes a code point and a byte in ", tok, n);
			if (lo > hi)
				return errtok("descending range in ", tok, n);
			bool bytes = !locp && !hicp;
			for (int32_t x = lo; x <= hi && (bytes ? x <= 0xFF : x < 0x80); x++)
				ubit(out, x);
			if (!bytes && hi >= 0x80)
				VPUSH(rs, ((LitUrange){lo < 0x80 ? 0x80 : lo, hi}));
			i += 2; /* the second dot and the range's end, already added */
			continue;
		}
		upiece(ps.v[i], &v, &cp);
		if (cp)
			VPUSH(rs, ((LitUrange){v, v}));
		else
			ubit(out, v);
	}
#undef DOT
	free(ps.v);
	out->ranges = rs.v;
	out->nranges = rs.n;
	return NULL;
}

/* parseInt decodes the digits of the integer literal tok in base. */
static LitError *parseInt(const char *tok, size_t tn, const char *digits, size_t n, int base, LitNumber *out) {
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)digits[i] | 0x20;
		if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'z'))) {
			return errtok("invalid digits in ", tok, tn);
		}
	}
	BigNat b;
	if (n == 0 || !bignat_parse(digits, n, base, &b)) {
		return errtok("invalid digits in ", tok, tn);
	}
	memset(out, 0, sizeof *out);
	if (bignat_int64(&b, &out->i)) {
		free(b.d);
		return NULL;
	}
	out->big = b;
	out->isbig = true;
	return NULL;
}

LitError *lit_number(const char *tok, size_t n, LitNumber *out) {
	size_t i;
	for (i = 0; i < n && tok[i] != 'r' && tok[i] != 'R'; i++)
		;
	if (i < n && i > 0) {
		/* strconv.Atoi of the base: an optional sign, decimal digits */
		size_t j = 0;
		bool neg = false;
		if (tok[0] == '+' || tok[0] == '-')
			neg = tok[0] == '-', j = 1;
		long base = 0;
		bool ok = j < i;
		for (; j < i && ok; j++) {
			if (tok[j] < '0' || tok[j] > '9' || base > 1000000)
				ok = false;
			else
				base = base * 10 + (tok[j] - '0');
		}
		if (neg)
			base = -base;
		if (!ok || base < 2 || base > 36) {
			return errtok("invalid radix in ", tok, n);
		}
		return parseInt(tok, n, tok + i + 1, n - i - 1, (int)base, out);
	}
	if (memchr(tok, '.', n) || memchr(tok, 'e', n) || memchr(tok, 'E', n)) {
		/* strconv.ParseFloat: correctly rounded; out of range is an
		 * error, underflow is not */
		char *s = xstrndup(tok, n), *end;
		errno = 0;
		double f = strtod(s, &end);
		bool bad = end != s + n || n == 0 || (errno == ERANGE && isinf(f));
		/* strtod takes forms ParseFloat does not ("inf", hex, "nan");
		 * a lexed real is digits, '.', 'e' and a sign only */
		for (size_t k = 0; k < n && !bad; k++)
			if (!strchr("0123456789.eE+-", s[k]))
				bad = true;
		free(s);
		if (bad) {
			return errtok("invalid real literal ", tok, n);
		}
		memset(out, 0, sizeof *out);
		out->real = f;
		out->isreal = true;
		return NULL;
	}
	return parseInt(tok, n, tok, n, 10, out);
}
