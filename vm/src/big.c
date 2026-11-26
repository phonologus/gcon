/*
 * Large integers (gcon's bigint.go; iconx's rlrgint.r): sign and
 * magnitude in base 2^32 digits, least significant first. Every result
 * is normalised: a value that fits in 64 bits is a small integer, so a
 * Big and a small integer are never equal.
 */
#include "vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static Big *newbig(int n) {
	Big *b = gcnew(sizeof *b + (size_t)(n > 0 ? n : 1) * sizeof b->d[0], T_RAW);
	b->n = n;
	return b;
}

static void trim(Big *b) {
	while (b->n > 0 && b->d[b->n - 1] == 0)
		b->n--;
	if (b->n == 0)
		b->neg = false;
}

Big *bigofint(int64_t i) {
	Big *b = newbig(2);
	uint64_t m = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
	b->neg = i < 0;
	b->d[0] = (uint32_t)m;
	b->d[1] = (uint32_t)(m >> 32);
	trim(b);
	return b;
}

Big *bigval(Value x) { return VKIND(x) == KBig ? x.w.p : bigofint(x.w.i); }

Value mkinteger(Big *b) {
	trim(b);
	if (b->n <= 2) {
		uint64_t m = b->n == 0 ? 0 : b->d[0] | (b->n == 2 ? (uint64_t)b->d[1] << 32 : 0);
		if (!b->neg && m <= INT64_MAX)
			return intval((int64_t)m);
		if (b->neg && m <= (uint64_t)INT64_MAX + 1)
			return intval((int64_t)(0 - m));
	}
	return ptrval(KBig, b);
}

/* ---- Magnitudes ---- */

static int magcmp(const Big *a, const Big *b) {
	if (a->n != b->n)
		return a->n < b->n ? -1 : 1;
	for (int i = a->n - 1; i >= 0; i--)
		if (a->d[i] != b->d[i])
			return a->d[i] < b->d[i] ? -1 : 1;
	return 0;
}

static Big *magadd(const Big *a, const Big *b) {
	if (a->n < b->n) {
		const Big *t = a;
		a = b;
		b = t;
	}
	Big *r = newbig(a->n + 1);
	uint64_t c = 0;
	for (int i = 0; i < a->n; i++) {
		c += (uint64_t)a->d[i] + (i < b->n ? b->d[i] : 0);
		r->d[i] = (uint32_t)c;
		c >>= 32;
	}
	r->d[a->n] = (uint32_t)c;
	trim(r);
	return r;
}

/* magsub returns |a| - |b|, which must not be negative. */
static Big *magsub(const Big *a, const Big *b) {
	Big *r = newbig(a->n);
	int64_t c = 0;
	for (int i = 0; i < a->n; i++) {
		c += (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0);
		r->d[i] = (uint32_t)c;
		c = c < 0 ? -1 : 0;
	}
	trim(r);
	return r;
}

static Big *magmul(const Big *a, const Big *b) {
	Big *r = newbig(a->n + b->n);
	for (int i = 0; i < a->n; i++) {
		uint64_t c = 0;
		for (int j = 0; j < b->n; j++) {
			c += (uint64_t)a->d[i] * b->d[j] + r->d[i + j];
			r->d[i + j] = (uint32_t)c;
			c >>= 32;
		}
		r->d[i + b->n] = (uint32_t)c;
	}
	trim(r);
	return r;
}

