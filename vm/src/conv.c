/*
 * Strings, variables and conversions, as gcon's value.go, valuecore.go,
 * substr.go, ops.go (conversions) and real.go (strToNum, CvNum).
 */
#include "vm.h"
#include "gfx.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Strings ---- */

String mkstr(const char *s, int64_t n) { return (String){s, n}; }

String textc(const char *s) { return mkstr(s, (int64_t)strlen(s)); }

String strcopy(const char *s, int64_t n) {
	char *p = gcraw((size_t)n);
	memcpy(p, s, (size_t)n);
	return mkstr(p, n);
}

String slice(String s, int64_t i, int64_t j) { return (String){s.s + i - 1, j - i}; }

String concat(String a, String b) {
	if (b.n == 0)
		return a;
	if (a.n == 0)
		return b;
	char *p = gcraw((size_t)(a.n + b.n));
	memcpy(p, a.s, (size_t)a.n);
	memcpy(p + a.n, b.s, (size_t)b.n);
	return (String){p, a.n + b.n};
}

bool streq_(String a, const char *t) {
	return (size_t)a.n == strlen(t) && memcmp(a.s, t, (size_t)a.n) == 0;
}

char *cstr(String s) {
	char *p = gcnew((size_t)s.n + 1, T_RAW);
	memcpy(p, s.s, (size_t)s.n);
	p[s.n] = 0;
	return p;
}

Value strval(String s) {
	return (Value){MKH(KString, 0, s.n), {.s = s.s}};
}

String vstr(Value v) {
	return (String){v.w.s, (int64_t)VN(v)};
}

void itoa_(Buf *b, int64_t i) { bufprintf(b, "%lld", (long long)i); }

/* ---- Variables ---- */

static Value subst_get(Substr *s);
static bool subst_set(Substr *s, Value x);
static Value var_get(Var *v);
static bool var_set(Var *v, Value x);

Value derefvar(Value v) {
	switch (VKIND(v)) {
	case KRef:
		return *(Value *)v.w.p;
	case KListElem: {
		List *l = v.w.p;
		if ((int64_t)VN(v) < l->n)
			return *list_at(l, (int64_t)VN(v));
		return NULLV;
	}
	case KField:
		return ((Record *)v.w.p)->f[VN(v)];
	case KSubstr:
		return subst_get(v.w.p);
	case KVar:
		return var_get(v.w.p);
	}
	return v;
}

/* assignvar assigns x to the variable dst, reporting whether the
 * assignment succeeded and (in *isvar_) whether dst is a variable. */
bool assignvar(Value dst, Value x, bool *isvar_) {
	*isvar_ = true;
	switch (VKIND(dst)) {
	case KRef:
		*(Value *)dst.w.p = x;
		return true;
	case KListElem: {
		List *l = dst.w.p;
		if ((int64_t)VN(dst) < l->n)
			*list_at(l, (int64_t)VN(dst)) = x;
		return true;
	}
	case KField:
		((Record *)dst.w.p)->f[VN(dst)] = x;
		return true;
	case KSubstr:
		return subst_set(dst.w.p, x);
	case KVar:
		return var_set(dst.w.p, x);
	}
	*isvar_ = false;
	return false;
}

bool setvar(Value ref, Value v) {
	bool isv;
	bool ok = assignvar(ref, v, &isv);
	if (!isv)
		runerrv(111, ref);
	return ok;
}

/* Substrings */

static String subst_str(Substr *s) {
	Value d = deref(s->v);
	if (VKIND(d) != KString)
		runerrv(103, d);
	String str = vstr(d);
	if (s->pos - 1 + s->n > str.n)
		runerr(205);
	return str;
}

static Value subst_get(Substr *s) {
	String d = subst_str(s);
	return strval(slice(d, s->pos, s->pos + s->n));
}

static bool subst_set(Substr *s, Value x) {
	String src;
	if (!cvstr(x, &src))
		runerrv(103, x);
	String d = subst_str(s);
	String r = concat(concat(slice(d, 1, s->pos), src), slice(d, s->pos + s->n, d.n + 1));
	if (!setvar(s->v, strval(r)))
		return false;
	s->n = src.n;
	return true;
}

/* Keyword and table-element variables */

static Value var_get(Var *v) {
	switch (v->kind) {
	case VSubject:
		return strval(vm.subject);
	case VPos:
		return intval(vm.pos);
	case VRandom:
		return intval(vm.random);
	case VTrace:
		return intval(vm.trace);
	case VError:
		return intval(vm.errorkw);
	case VProgname:
		return strval(vm.progname);
	case VDump:
		return intval(vm.dump);
	case VValue:
		return *v->p;
	case VGraphics:
		return gfx_kwget((int)v->k.w.i);
	case VTableElem: {
		Value x;
		if (omap_get(&v->t->m, v->k, &x))
			return x;
		return v->t->dflt;
	}
	}
	return NULLV;
}

