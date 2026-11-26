/*
 * Arithmetic, comparison and the numeric functions (gcon's real.go,
 * bigint.go, ops.go), following iconx's oarith.r.
 */
#include "vm.h"

#include <math.h>
#include <string.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)

bool addint(int64_t x, int64_t y, int64_t *r) { return !__builtin_add_overflow(x, y, r); }
bool subint(int64_t x, int64_t y, int64_t *r) { return !__builtin_sub_overflow(x, y, r); }
bool mulint(int64_t x, int64_t y, int64_t *r) { return !__builtin_mul_overflow(x, y, r); }

static double realresult(double f) {
	if (isinf(f) || isnan(f))
		runerr(204);
	return f;
}

enum { AAdd, ASub, AMul, ADiv, AMod };

static Value intarith(int op, Value x, Value y) {
	if (VKIND(x) == KInt && VKIND(y) == KInt) {
		int64_t r, a = x.w.i, b = y.w.i;
		switch (op) {
		case AAdd:
			if (addint(a, b, &r))
				return intval(r);
			break;
		case ASub:
			if (subint(a, b, &r))
				return intval(r);
			break;
		case AMul:
			if (mulint(a, b, &r))
				return intval(r);
			break;
		case ADiv:
			if (b == 0)
				runerr(201);
			if (!(a == INT64_MIN && b == -1))
				return intval(a / b);
			break;
		case AMod:
			if (b == 0)
				runerr(202);
			if (b == -1)
				return intval(0);
			return intval(a % b);
		}
	}
	switch (op) {
	case AAdd: return bigadd(x, y);
	case ASub: return bigsub(x, y);
	case AMul: return bigmul(x, y);
	case ADiv: return bigdiv(x, y);
	default: return bigmod(x, y);
	}
}

static bool arith(int op, Value *a, Value *out) {
	Value x = mustnum(a[0]), y = mustnum(a[1]);
	if (isinteger(x) && isinteger(y)) {
		*out = intarith(op, x, y);
		return true;
	}
	double fx = tofloat(x), fy = tofloat(y), r;
	switch (op) {
	case AAdd: r = fx + fy; break;
	case ASub: r = fx - fy; break;
	case AMul: r = fx * fy; break;
	case ADiv:
		if (fy == 0)
			runerr(204);
		r = fx / fy;
		break;
	default:
		if (fy == 0)
			runerr(204);
		r = fmod(fx, fy);
	}
	*out = realval(realresult(r));
	return true;
}

FN(o_add) { (void)b; (void)n; return arith(AAdd, a, out); }
FN(o_sub) { (void)b; (void)n; return arith(ASub, a, out); }
FN(o_mul) { (void)b; (void)n; return arith(AMul, a, out); }
FN(o_div) { (void)b; (void)n; return arith(ADiv, a, out); }
FN(o_mod) { (void)b; (void)n; return arith(AMod, a, out); }

/* ripow raises a real to an integral power, step for step as iconx's. */
static double ripow(double r, int64_t n) {
	if (r == 0 && n <= 0)
		runerr(204);
	double ret = 1.0;
	if (n < 0) {
		n = -1 - n;
		r = 1 / r;
		ret = r;
	}
	while (n > 0) {
		if (n & 1)
			ret *= r;
		r *= r;
		n >>= 1;
	}
	return realresult(ret);
}

static Value intpow(Value x, Value y) {
	int ys = bigsign(y);
	bool x0 = VKIND(x) == KInt && x.w.i == 0;
	bool x1 = VKIND(x) == KInt && x.w.i == 1;
	bool xm1 = VKIND(x) == KInt && x.w.i == -1;
	if (x0 && ys <= 0)
		runerr(204);
	if (ys == 0 || x1)
		return intval(1);
	if (xm1)
		return intval(bigbit0(y) ? -1 : 1);
	if (ys < 0 || x0)
		return intval(0);
	if (VKIND(y) != KInt)
		runerr(307);
	return bigpow(x, y.w.i);
}

