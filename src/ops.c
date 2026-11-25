/*
 * Subscripts, sections, !x, ?x, to-by, string invocation's operators,
 * co-expression keywords (gcon's ops.go, invoke.go, random.go,
 * coexpr.go, substr.go, record.go).
 */
#include "vm.h"

#include <string.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)
#define GENFN(f) static Gen *f(Builtin *b, Value *a, int n)

/* strvar reports whether x, passed as a variable, is a variable holding
 * a string: only then do subscripts make substring variables. */
static bool strvar(Value x) {
	return isvar(x) && VKIND(deref(x)) == KString;
}

static Value newsubstr(Value v, int64_t pos, int64_t len) {
	Substr *s = gcnew(sizeof *s, T_SUBSTR);
	if (VKIND(v) == KSubstr) {
		Substr *o = v.w.p;
		s->v = o->v;
		s->pos = o->pos + pos - 1;
	} else {
		s->v = v;
		s->pos = pos;
	}
	s->n = len;
	return ptrval(KSubstr, s);
}

static bool recordindex(Record *r, Value x, Value *out) {
	if (VKIND(x) == KString) {
		String s = vstr(x);
		Value num;
		if (!str2num(s, &num)) {
			int i = recfield(r->t, (Str){s.s, (size_t)s.n});
			*out = idxval(KField, r, i < 0 ? 0 : i);
			return i >= 0;
		}
	}
	int64_t i = mustint(x), k = r->t->nfields;
	if (i <= 0)
		i += k + 1;
	if (i < 1 || i > k)
		return false;
	*out = idxval(KField, r, i - 1);
	return true;
}

FN(o_index) {
	(void)b;
	(void)n;
	Value r = a[0], x = deref(r);
	switch (VKIND(x)) {
	case KRecord:
		return recordindex(x.w.p, a[1], out);
	case KTable:
		*out = tableelem(x.w.p, a[1]);
		return true;
	case KList: {
		List *l = x.w.p;
		int64_t i = mustint(a[1]);
		if (i <= 0)
			i += l->n + 1;
		if (i < 1 || i > l->n)
			return false;
		*out = idxval(KListElem, l, i - 1);
		return true;
	}
	}
	String s;
	if (!cvstr(x, &s))
		runerrv(114, x);
	int64_t i;
	if (!cvpos(mustint(a[1]), s.n, &i) || i > s.n)
		return false;
	if (strvar(r))
		*out = newsubstr(r, i, 1);
	else
		*out = strval(slice(s, i, i + 1));
	return true;
}

FN(o_section) {
	(void)b;
	(void)n;
	Value r = a[0], x = deref(r);
	int64_t len;
	String s;
	bool islist = VKIND(x) == KList;
	if (islist)
		len = ((List *)x.w.p)->n;
	else {
		if (!cvstr(x, &s))
			runerrv(110, x);
		len = s.n;
	}
	int64_t i, j;
	bool ok1 = cvpos(mustint(a[1]), len, &i);
	bool ok2 = cvpos(mustint(a[2]), len, &j);
	if (!ok1 || !ok2)
		return false;
	if (i > j) {
		int64_t t = i;
		i = j;
		j = t;
	}
	if (islist) {
		List *l = x.w.p, *c = newlist(j - i);
		memcpy(c->e, list_at(l, i - 1), (size_t)(j - i) * sizeof(Value));
		*out = ptrval(KList, c);
		return true;
	}
	if (strvar(r))
		*out = newsubstr(r, i, j - i);
	else
		*out = strval(slice(s, i, j));
	return true;
}

/* i to j by k */
typedef struct ToGen {
	Gen g;
	int64_t i, j, by;
	bool started;
} ToGen;

static bool tonext(Gen *gg, Value *out) {
	ToGen *g = (ToGen *)gg;
	if (g->started) {
		int64_t k;
		if (!addint(g->i, g->by, &k))
			return false;
		g->i = k;
	}
	g->started = true;
	if ((g->by > 0 && g->i > g->j) || (g->by < 0 && g->i < g->j))
		return false;
	*out = intval(g->i);
	return true;
}

GENFN(o_to) {
	(void)b;
	ToGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = tonext;
	g->i = mustint(a[0]);
	g->j = mustint(a[1]);
	g->by = 1;
	if (n > 2)
		g->by = mustint(a[2]);
	if (g->by == 0)
		runerrv(211, intval(g->by));
	return &g->g;
}

/* !x */
typedef struct IdxGen {
	Gen g;
	void *p;
	Value v;
	int64_t i;
} IdxGen;

static bool listnext(Gen *gg, Value *out) {
	IdxGen *g = (IdxGen *)gg;
	List *l = g->p;
	if (g->i >= l->n)
		return false;
	*out = idxval(KListElem, l, g->i++);
	return true;
}