static bool var_set(Var *v, Value x) {
	switch (v->kind) {
	case VSubject:
		vm.subject = muststr(x);
		vm.pos = 1;
		return true;
	case VPos: {
		int64_t p;
		if (!cvpos(mustint(x), vm.subject.n, &p))
			return false;
		vm.pos = p;
		return true;
	}
	case VRandom:
		vm.random = mustint(x);
		return true;
	case VTrace:
		vm.trace = mustint(x);
		return true;
	case VError:
		vm.errorkw = mustint(x);
		return true;
	case VProgname:
		vm.progname = muststr(x);
		return true;
	case VDump:
		vm.dump = mustint(x);
		return true;
	case VValue:
		*v->p = x;
		return true;
	case VGraphics:
		return gfx_kwset((int)v->k.w.i, x);
	case VTableElem:
		omap_put(&v->t->m, v->k, x);
		return true;
	}
	return false;
}

/* ---- Conversions ---- */

static bool isdigit_(char c) { return c >= '0' && c <= '9'; }

static bool isspace_(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

/* parseradix parses digits of a radix literal "16rFF" (without sign). */
static bool parseradix(const char *s, size_t n, Value *out) {
	size_t i = 0;
	int base = 0;
	while (i < n && isdigit_(s[i])) {
		base = base * 10 + (s[i] - '0');
		if (base > 36)
			return false;
		i++;
	}
	if (i == 0 || i >= n || (s[i] != 'r' && s[i] != 'R') || base < 2)
		return false;
	i++;
	if (i >= n)
		return false;
	for (size_t k = i; k < n; k++) {
		char c = s[k];
		int d = isdigit_(c) ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
		if (d >= base)
			return false;
	}
	return bigparse(s + i, n - i, base, out);
}

/* strtonum converts a string to a number as Icon does (gcon's strToNum):
 * surrounding white space; an optional sign; then a radix integer, or
 * decimal digits with an optional fraction and exponent. */
bool str2num(String str, Value *out) {
	const char *s = str.s;
	size_t n = (size_t)str.n;
	while (n > 0 && isspace_(s[0]))
		s++, n--;
	while (n > 0 && isspace_(s[n - 1]))
		n--;
	bool neg = false;
	if (n > 0 && (s[0] == '+' || s[0] == '-')) {
		neg = s[0] == '-';
		s++, n--;
	}
	if (memchr(s, 'r', n) || memchr(s, 'R', n)) {
		if (n == 0 || !isdigit_(s[0]))
			return false;
		Value v;
		if (!parseradix(s, n, &v))
			return false;
		*out = neg ? neg_(v) : v;
		return true;
	}
	size_t i = 0, mant = 0;
	bool real = false;
	while (i < n && isdigit_(s[i]))
		i++, mant++;
	if (i < n && s[i] == '.') {
		real = true;
		for (i++; i < n && isdigit_(s[i]); i++)
			mant++;
	}
	if (mant == 0)
		return false;
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		real = true;
		i++;
		if (i < n && (s[i] == '+' || s[i] == '-'))
			i++;
		size_t exp = 0;
		for (; i < n && isdigit_(s[i]); i++)
			exp++;
		if (exp == 0)
			return false;
	}
	if (i != n)
		return false;
	char *t = xmalloc(n + 2);
	t[0] = neg ? '-' : '+';
	memcpy(t + 1, s, n);
	t[n + 1] = 0;
	bool ok;
	if (!real) {
		errno = 0;
		char *end;
		long long v = strtoll(t, &end, 10);
		if (errno == 0) {
			*out = intval(v);
			ok = true;
		} else
			ok = bigparse(t, n + 1, 10, out);
	} else {
		errno = 0;
		double f = strtod(t, NULL);
		/* Go's ParseFloat reports a range error for overflow (inf),
		 * not for underflow to zero or a denormal. */
		ok = !isinf(f);
		if (ok)
			*out = realval(f);
	}
	free(t);
	return ok;
}

bool cvnum(Value v, Value *out) {
	switch (VKIND(v)) {
	case KInt:
	case KReal:
	case KBig:
		*out = v;
		return true;
	case KString:
		return str2num(vstr(v), out);
	case KCset:
		return str2num(cset_string(v.w.p), out);
	}
	return false;
}