FN(o_pow) {
	(void)b;
	(void)n;
	Value x = mustnum(a[0]), y = mustnum(a[1]);
	if (isinteger(y)) {
		if (isinteger(x)) {
			*out = intpow(x, y);
			return true;
		}
		if (VKIND(y) == KInt)
			*out = realval(ripow(x.w.r, y.w.i));
		else
			*out = realval(realpowbig(x.w.r, y));
		return true;
	}
	double fx = tofloat(x), fy = tofloat(y);
	if (fx == 0 && fy < 0)
		runerr(204);
	if (fx < 0)
		runerr(206);
	*out = realval(realresult(pow(fx, fy)));
	return true;
}

enum { CLT, CLE, CGT, CGE, CEQ, CNE };

static bool cmpok(int op, int c) {
	switch (op) {
	case CLT: return c < 0;
	case CLE: return c <= 0;
	case CGT: return c > 0;
	case CGE: return c >= 0;
	case CEQ: return c == 0;
	}
	return c != 0;
}

static bool numcompare(int op, Value *a, Value *out) {
	Value x = mustnum(a[0]), y = mustnum(a[1]);
	if (isinteger(x) && isinteger(y)) {
		*out = y;
		return cmpok(op, cmpinteger(x, y));
	}
	double fx = tofloat(x), fy = tofloat(y);
	*out = realval(fy);
	return cmpok(op, (fx > fy) - (fx < fy));
}

FN(o_lt) { (void)b; (void)n; return numcompare(CLT, a, out); }
FN(o_le) { (void)b; (void)n; return numcompare(CLE, a, out); }
FN(o_gt) { (void)b; (void)n; return numcompare(CGT, a, out); }
FN(o_ge) { (void)b; (void)n; return numcompare(CGE, a, out); }
FN(o_eq) { (void)b; (void)n; return numcompare(CEQ, a, out); }
FN(o_ne) { (void)b; (void)n; return numcompare(CNE, a, out); }

static bool strcompare(int op, Value *a, Value *out) {
	String x = muststr(a[0]), y = muststr(a[1]);
	int c = memcmp(x.s, y.s, (size_t)(x.n < y.n ? x.n : y.n));
	c = c ? (c < 0 ? -1 : 1) : (x.n > y.n) - (x.n < y.n);
	*out = strval(y);
	return cmpok(op, c);
}

FN(o_slt) { (void)b; (void)n; return strcompare(CLT, a, out); }
FN(o_sle) { (void)b; (void)n; return strcompare(CLE, a, out); }
FN(o_sgt) { (void)b; (void)n; return strcompare(CGT, a, out); }
FN(o_sge) { (void)b; (void)n; return strcompare(CGE, a, out); }
FN(o_seq) { (void)b; (void)n; return strcompare(CEQ, a, out); }
FN(o_sne) { (void)b; (void)n; return strcompare(CNE, a, out); }

FN(o_ident) { (void)b; (void)n; *out = a[1]; return identical(a[0], a[1]); }
FN(o_nident) { (void)b; (void)n; *out = a[1]; return !identical(a[0], a[1]); }

FN(o_concat) {
	(void)b;
	(void)n;
	*out = strval(concat(muststr(a[0]), muststr(a[1])));
	return true;
}

FN(o_neg) {
	(void)b;
	(void)n;
	Value x = mustnum(a[0]);
	*out = VKIND(x) == KReal ? realval(-x.w.r) : neg_(x);
	return true;
}

FN(o_plus) {
	(void)b;
	(void)n;
	*out = mustnum(a[0]);
	return true;
}

