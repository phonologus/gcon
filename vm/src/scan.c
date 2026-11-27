/*
 * String scanning and the string analysis functions, as iconx's.
 * &subject and &pos are global, as in iconx. Positions are bytes.
 */
#include "vm.h"

#include <string.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)
#define GENFN(f) static Gen *f(Builtin *b, Value *a, int n)
#define RESTFN(f) static bool f(Builtin *b, Value *a, int n, Value *out, int64_t *old)

/* A saved environment is held only in its scan's temporary, slot (it is
 * never copied: co-expressions copy locals, not temporaries), so it is
 * exchanged in place, and entering a scan reuses the environment the
 * scan's last run left there, which ended with it. */
void scanenter(Value v, Value *slot) {
	String s;
	if (!cvstr(v, &s))
		runerrv(103, v);
	ScanEnv *e = VKIND(*slot) == KScanEnv ? slot->w.p : gcnew(sizeof *e, T_SCANENV);
	e->subject = vm.subject;
	e->pos = vm.pos;
	vm.subject = s;
	vm.pos = 1;
	*slot = ptrval(KScanEnv, e);
}

void scanswap(Value *ev) {
	ScanEnv *e = ev->w.p, t = *e;
	e->subject = vm.subject;
	e->pos = vm.pos;
	vm.subject = t.subject;
	vm.pos = t.pos;
}

/* ---- Generators used here and elsewhere ---- */

static bool failnext(Gen *g, Value *out) {
	(void)g;
	(void)out;
	return false;
}

Gen failgen = {failnext, NULL};

typedef struct Restorer {
	Gen g;
	Value v;
	int64_t old;
	bool done;
} Restorer;

static bool restnext(Gen *gg, Value *out) {
	Restorer *g = (Restorer *)gg;
	if (g->done) {
		vm.pos = g->old;
		return false;
	}
	g->done = true;
	*out = g->v;
	return true;
}

static void restmark(Gen *g) { markvalue(((Restorer *)g)->v); }

Gen *newrestorer(Value v, int64_t old) {
	Restorer *g = gcnew(sizeof *g, T_GEN);
	g->g.next = restnext;
	g->g.mark = restmark;
	g->v = v;
	g->old = old;
	return &g->g;
}

/* tabto sets &pos to p, giving the substring passed over and the old
 * &pos, which resumption restores. */
bool tabto(int64_t p, Value *out, int64_t *old) {
	*old = vm.pos;
	int64_t i = vm.pos, j = p;
	if (i > j) {
		int64_t t = i;
		i = j;
		j = t;
	}
	*out = strval(slice(vm.subject, i, j));
	vm.pos = p;
	return true;
}

/* scanargs converts the subject and range arguments a[k..k+2] as iconx's
 * str_anal: s defaults to &subject, and then i to &pos, else to 1; j to
 * the end. It fails if a position is out of range. */
static bool scanargs(Value *a, int n, int k, String *s, int64_t *i, int64_t *j) {
	Value sv = ARG(k), iv = ARG(k + 1), jv = ARG(k + 2);
	bool inull = ISNULL(iv);
	if (ISNULL(sv)) {
		*s = vm.subject;
		if (inull)
			*i = vm.pos;
	} else {
		*s = muststr(sv);
		if (inull)
			*i = 1;
	}
	int64_t len = s->n;
	if (!inull && !cvpos(mustint(iv), len, i))
		return false;
	if (ISNULL(jv))
		*j = len + 1;
	else {
		if (!cvpos(mustint(jv), len, j))
			return false;
		if (*i > *j) {
			int64_t t = *i;
			*i = *j;
			*j = t;
		}
	}
	return true;
}

static Cset *csetarg(Value *a, int n, int i, Cset *d) {
	if (ISNULL(ARG(i)))
		return d;
	return mustcset(a[i]);
}

static bool matches(String s, int64_t off, int64_t end, String t) {
	return off + t.n <= end && memcmp(s.s + off, t.s, (size_t)t.n) == 0;
}