bool realtoint(double f, int64_t *out) {
	if (isnan(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0)
		return false;
	*out = (int64_t)f;
	return true;
}

bool cvint(Value v, int64_t *out) {
	Value n;
	if (!cvnum(v, &n))
		return false;
	switch (VKIND(n)) {
	case KReal:
		return realtoint(n.w.r, out);
	case KInt:
		*out = n.w.i;
		return true;
	}
	return false;
}

bool cvinteger(Value v, Value *out) {
	Value n;
	if (!cvnum(v, &n))
		return false;
	if (VKIND(n) == KReal)
		return realtointeger(n.w.r, out);
	*out = n;
	return true;
}

double tofloat(Value n) {
	switch (VKIND(n)) {
	case KInt:
		return (double)n.w.i;
	case KBig: {
		double f;
		if (!bigtoreal(n.w.p, &f))
			runerrv(102, n);
		return f;
	}
	}
	return n.w.r;
}

bool cvreal(Value v, double *out) {
	Value n;
	if (!cvnum(v, &n))
		return false;
	if (VKIND(n) == KBig)
		return bigtoreal(n.w.p, out);
	*out = tofloat(n);
	return true;
}

bool cvstr(Value v, String *out) {
	switch (VKIND(v)) {
	case KString:
		*out = vstr(v);
		return true;
	case KInt: {
		char buf[24];
		int k = snprintf(buf, sizeof buf, "%lld", (long long)v.w.i);
		*out = strcopy(buf, k);
		return true;
	}
	case KBig: {
		Buf b = {0};
		bigstr(&b, v.w.p);
		*out = bufstring(&b);
		return true;
	}
	case KReal: {
		Buf b = {0};
		rtos(&b, v.w.r);
		*out = bufstring(&b);
		return true;
	}
	case KCset:
		*out = cset_string(v.w.p);
		return true;
	}
	return false;
}

bool cvcset(Value v, Cset **out) {
	if (VKIND(v) == KCset) {
		*out = v.w.p;
		return true;
	}
	String s;
	if (cvstr(v, &s)) {
		*out = csetof(s);
		return true;
	}
	return false;
}

int64_t mustint(Value v) {
	int64_t i;
	if (!cvint(v, &i))
		runerrv(101, v);
	return i;
}

Value mustinteger(Value v) {
	Value i;
	if (!cvinteger(v, &i))
		runerrv(101, v);
	return i;
}

Value mustnum(Value v) {
	Value n;
	if (!cvnum(v, &n))
		runerrv(102, v);
	return n;
}

double mustreal(Value v) {
	double f;
	if (!cvreal(v, &f))
		runerrv(102, v);
	return f;
}

String muststr(Value v) {
	String s;
	if (!cvstr(v, &s))
		runerrv(103, v);
	return s;
}

Cset *mustcset(Value v) {
	Cset *c;
	if (!cvcset(v, &c))
		runerrv(104, v);
	return c;
}

List *mustlist(Value v) {
	if (VKIND(v) != KList)
		runerrv(108, v);
	return v.w.p;
}

bool cvpos(int64_t p, int64_t n, int64_t *out) {
	if (p <= 0)
		p += n + 1;
	if (p < 1 || p > n + 1)
		return false;
	*out = p;
	return true;
}

bool isinteger(Value v) { return VKIND(v) == KInt || VKIND(v) == KBig; }

int cmpinteger(Value x, Value y) {
	if (VKIND(x) == KInt && VKIND(y) == KInt)
		return (x.w.i > y.w.i) - (x.w.i < y.w.i);
	return bigcmp(x, y);
}

bool identical(Value x, Value y) {
	switch (VKIND(x)) {
	case KNull:
		return VKIND(y) == KNull;
	case KInt:
		return VKIND(y) == KInt && x.w.i == y.w.i;
	case KBig:
		return VKIND(y) == KBig && bigcmp(x, y) == 0;
	case KReal:
		return VKIND(y) == KReal && x.w.r == y.w.r;
	case KString: {
		if (VKIND(y) != KString)
			return false;
		String a = vstr(x), b = vstr(y);
		return a.n == b.n && memcmp(a.s, b.s, (size_t)a.n) == 0;
	}
	case KCset:
		return VKIND(y) == KCset && cset_equal(x.w.p, y.w.p);
	}
	return x.h == y.h && x.w.p == y.w.p;
}

const char *typename_(Value v) {
	switch (VKIND(v)) {
	case KNull: return "null";
	case KInt: case KBig: return "integer";
	case KString: return "string";
	case KList: return "list";
	case KProc: case KBuiltin: case KRecordType: return "procedure";
	case KCoexp: return "co-expression";
	case KExternal: return ext_typename(v);
	case KCset: return "cset";
	case KReal: return "real";
	case KRecord: {
		Record *r = v.w.p;
		return cstr(mkstr(r->t->name.s, (int64_t)r->t->name.n));
	}
	case KFile: return ((File *)v.w.p)->gfx ? "window" : "file";
	case KTable: return "table";
	case KSet: return "set";
	}
	return "unknown";
}

/* bufstring returns the bytes of b as a string in the heap, and frees b. */
String bufstring(Buf *b) {
	String s = b->n == 0 ? mkstr("", 0) : strcopy(b->b, (int64_t)b->n);
	free(b->b);
	*b = (Buf){0};
	return s;
}

void sbgrow_(SB *s, size_t k) {
	size_t cap = s->cap * 2;
	if (cap < s->n + k)
		cap = s->n + k;
	if (cap < 32)
		cap = 32;
	char *b = gcraw(cap);
	if (s->n)
		memcpy(b, s->b, s->n);
	s->b = b;
	s->cap = cap;
}

String sbstring(SB *s) {
	if (s->n == 0)
		return mkstr("", 0);
	return mkstr(s->b, (int64_t)s->n);
}