FN(o_size) {
	(void)b;
	(void)n;
	Value x = a[0];
	switch (VKIND(x)) {
	case KList: *out = intval(((List *)x.w.p)->n); return true;
	case KCoexp: *out = intval(((Coexp *)x.w.p)->results); return true;
	case KCset: *out = intval(cset_size(x.w.p)); return true;
	case KRecord: *out = intval(((Record *)x.w.p)->t->nfields); return true;
	case KTable: *out = intval(((Table *)x.w.p)->m.live); return true;
	case KSet: *out = intval(((Set *)x.w.p)->m.live); return true;
	}
	String s;
	if (cvstr(x, &s)) {
		*out = intval(s.n);
		return true;
	}
	runerrv(112, x);
}

/* limit validates the limit of e \ n. */
FN(o_limit) {
	(void)b;
	(void)n;
	int64_t k = mustint(a[0]);
	if (k < 0)
		runerrv(205, intval(k));
	*out = intval(k);
	return k > 0;
}

/* ---- Functions ---- */

static bool mathfn(Value *a, int n, Value *out, double (*f)(double), int domain) {
	double x = mustreal(ARG(0));
	if ((domain == 1 && x < 0) || (domain == 2 && (x < -1 || x > 1)))
		runerrv(205, a[0]);
	*out = realval(f(x));
	return true;
}

static double dtor(double x) { return x * M_PI / 180; }
static double rtod(double x) { return x * 180 / M_PI; }

FN(f_sqrt) { (void)b; return mathfn(a, n, out, sqrt, 1); }
FN(f_exp) { (void)b; return mathfn(a, n, out, exp, 0); }
FN(f_sin) { (void)b; return mathfn(a, n, out, sin, 0); }
FN(f_cos) { (void)b; return mathfn(a, n, out, cos, 0); }
FN(f_tan) { (void)b; return mathfn(a, n, out, tan, 0); }
FN(f_asin) { (void)b; return mathfn(a, n, out, asin, 2); }
FN(f_acos) { (void)b; return mathfn(a, n, out, acos, 2); }
FN(f_dtor) { (void)b; return mathfn(a, n, out, dtor, 0); }
FN(f_rtod) { (void)b; return mathfn(a, n, out, rtod, 0); }

FN(f_atan) {
	(void)b;
	double y = mustreal(ARG(0));
	if (ISNULL(ARG(1)))
		*out = realval(atan(y));
	else
		*out = realval(atan2(y, mustreal(a[1])));
	return true;
}

FN(f_log) {
	(void)b;
	double x = mustreal(ARG(0));
	if (x <= 0)
		runerrv(205, a[0]);
	if (ISNULL(ARG(1))) {
		*out = realval(log(x));
		return true;
	}
	double base = mustreal(a[1]);
	if (base <= 1)
		runerrv(205, a[1]);
	*out = realval(log(x) / log(base));
	return true;
}

FN(f_abs) {
	(void)b;
	Value x = mustnum(ARG(0));
	if (VKIND(x) == KReal)
		*out = realval(fabs(x.w.r));
	else
		*out = bigsign(x) < 0 ? neg_(x) : x;
	return true;
}

FN(f_real) {
	(void)b;
	double f;
	if (!cvreal(ARG(0), &f))
		return false;
	*out = realval(f);
	return true;
}

FN(f_numeric) { (void)b; return cvnum(ARG(0), out); }
FN(f_integer) { (void)b; return cvinteger(ARG(0), out); }

static bool bitwise(Value *a, int n, Value *out, int op) {
	Value x = mustinteger(ARG(0)), y = mustinteger(ARG(1));
	if (VKIND(x) == KInt && VKIND(y) == KInt) {
		int64_t p = x.w.i, q = y.w.i;
		*out = intval(op == '&' ? p & q : op == '|' ? p | q : p ^ q);
		return true;
	}
	*out = op == '&' ? bigand(x, y) : op == '|' ? bigor(x, y) : bigxor(x, y);
	return true;
}