static bool recnext(Gen *gg, Value *out) {
	IdxGen *g = (IdxGen *)gg;
	Record *r = g->p;
	if (g->i >= r->t->nfields)
		return false;
	*out = idxval(KField, r, g->i++);
	return true;
}

static bool strnext(Gen *gg, Value *out) {
	IdxGen *g = (IdxGen *)gg;
	String s = vstr(g->v);
	if (g->i >= s.n)
		return false;
	int w = width(s, g->i);
	g->i += w;
	*out = strval(slice(s, g->i - w + 1, g->i + 1));
	return true;
}

static bool strvarnext(Gen *gg, Value *out) {
	IdxGen *g = (IdxGen *)gg;
	Value d = deref(g->v);
	if (VKIND(d) != KString)
		runerrv(103, d);
	String s = vstr(d);
	if (g->i >= s.n)
		return false;
	int w = width(s, g->i);
	g->i += w;
	*out = newsubstr(g->v, g->i - w + 1, w);
	return true;
}

static bool linenext(Gen *gg, Value *out) {
	IdxGen *g = (IdxGen *)gg;
	String s;
	if (!readline_(g->p, &s))
		return false;
	*out = strval(s);
	return true;
}

static void idxmark(Gen *gg) {
	IdxGen *g = (IdxGen *)gg;
	markptr(g->p);
	markvalue(g->v);
}

static Gen *idxgen(bool (*next)(Gen *, Value *), void *p, Value v) {
	IdxGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = next;
	g->g.mark = idxmark;
	g->p = p;
	g->v = v;
	return &g->g;
}

GENFN(o_bang) {
	(void)b;
	(void)n;
	if (strvar(a[0]))
		return idxgen(strvarnext, NULL, a[0]);
	Value x = deref(a[0]);
	switch (VKIND(x)) {
	case KList: return idxgen(listnext, x.w.p, NONE);
	case KRecord: return idxgen(recnext, x.w.p, NONE);
	case KFile: return idxgen(linenext, x.w.p, NONE);
	case KTable: return omapgen(&((Table *)x.w.p)->m, x.w.p, 0);
	case KSet: return omapgen(&((Set *)x.w.p)->m, NULL, 1);
	}
	String s;
	if (!cvstr(x, &s))
		runerrv(116, x);
	return idxgen(strnext, NULL, strval(s));
}

/* ---- ?x and &random, as iconx's ---- */

double randval(void) {
	vm.random = (int64_t)((1103515245 * (uint64_t)vm.random + 453816694) & 0x7fffffff);
	return 4.65661286e-10 * (double)vm.random;
}

static int64_t randindex(int64_t k) { return (int64_t)(randval() * (double)k); }

static bool randomchar(String s, Value *out) {
	int64_t k = runes(s);
	if (k == 0)
		return false;
	int64_t r = randindex(k);
	if (k == s.n) {
		vm.lastcharpos = r + 1;
		*out = strval(slice(s, r + 1, r + 2));
		return true;
	}
	int64_t i = 1;
	for (; r > 0; r--)
		i += width(s, i - 1);
	vm.lastcharpos = i;
	*out = strval(slice(s, i, i + width(s, i - 1)));
	return true;
}

static bool randomentry(OMap *m, Value *out) {
	if (m->live == 0)
		return false;
	int64_t k = randindex(m->live);
	for (int64_t i = 0; i < m->n; i++) {
		if (VKIND(m->keys[i]) == KNone)
			continue;
		if (k == 0) {
			*out = m->keys[i];
			return true;
		}
		k--;
	}
	return false;
}

FN(o_random) {
	(void)b;
	(void)n;
	if (strvar(a[0])) {
		Value c;
		if (!randomchar(vstr(deref(a[0])), &c))
			return false;
		*out = newsubstr(a[0], vm.lastcharpos, vstr(c).n);
		return true;
	}
	Value x = deref(a[0]);
	switch (VKIND(x)) {
	case KString:
		return randomchar(vstr(x), out);
	case KCset:
		return randomchar(cset_string(x.w.p), out);
	case KList: {
		List *l = x.w.p;
		if (l->n == 0)
			return false;
		*out = idxval(KListElem, l, randindex(l->n));
		return true;
	}
	case KRecord: {
		Record *r = x.w.p;
		if (r->t->nfields == 0)
			return false;
		*out = idxval(KField, r, randindex(r->t->nfields));
		return true;
	}
	case KTable: {
		Value k;
		if (!randomentry(&((Table *)x.w.p)->m, &k))
			return false;
		*out = tableelem(x.w.p, k);
		return true;
	}
	case KSet:
		return randomentry(&((Set *)x.w.p)->m, out);
	}
	Value k;
	if (!cvinteger(x, &k))
		runerrv(113, x);
	if (VKIND(k) == KBig) {
		*out = bigrand(k);
		return true;
	}
	if (k.w.i < 0)
		runerrv(205, x);
	if (k.w.i == 0) {
		*out = realval(randval());
		return true;
	}
	*out = intval((int64_t)(randval() * (double)k.w.i) + 1);
	return true;
}