/* ---- Keywords and operators ---- */

static Var subjectvar = {VSubject, 0, 0, {0, {0}}};
static Var posvar = {VPos, 0, 0, {0, {0}}};

FN(k_subject) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &subjectvar); return true; }
FN(k_pos) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &posvar); return true; }

/* =s is tab(match(s)). */
RESTFN(o_tabmatch) {
	(void)b;
	(void)n;
	String t = muststr(a[0]);
	if (!matches(vm.subject, vm.pos - 1, vm.subject.n, t))
		return false;
	return tabto(vm.pos + t.n, out, old);
}

FN(o_compl) {
	(void)b;
	(void)n;
	*out = ptrval(KCset, complement(mustcset(a[0])));
	return true;
}

/* ---- Functions ---- */

FN(f_cset) {
	(void)b;
	Cset *c;
	if (!cvcset(ARG(0), &c))
		return false;
	*out = ptrval(KCset, c);
	return true;
}

RESTFN(f_tab) {
	(void)b;
	int64_t p;
	if (!cvpos(mustint(ARG(0)), vm.subject.n, &p))
		return false;
	return tabto(p, out, old);
}

RESTFN(f_move) {
	(void)b;
	int64_t k = mustint(ARG(0));
	if (k < 1 - vm.pos || k > vm.subject.n + 1 - vm.pos)
		return false;
	return tabto(vm.pos + k, out, old);
}

FN(f_pos) {
	(void)b;
	int64_t p;
	bool ok = cvpos(mustint(ARG(0)), vm.subject.n, &p);
	*out = intval(p);
	return ok && p == vm.pos;
}

FN(f_match) {
	(void)b;
	String s;
	int64_t i, j;
	if (!scanargs(a, n, 1, &s, &i, &j))
		return false;
	String t = muststr(ARG(0));
	*out = intval(i + t.n);
	return matches(s, i - 1, j - 1, t);
}

typedef struct FindGen {
	Gen g;
	String s, t;
	int64_t off, end;
} FindGen;

static bool findnext(Gen *gg, Value *out) {
	FindGen *g = (FindGen *)gg;
	if (g->off > g->end)
		return false;
	const char *p = g->t.n == 0 ? g->s.s + g->off
				    : memmem(g->s.s + g->off, (size_t)(g->end - g->off), g->t.s, (size_t)g->t.n);
	if (p) {
		int64_t k = p - g->s.s;
		*out = intval(k + 1);
		g->off = k + 1;
		return true;
	}
	g->off = g->end + 1;
	return false;
}

static void findmark(Gen *gg) {
	FindGen *g = (FindGen *)gg;
	markptr(g->s.s);
	markptr(g->t.s);
}

GENFN(f_find) {
	(void)b;
	String s;
	int64_t i, j;
	if (!scanargs(a, n, 1, &s, &i, &j))
		return &failgen;
	FindGen *g = newgen(sizeof *g, findnext);
	g->g.next = findnext;
	g->g.mark = findmark;
	g->t = muststr(ARG(0));
	g->s = s;
	g->off = i - 1;
	g->end = j - 1;
	return &g->g;
}

typedef struct UptoGen {
	Gen g;
	Cset *c;
	String s;
	int64_t off, end;
} UptoGen;

static bool uptonext(Gen *gg, Value *out) {
	UptoGen *g = (UptoGen *)gg;
	const Cset *c = g->c;
	const uint8_t *s = (const uint8_t *)g->s.s;
	for (int64_t o = g->off; o < g->end; o++)
		if (cset_has(c, s[o])) {
			g->off = o + 1;
			*out = intval(o + 1);
			return true;
		}
	g->off = g->end;
	return false;
}

static void uptomark(Gen *gg) {
	UptoGen *g = (UptoGen *)gg;
	markptr(g->s.s);
	markvalue(ptrval(KCset, g->c));
}