FN(f_iand) { (void)b; return bitwise(a, n, out, '&'); }
FN(f_ior) { (void)b; return bitwise(a, n, out, '|'); }
FN(f_ixor) { (void)b; return bitwise(a, n, out, '^'); }

FN(f_icom) {
	(void)b;
	Value x = mustinteger(ARG(0));
	*out = VKIND(x) == KInt ? intval(~x.w.i) : bignot(x);
	return true;
}

FN(f_ishift) {
	(void)b;
	Value x = mustinteger(ARG(0)), y = mustinteger(ARG(1));
	if (VKIND(y) != KInt)
		runerrv(101, y);
	int64_t j = y.w.i;
	if (VKIND(x) == KInt && j <= 0) {
		if (j <= -64)
			*out = intval(x.w.i < 0 ? -1 : 0);
		else
			*out = intval(x.w.i >> -j);
		return true;
	}
	if (j > ((int64_t)1 << 32))
		runerr(307);
	*out = bigshift(x, j);
	return true;
}

static Builtin ops[] = {
	{.name = "+", .key = "+", .fn = o_add, .intop = IntAdd},
	{.name = "-", .key = "-", .fn = o_sub, .intop = IntSub},
	{.name = "*", .key = "*", .fn = o_mul, .intop = IntMul},
	{.name = "/", .key = "/", .fn = o_div},
	{.name = "%", .key = "%", .fn = o_mod},
	{.name = "^", .key = "^", .fn = o_pow},
	{.name = "<", .key = "<", .fn = o_lt, .intop = IntLT},
	{.name = "<=", .key = "<=", .fn = o_le, .intop = IntLE},
	{.name = ">", .key = ">", .fn = o_gt, .intop = IntGT},
	{.name = ">=", .key = ">=", .fn = o_ge, .intop = IntGE},
	{.name = "=", .key = "=", .fn = o_eq, .intop = IntEQ},
	{.name = "~=", .key = "~=", .fn = o_ne, .intop = IntNE},
	{.name = "==", .key = "==", .fn = o_seq},
	{.name = "~==", .key = "~==", .fn = o_sne},
	{.name = "<<", .key = "<<", .fn = o_slt},
	{.name = "<<=", .key = "<<=", .fn = o_sle},
	{.name = ">>", .key = ">>", .fn = o_sgt},
	{.name = ">>=", .key = ">>=", .fn = o_sge},
	{.name = "===", .key = "===", .fn = o_ident, .intop = OpIdent},
	{.name = "~===", .key = "~===", .fn = o_nident, .intop = OpNotIdent},
	{.name = "||", .key = "||", .fn = o_concat},
	{.name = "-", .key = "u-", .fn = o_neg, .intop = OpNeg},
	{.name = "+", .key = "u+", .fn = o_plus, .intop = OpPlus},
	{.name = "*", .key = "u*", .fn = o_size, .intop = OpSize},
	{.name = "limit", .key = "limit", .fn = o_limit},
};

static Builtin fns[] = {
	{.name = "sqrt", .fn = f_sqrt}, {.name = "exp", .fn = f_exp},
	{.name = "sin", .fn = f_sin}, {.name = "cos", .fn = f_cos},
	{.name = "tan", .fn = f_tan}, {.name = "asin", .fn = f_asin},
	{.name = "acos", .fn = f_acos}, {.name = "dtor", .fn = f_dtor},
	{.name = "rtod", .fn = f_rtod}, {.name = "atan", .fn = f_atan},
	{.name = "log", .fn = f_log}, {.name = "abs", .fn = f_abs},
	{.name = "real", .fn = f_real}, {.name = "numeric", .fn = f_numeric},
	{.name = "integer", .fn = f_integer}, {.name = "iand", .fn = f_iand},
	{.name = "ior", .fn = f_ior}, {.name = "ixor", .fn = f_ixor},
	{.name = "icom", .fn = f_icom}, {.name = "ishift", .fn = f_ishift},
};

void num_init(void) {
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
}