/* magdivmod divides |a| by |b| (nonzero), Knuth's algorithm D. */
static void magdivmod(const Big *a, const Big *b, Big **q, Big **r) {
	if (magcmp(a, b) < 0) {
		*q = newbig(0);
		*r = newbig(a->n);
		memcpy((*r)->d, a->d, (size_t)a->n * 4);
		trim(*r);
		return;
	}
	if (b->n == 1) {
		Big *qq = newbig(a->n);
		uint64_t rem = 0;
		for (int i = a->n - 1; i >= 0; i--) {
			uint64_t cur = rem << 32 | a->d[i];
			qq->d[i] = (uint32_t)(cur / b->d[0]);
			rem = cur % b->d[0];
		}
		trim(qq);
		Big *rr = newbig(1);
		rr->d[0] = (uint32_t)rem;
		trim(rr);
		*q = qq, *r = rr;
		return;
	}
	int n = b->n, m = a->n - b->n;
	int s = __builtin_clz(b->d[n - 1]);
	uint32_t *vn = xcalloc((size_t)n, 4), *un = xcalloc((size_t)(a->n + 1), 4);
	for (int i = n - 1; i > 0; i--)
		vn[i] = (b->d[i] << s) | (s ? (uint32_t)((uint64_t)b->d[i - 1] >> (32 - s)) : 0);
	vn[0] = b->d[0] << s;
	un[a->n] = s ? (uint32_t)((uint64_t)a->d[a->n - 1] >> (32 - s)) : 0;
	for (int i = a->n - 1; i > 0; i--)
		un[i] = (a->d[i] << s) | (s ? (uint32_t)((uint64_t)a->d[i - 1] >> (32 - s)) : 0);
	un[0] = a->d[0] << s;
	Big *qq = newbig(m + 1);
	for (int j = m; j >= 0; j--) {
		uint64_t num = (uint64_t)un[j + n] << 32 | un[j + n - 1];
		uint64_t qhat = num / vn[n - 1], rhat = num % vn[n - 1];
		while (qhat >= ((uint64_t)1 << 32) ||
		       qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
			qhat--;
			rhat += vn[n - 1];
			if (rhat >= ((uint64_t)1 << 32))
				break;
		}
		int64_t borrow = 0;
		uint64_t carry = 0;
		for (int i = 0; i < n; i++) {
			uint64_t p = qhat * vn[i] + carry;
			carry = p >> 32;
			int64_t t = (int64_t)un[i + j] - borrow - (int64_t)(uint32_t)p;
			un[i + j] = (uint32_t)t;
			borrow = t < 0 ? 1 : 0;
		}
		int64_t t = (int64_t)un[j + n] - borrow - (int64_t)carry;
		un[j + n] = (uint32_t)t;
		if (t < 0) {
			qhat--;
			uint64_t c = 0;
			for (int i = 0; i < n; i++) {
				c += (uint64_t)un[i + j] + vn[i];
				un[i + j] = (uint32_t)c;
				c >>= 32;
			}
			un[j + n] += (uint32_t)c;
		}
		qq->d[j] = (uint32_t)qhat;
	}
	Big *rr = newbig(n);
	for (int i = 0; i < n; i++)
		rr->d[i] = (un[i] >> s) | (s ? (uint32_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
	trim(qq);
	trim(rr);
	free(vn);
	free(un);
	*q = qq, *r = rr;
}

/* ---- Arithmetic ---- */

static Big *signedadd(const Big *a, bool aneg, const Big *b, bool bneg) {
	Big *r;
	if (aneg == bneg) {
		r = magadd(a, b);
		r->neg = aneg;
	} else if (magcmp(a, b) >= 0) {
		r = magsub(a, b);
		r->neg = aneg;
	} else {
		r = magsub(b, a);
		r->neg = bneg;
	}
	trim(r);
	return r;
}

Value bigadd(Value x, Value y) {
	Big *a = bigval(x), *b = bigval(y);
	return mkinteger(signedadd(a, a->neg, b, b->neg));
}

Value bigsub(Value x, Value y) {
	Big *a = bigval(x), *b = bigval(y);
	return mkinteger(signedadd(a, a->neg, b, !b->neg && b->n > 0));
}

Value bigmul(Value x, Value y) {
	Big *a = bigval(x), *b = bigval(y);
	Big *r = magmul(a, b);
	r->neg = a->neg != b->neg;
	return mkinteger(r);
}

Value bigdiv(Value x, Value y) {
	Big *a = bigval(x), *b = bigval(y), *q, *r;
	if (b->n == 0)
		runerr(201);
	magdivmod(a, b, &q, &r);
	q->neg = a->neg != b->neg;
	return mkinteger(q);
}

Value bigmod(Value x, Value y) {
	Big *a = bigval(x), *b = bigval(y), *q, *r;
	if (b->n == 0)
		runerr(202);
	magdivmod(a, b, &q, &r);
	r->neg = a->neg;
	return mkinteger(r);
}

Value bigneg(Value x) {
	Big *a = bigval(x), *r = newbig(a->n);
	memcpy(r->d, a->d, (size_t)a->n * 4);
	r->neg = !a->neg;
	return mkinteger(r);
}

Value neg_(Value x) {
	if (VKIND(x) == KInt && x.w.i != INT64_MIN)
		return intval(-x.w.i);
	return bigneg(x);
}

int bigsign(Value x) {
	if (VKIND(x) == KInt)
		return (x.w.i > 0) - (x.w.i < 0);
	Big *b = x.w.p;
	return b->n == 0 ? 0 : b->neg ? -1 : 1;
}

int bigcmp(Value x, Value y) {
	Big *a = bigval(x), *b = bigval(y);
	if (a->neg != b->neg)
		return a->neg ? -1 : 1;
	int c = magcmp(a, b);
	return a->neg ? -c : c;
}

bool bigbit0(Value x) {
	if (VKIND(x) == KInt)
		return x.w.i & 1;
	Big *b = x.w.p;
	return b->n > 0 && (b->d[0] & 1);
}

Value bigpow(Value x, int64_t n) {
	Value r = intval(1);
	while (n > 0) {
		if (n & 1)
			r = bigmul(r, x);
		n >>= 1;
		if (n > 0)
			x = bigmul(x, x);
	}
	return r;
}

/* ---- Two's complement (bitwise functions) ---- */

/* twos returns x in two's complement in n digits. */
static uint32_t *twos(const Big *x, int n) {
	uint32_t *d = xcalloc((size_t)n, 4);
	memcpy(d, x->d, (size_t)x->n * 4);
	if (x->neg) {
		uint64_t c = 1;
		for (int i = 0; i < n; i++) {
			c += (uint32_t)~d[i];
			d[i] = (uint32_t)c;
			c >>= 32;
		}
	}
	return d;
}

static Value untwos(uint32_t *d, int n) {
	Big *b = newbig(n);
	bool neg = d[n - 1] >> 31;
	if (neg) {
		uint64_t c = 1;
		for (int i = 0; i < n; i++) {
			c += (uint32_t)~d[i];
			b->d[i] = (uint32_t)c;
			c >>= 32;
		}
	} else
		memcpy(b->d, d, (size_t)n * 4);
	b->neg = neg;
	free(d);
	return mkinteger(b);
}

static Value bitop(Value x, Value y, int op) {
	Big *a = bigval(x), *b = bigval(y);
	int n = (a->n > b->n ? a->n : b->n) + 1;
	uint32_t *p = twos(a, n), *q = twos(b, n);
	for (int i = 0; i < n; i++)
		p[i] = op == '&' ? p[i] & q[i] : op == '|' ? p[i] | q[i] : p[i] ^ q[i];
	free(q);
	return untwos(p, n);
}

Value bigand(Value x, Value y) { return bitop(x, y, '&'); }
Value bigor(Value x, Value y) { return bitop(x, y, '|'); }
Value bigxor(Value x, Value y) { return bitop(x, y, '^'); }
Value bignot(Value x) { return bigsub(intval(-1), x); }

/* bigshift returns x * 2^n rounded down. */
Value bigshift(Value x, int64_t n) {
	Big *a = bigval(x);
	if (n >= 0) {
		int words = (int)(n / 32), bits = (int)(n % 32);
		Big *r = newbig(a->n + words + 1);
		for (int i = 0; i < a->n; i++) {
			uint64_t v = (uint64_t)a->d[i] << bits;
			r->d[i + words] |= (uint32_t)v;
			r->d[i + words + 1] |= (uint32_t)(v >> 32);
		}
		r->neg = a->neg;
		return mkinteger(r);
	}
	n = -n;
	if (n >= (int64_t)a->n * 32)
		return intval(a->neg ? -1 : 0);
	int words = (int)(n / 32), bits = (int)(n % 32);
	Big *r = newbig(a->n - words);
	bool lost = false;
	for (int i = 0; i < words; i++)
		lost |= a->d[i] != 0;
	if (bits)
		lost |= (a->d[words] & ((1u << bits) - 1)) != 0;
	for (int i = 0; i < r->n; i++) {
		uint64_t v = a->d[i + words] >> bits;
		if (bits && i + words + 1 < a->n)
			v |= (uint64_t)a->d[i + words + 1] << (32 - bits);
		r->d[i] = (uint32_t)v;
	}
	trim(r);
	Value v = mkinteger(r);
	if (a->neg) {
		v = neg_(v);
		if (lost)
			v = bigsub(v, intval(1));
	}
	return v;
}

/* ---- Conversions ---- */

void bigstr(Buf *b, const Big *x) {
	uint32_t *d = xmalloc((size_t)(x->n ? x->n : 1) * 4);
	memcpy(d, x->d, (size_t)x->n * 4);
	int n = x->n;
	uint32_t *groups = xmalloc((size_t)(x->n * 2 + 2) * 4);
	int ng = 0;
	while (n > 0) {
		uint64_t rem = 0;
		for (int i = n - 1; i >= 0; i--) {
			uint64_t cur = rem << 32 | d[i];
			d[i] = (uint32_t)(cur / 1000000000);
			rem = cur % 1000000000;
		}
		groups[ng++] = (uint32_t)rem;
		while (n > 0 && d[n - 1] == 0)
			n--;
	}
	if (ng == 0)
		groups[ng++] = 0;
	if (x->neg)
		bufputc(b, '-');
	bufprintf(b, "%u", groups[ng - 1]);
	for (int i = ng - 2; i >= 0; i--)
		bufprintf(b, "%09u", groups[i]);
	free(d);
	free(groups);
}

/* bigtoreal converts as iconx's bigtoreal: digit by digit from the most
 * significant, so that the rounding is the same. */
bool bigtoreal(const Big *x, double *out) {
	double r = 0;
	for (int i = x->n - 1; i >= 0; i--)
		r = r * 4294967296.0 + x->d[i];
	if (isinf(r))
		return false;
	*out = x->neg ? -r : r;
	return true;
}

bool realtointeger(double f, Value *out) {
	int64_t i;
	if (realtoint(f, &i)) {
		*out = intval(i);
		return true;
	}
	if (isinf(f) || isnan(f))
		return false;
	int e;
	double m = frexp(fabs(f), &e); /* f = m * 2^e, 0.5 <= m < 1 */
	uint64_t mant = (uint64_t)ldexp(m, 53);
	Value v = bigshift(intval((int64_t)mant), e - 53);
	*out = f < 0 ? neg_(v) : v;
	return true;
}

/* bigparse parses digits in base, with an optional sign. */
bool bigparse(const char *s, size_t n, int base, Value *out) {
	bool neg = false;
	if (n > 0 && (s[0] == '+' || s[0] == '-')) {
		neg = s[0] == '-';
		s++, n--;
	}
	if (n == 0)
		return false;
	Big *b = newbig((int)(n / 8 + 2));
	b->n = 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
		if (d >= base)
			return false;
		uint64_t carry = (uint64_t)d;
		for (int k = 0; k < b->n; k++) {
			carry += (uint64_t)b->d[k] * (uint64_t)base;
			b->d[k] = (uint32_t)carry;
			carry >>= 32;
		}
		if (carry)
			b->d[b->n++] = (uint32_t)carry;
	}
	b->neg = neg;
	*out = mkinteger(b);
	return true;
}

/* bigrand returns ?b for a large integer, as iconx's bigrand. */
extern double randval(void);
Value bigrand(Value x) {
	Big *a = bigval(x);
	int alen = a->n;
	Big *d = newbig(alen + 1);
	for (int i = 0; i <= alen; i++) /* the least significant digit first */
		d->d[i] = (uint32_t)(randval() * 4294967296.0);
	trim(d);
	Big *q, *r, *ab = newbig(a->n);
	memcpy(ab->d, a->d, (size_t)a->n * 4);
	magdivmod(d, ab, &q, &r);
	return bigadd(mkinteger(r), intval(1));
}

/* realpowbig raises a real to a large integer power, as iconx's
 * bigpowri: square and multiply over the exponent's bits. */
double realpowbig(double a, Value bv) {
	Big *b = bigval(bv);
	if (b->neg) {
		if (a == 0)
			runerr(204);
		a = 1 / a;
	}
	double r = 1.0;
	for (int i = b->n - 1; i >= 0; i--)
		for (int k = 31; k >= 0; k--) {
			r *= r;
			if (b->d[i] >> k & 1)
				r *= a;
		}
	return r;
}