static Var randomvar = {VRandom, 0, 0, {0, {0}}};
FN(k_random) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &randomvar); return true; }

/* ---- Operators for string invocation ---- */

FN(o_nassign) {
	(void)b;
	(void)n;
	*out = a[0];
	return setvar(a[0], deref(a[1]));
}

FN(o_nswap) {
	(void)b;
	(void)n;
	Value x = deref(a[0]), y = deref(a[1]);
	*out = a[0];
	return setvar(a[0], y) && setvar(a[1], x);
}

typedef struct RevGen {
	Gen g;
	Value vars[2], olds[2], news[2];
	int k;
	bool done;
} RevGen;

static void revmark(Gen *gg) {
	RevGen *g = (RevGen *)gg;
	for (int i = 0; i < 2; i++) {
		markvalue(g->vars[i]);
		markvalue(g->olds[i]);
		markvalue(g->news[i]);
	}
}

static bool revnext(Gen *gg, Value *out) {
	RevGen *g = (RevGen *)gg;
	if (g->done) {
		for (int i = 0; i < g->k; i++)
			setvar(g->vars[i], g->olds[i]);
		return false;
	}
	g->done = true;
	for (int i = 0; i < g->k; i++)
		if (!setvar(g->vars[i], g->news[i]))
			return false;
	*out = g->vars[0];
	return true;
}

GENFN(o_nrevassign) {
	(void)b;
	(void)n;
	RevGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = revnext;
	g->g.mark = revmark;
	g->k = 1;
	g->vars[0] = a[0];
	g->olds[0] = deref(a[0]);
	g->news[0] = deref(a[1]);
	return &g->g;
}

GENFN(o_nrevswap) {
	(void)b;
	(void)n;
	RevGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = revnext;
	g->g.mark = revmark;
	g->k = 2;
	Value x = deref(a[0]), y = deref(a[1]);
	g->vars[0] = a[0];
	g->vars[1] = a[1];
	g->olds[0] = x, g->olds[1] = y;
	g->news[0] = y, g->news[1] = x;
	return &g->g;
}

FN(o_null) { (void)b; (void)n; *out = a[0]; return ISNULL(a[0]); }
FN(o_nonnull) { (void)b; (void)n; *out = a[0]; return !ISNULL(a[0]); }
FN(o_dot) { (void)b; (void)n; *out = a[0]; return true; }

/* ---- Co-expressions ---- */

FN(o_refresh) {
	(void)b;
	(void)n;
	if (VKIND(a[0]) != KCoexp)
		runerrv(118, a[0]);
	Coexp *c = a[0].w.p;
	if (c == vm.main)
		runerrv(215, a[0]);
	Coexp *r = newcoexp(c->proc, c->entry, c->init);
	r->from = c->from;
	*out = ptrval(KCoexp, r);
	return true;
}

FN(k_current) { (void)b; (void)a; (void)n; *out = ptrval(KCoexp, vm.co); return true; }
FN(k_source) { (void)b; (void)a; (void)n; *out = ptrval(KCoexp, vm.co->activator); return true; }
FN(k_main) { (void)b; (void)a; (void)n; *out = ptrval(KCoexp, vm.main); return true; }

static Builtin ops[] = {
	{.name = "[]", .key = "[]", .fn = o_index, .reffirst = true},
	{.name = "[:]", .key = "[:]", .fn = o_section, .reffirst = true},
	{.name = "to", .key = "to", .gen = o_to},
	{.name = "!", .key = "u!", .gen = o_bang, .reffirst = true},
	{.name = "?", .key = "u?", .fn = o_random, .reffirst = true},
	{.name = "&random", .key = "&random", .fn = k_random},
	{.name = ":=", .key = "n:=", .fn = o_nassign, .noderef = true},
	{.name = ":=:", .key = "n:=:", .fn = o_nswap, .noderef = true},
	{.name = "<-", .key = "n<-", .gen = o_nrevassign, .noderef = true},
	{.name = "<->", .key = "n<->", .gen = o_nrevswap, .noderef = true},
	{.name = "/", .key = "u/", .fn = o_null},
	{.name = "\\", .key = "u\\", .fn = o_nonnull},
	{.name = ".", .key = "u.", .fn = o_dot},
	{.name = "^", .key = "u^", .fn = o_refresh},
	{.name = "&current", .key = "&current", .fn = k_current},
	{.name = "&source", .key = "&source", .fn = k_source},
	{.name = "&main", .key = "&main", .fn = k_main},
};

void ops_init(void) {
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
}