GENFN(f_upto) {
	(void)b;
	Cset *c = mustcset(ARG(0));
	String s;
	int64_t i, j;
	if (!scanargs(a, n, 1, &s, &i, &j))
		return &failgen;
	UptoGen *g = newgen(sizeof *g, uptonext);
	g->g.next = uptonext;
	g->g.mark = uptomark;
	g->c = c;
	g->s = s;
	g->off = i - 1;
	g->end = j - 1;
	return &g->g;
}

FN(f_many) {
	(void)b;
	Cset *c = mustcset(ARG(0));
	String s;
	int64_t i, j;
	if (!scanargs(a, n, 1, &s, &i, &j))
		return false;
	const uint8_t *p = (const uint8_t *)s.s;
	int64_t off = i - 1, end = j - 1;
	while (off < end && cset_has(c, p[off]))
		off++;
	*out = intval(off + 1);
	return off > i - 1;
}

FN(f_any) {
	(void)b;
	Cset *c = mustcset(ARG(0));
	String s;
	int64_t i, j;
	if (!scanargs(a, n, 1, &s, &i, &j) || i >= j)
		return false;
	*out = intval(i + 1);
	return cset_has(c, (uint8_t)s.s[i - 1]);
}

typedef struct BalGen {
	Gen g;
	Cset *c1, *c2, *c3;
	String s;
	int64_t off, end, cnt;
} BalGen;

static bool balnext(Gen *gg, Value *out) {
	BalGen *g = (BalGen *)gg;
	while (g->off < g->end) {
		int64_t o = g->off++;
		uint8_t ch = (uint8_t)g->s.s[o];
		bool suspend = g->cnt == 0 && cset_has(g->c1, ch);
		if (cset_has(g->c2, ch))
			g->cnt++;
		else if (cset_has(g->c3, ch))
			g->cnt--;
		if (suspend) {
			if (g->cnt < 0)
				g->end = g->off;
			*out = intval(o + 1);
			return true;
		}
		if (g->cnt < 0)
			return false;
	}
	return false;
}

static Cset *lparen, *rparen;

static void balmark(Gen *gg) {
	BalGen *g = (BalGen *)gg;
	markptr(g->s.s);
	markvalue(ptrval(KCset, g->c1));
	markvalue(ptrval(KCset, g->c2));
	markvalue(ptrval(KCset, g->c3));
}

GENFN(f_bal) {
	(void)b;
	String s;
	int64_t i, j;
	bool ok = scanargs(a, n, 3, &s, &i, &j);
	Cset *c1 = csetarg(a, n, 0, CsetAll);
	Cset *c2 = csetarg(a, n, 1, lparen);
	Cset *c3 = csetarg(a, n, 2, rparen);
	if (!ok)
		return &failgen;
	BalGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = balnext;
	g->g.mark = balmark;
	g->c1 = c1, g->c2 = c2, g->c3 = c3;
	g->s = s;
	g->off = i - 1;
	g->end = j - 1;
	return &g->g;
}

static Builtin ops[] = {
	{.name = "&subject", .key = "&subject", .fn = k_subject},
	{.name = "&pos", .key = "&pos", .fn = k_pos},
	{.name = "=", .key = "u=", .restore = o_tabmatch},
	{.name = "~", .key = "u~", .fn = o_compl},
};

static Builtin fns[] = {
	{.name = "cset", .fn = f_cset},
	{.name = "tab", .restore = f_tab},
	{.name = "move", .restore = f_move},
	{.name = "pos", .fn = f_pos},
	{.name = "match", .fn = f_match},
	{.name = "find", .gen = f_find},
	{.name = "upto", .gen = f_upto},
	{.name = "many", .fn = f_many},
	{.name = "any", .fn = f_any},
	{.name = "bal", .gen = f_bal},
};

void scan_init(void) {
	lparen = csetfrom("(", 1);
	rparen = csetfrom(")", 1);
	gcroot((void **)&lparen);
	gcroot((void **)&rparen);
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
}
