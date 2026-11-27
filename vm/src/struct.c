/*
 * Lists, records, tables and sets (gcon's value.go, record.go,
 * structs.go, builtins.go): tables and sets are insertion-ordered maps
 * keyed by ===; traversal starts at a random entry, as gcon's does.
 */
#include "vm.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)
#define GENFN(f) static Gen *f(Builtin *b, Value *a, int n)

/* ---- Lists ---- */

List *newlist(int64_t n) {
	List *l = gcnew(sizeof *l, T_LIST);
	l->cap = n > 4 ? n : 4;
	l->e = gcnew((size_t)l->cap * sizeof(Value), T_VALUES);
	l->n = n;
	l->serial = ++vm.serial_lists;
	return l;
}

static void list_grow(List *l, int64_t front, int64_t back) {
	int64_t cap = l->n + front + back + l->n / 2 + 4;
	Value *e = gcnew((size_t)cap * sizeof(Value), T_VALUES);
	int64_t off = front + (l->n / 4) + 2;
	if (off + l->n + back > cap)
		off = front;
	memcpy(e + off, l->e + l->off, (size_t)l->n * sizeof(Value));
	l->e = e;
	l->off = off;
	l->cap = cap;
}

void list_append(List *l, Value v) {
	if (l->off + l->n >= l->cap)
		list_grow(l, 0, 1);
	l->e[l->off + l->n++] = v;
}

void list_pushfront(List *l, Value v) {
	if (l->off == 0)
		list_grow(l, 1, 0);
	l->e[--l->off] = v;
	l->n++;
}

/* ---- Records ---- */

Record *construct(RecordType *t, Value *a, int n) {
	Record *r = gcnew(sizeof *r + (size_t)t->nfields * sizeof(Value), T_RECORD);
	r->t = t;
	r->serial = ++t->serial;
	for (int i = 0; i < t->nfields; i++)
		r->f[i] = ARG(i);
	return r;
}

int recfield(RecordType *t, Str name) {
	for (int i = 0; i < t->nfields; i++)
		if (t->fields[i].n == name.n && memcmp(t->fields[i].s, name.s, name.n) == 0)
			return i;
	return -1;
}

/* ---- Hashing by === ---- */

static uint64_t mix(uint64_t x) {
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
}

static uint64_t hashbytes(const void *p, size_t n, uint64_t h) {
	const uint8_t *s = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ s[i]) * 0x100000001b3ULL;
	return h;
}

static uint64_t hashval(Value v) {
	switch (VKIND(v)) {
	case KNull:
		return 0x9e3779b97f4a7c15ULL;
	case KInt:
		return mix((uint64_t)v.w.i + 1);
	case KBig: {
		Big *b = v.w.p;
		return hashbytes(b->d, (size_t)b->n * 4, 0xcbf29ce484222325ULL + b->neg);
	}
	case KReal: {
		double r = v.w.r == 0 ? 0 : v.w.r;
		uint64_t u;
		memcpy(&u, &r, 8);
		return mix(u ^ 0x5555);
	}
	case KString: {
		String s = vstr(v);
		return hashbytes(s.s, (size_t)s.n, 0xcbf29ce484222325ULL);
	}
	case KCset: {
		Cset *c = v.w.p;
		return hashbytes(c->bits, sizeof c->bits, 0x1234);
	}
	case KUset: {
		Uset *c = v.w.p;
		uint64_t h = hashbytes(c->bits, sizeof c->bits, 0x4321);
		return hashbytes(c->ranges, (size_t)c->nranges * 8, h);
	}
	}
	return mix((uint64_t)(uintptr_t)v.w.p ^ (uint64_t)VKIND(v) << 56 ^ VN(v));
}

void omap_init(OMap *m) { memset(m, 0, sizeof *m); }

/* find returns the index slot for k, and the entry it holds or -1. */
static int64_t find(OMap *m, Value k, uint64_t h, int64_t *slot) {
	if (m->isize == 0) {
		*slot = -1;
		return -1;
	}
	int64_t mask = m->isize - 1, i = (int64_t)(h & (uint64_t)mask), tomb = -1;
	for (;;) {
		int32_t e = m->index[i];
		if (e == 0) {
			*slot = tomb >= 0 ? tomb : i;
			return -1;
		}
		if (e < 0) {
			if (tomb < 0)
				tomb = i;
		} else if (identical(m->keys[e - 1], k)) {
			*slot = i;
			return e - 1;
		}
		i = (i + 1) & mask;
	}
}

static void reindex(OMap *m, int64_t size) {
	m->isize = size;
	m->index = gcnew((size_t)size * sizeof(int32_t), T_RAW);
	int64_t mask = size - 1;
	for (int64_t e = 0; e < m->n; e++) {
		if (VKIND(m->keys[e]) == KNone)
			continue;
		int64_t i = (int64_t)(hashval(m->keys[e]) & (uint64_t)mask);
		while (m->index[i] != 0)
			i = (i + 1) & mask;
		m->index[i] = (int32_t)(e + 1);
	}
}

bool omap_get(OMap *m, Value k, Value *out) {
	int64_t slot, e = find(m, k, hashval(k), &slot);
	if (e < 0)
		return false;
	if (out)
		*out = m->vals[e];
	return true;
}

static void growentries(OMap *m) {
	int64_t cap = m->cap ? 2 * m->cap : 8;
	Value *k = gcnew((size_t)cap * sizeof(Value), T_VALUES), *v = gcnew((size_t)cap * sizeof(Value), T_VALUES);
	memcpy(k, m->keys, (size_t)m->n * sizeof(Value));
	memcpy(v, m->vals, (size_t)m->n * sizeof(Value));
	m->keys = k, m->vals = v, m->cap = cap; /* a traversal keeps the old ones alive */
}

void omap_put(OMap *m, Value k, Value v) {
	uint64_t h = hashval(k);
	int64_t slot, e = find(m, k, h, &slot);
	if (e >= 0) {
		m->vals[e] = v;
		return;
	}
	if ((m->n + 1) * 2 > m->isize) {
		reindex(m, m->isize ? m->isize * 2 : 16);
		find(m, k, h, &slot);
	}
	if (m->n == m->cap)
		growentries(m);
	m->keys[m->n] = k;
	m->vals[m->n] = v;
	m->index[slot] = (int32_t)(m->n + 1);
	m->n++;
	m->live++;
}

static void compact(OMap *m) {
	if (m->shared) {
		Value *k = gcnew((size_t)(m->live + 1) * sizeof(Value), T_VALUES), *v = gcnew((size_t)(m->live + 1) * sizeof(Value), T_VALUES);
		int64_t j = 0;
		for (int64_t i = 0; i < m->n; i++)
			if (VKIND(m->keys[i]) != KNone) {
				k[j] = m->keys[i];
				v[j++] = m->vals[i];
			}
		m->keys = k, m->vals = v, m->n = j, m->cap = m->live + 1, m->shared = false;
	} else {
		int64_t j = 0;
		for (int64_t i = 0; i < m->n; i++)
			if (VKIND(m->keys[i]) != KNone) {
				m->keys[j] = m->keys[i];
				m->vals[j++] = m->vals[i];
			}
		m->n = j;
	}
	m->dead = 0;
	reindex(m, m->isize);
}

void omap_del(OMap *m, Value k) {
	int64_t slot, e = find(m, k, hashval(k), &slot);
	if (e < 0)
		return;
	m->index[slot] = -1;
	m->live--;
	if (e == m->n - 1 && !m->shared) {
		m->n--;
		return;
	}
	m->keys[e] = m->vals[e] = NONE;
	m->dead++;
	if (m->dead > 16 && m->dead > m->n / 2)
		compact(m);
}

void omap_copy(OMap *dst, const OMap *src) {
	omap_init(dst);
	for (int64_t i = 0; i < src->n; i++)
		if (VKIND(src->keys[i]) != KNone)
			omap_put(dst, src->keys[i], src->vals[i]);
}

/* ---- Tables and sets ---- */

Table *newtable(Value dflt) {
	Table *t = gcnew(sizeof *t, T_TABLE);
	t->dflt = dflt;
	t->serial = ++vm.serial_tables;
	return t;
}

Set *newset(void) {
	Set *s = gcnew(sizeof *s, T_SET);
	s->serial = ++vm.serial_sets;
	return s;
}

Value tableelem(Table *t, Value k) {
	Var *v = gcnew(sizeof *v, T_VAR);
	v->kind = VTableElem;
	v->t = t;
	v->k = k;
	return ptrval(KVar, v);
}

static uint64_t rngstate;

static uint64_t rng(void) {
	if (rngstate == 0)
		rngstate = mix((uint64_t)time(NULL) ^ (uint64_t)getpid() << 20) | 1;
	rngstate ^= rngstate << 13;
	rngstate ^= rngstate >> 7;
	rngstate ^= rngstate << 17;
	return rngstate;
}

/* omapgen generates an omap's entries: mode 0 table elements (variables),
 * mode 1 keys, from a snapshot of the keys, starting at a random one. */
typedef struct OMapGen {
	Gen g;
	OMap *m;
	Table *t;
	int mode;
	Value *keys;
	int64_t n, i, start;
	bool started;
} OMapGen;

static bool omapnext(Gen *gg, Value *out) {
	OMapGen *g = (OMapGen *)gg;
	if (!g->started) {
		g->started = true;
		g->keys = g->m->keys;
		g->n = g->m->n;
		g->m->shared = true;
		if (g->n > 0)
			g->start = (int64_t)(rng() % (uint64_t)g->n);
	}
	while (g->i < g->n) {
		Value k = g->keys[(g->start + g->i) % g->n];
		g->i++;
		if (VKIND(k) == KNone)
			continue;
		if (omap_get(g->m, k, NULL)) {
			*out = g->mode == 0 ? tableelem(g->t, k) : k;
			return true;
		}
	}
	return false;
}

static void omapmark(Gen *gg) {
	OMapGen *g = (OMapGen *)gg;
	markptr(g->m); /* inside its table or set */
	markptr(g->t);
	markptr(g->keys);
}

Gen *omapgen(OMap *m, Table *t, int mode) {
	OMapGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = omapnext;
	g->g.mark = omapmark;
	g->m = m, g->t = t, g->mode = mode;
	return &g->g;
}

/* members returns the live keys of m, in order. */
static Value *members(const OMap *m, int64_t *np) {
	Value *out = gcnew((size_t)(m->live + 1) * sizeof(Value), T_VALUES);
	int64_t j = 0;
	for (int64_t i = 0; i < m->n; i++)
		if (VKIND(m->keys[i]) != KNone)
			out[j++] = m->keys[i];
	*np = j;
	return out;
}

static Set *setop(int op, Set *a, Set *b) {
	Set *r = newset();
	int64_t na, nb;
	Value *ma = members(&a->m, &na);
	for (int64_t i = 0; i < na; i++) {
		bool inb = omap_get(&b->m, ma[i], NULL);
		if (op == '+' || (op == '*') == inb)
			omap_put(&r->m, ma[i], NULLV);
	}
	if (op == '+') {
		Value *mb = members(&b->m, &nb);
		for (int64_t i = 0; i < nb; i++)
			omap_put(&r->m, mb[i], NULLV);
	}
	return r;
}

/* ---- Sorting ---- */

static int order(Value v) {
	switch (VKIND(v)) {
	case KNull: return 0;
	case KInt: case KBig: return 1;
	case KReal: return 2;
	case KString: return 3;
	case KCset: return 4;
	case KUset: return 5; /* after csets */
	case KFile: return 6;
	case KCoexp: return 7;
	case KProc: case KBuiltin: case KRecordType: return 8;
	case KList: return 9;
	case KSet: return 10;
	case KTable: return 11;
	case KRecord: return 12;
	case KExternal: return 13;
	}
	return 13;
}

static int cmpi(int64_t a, int64_t b) { return (a > b) - (a < b); }

static int cmpbytes(const char *a, int64_t na, const char *b, int64_t nb) {
	int c = memcmp(a, b, (size_t)(na < nb ? na : nb));
	if (c)
		return c < 0 ? -1 : 1;
	return cmpi(na, nb);
}

static Str procname(Value v) {
	switch (VKIND(v)) {
	case KProc: return ((Proc *)v.w.p)->name;
	case KBuiltin: { const char *s = ((Builtin *)v.w.p)->name; return (Str){s, strlen(s)}; }
	case KRecordType: return ((RecordType *)v.w.p)->name;
	}
	return (Str){"", 0};
}

int anycmp(Value x, Value y) {
	int ox = order(x), oy = order(y);
	if (ox != oy)
		return cmpi(ox, oy);
	switch (VKIND(x)) {
	case KInt: case KBig:
		return cmpinteger(x, y);
	case KReal:
		return (x.w.r > y.w.r) - (x.w.r < y.w.r);
	case KString: {
		String a = vstr(x), b = vstr(y);
		return cmpbytes(a.s, a.n, b.s, b.n);
	}
	case KCset: {
		String a = cset_string(x.w.p), b = cset_string(y.w.p);
		return cmpbytes(a.s, a.n, b.s, b.n);
	}
	case KUset: {
		String a = uset_string(x.w.p), b = uset_string(y.w.p);
		return cmpbytes(a.s, a.n, b.s, b.n);
	}
	case KFile: {
		File *a = x.w.p, *b = y.w.p;
		return cmpbytes(a->name.s, a->name.n, b->name.s, b->name.n);
	}
	case KCoexp: return cmpi(((Coexp *)x.w.p)->serial, ((Coexp *)y.w.p)->serial);
	case KExternal: return ext_cmp(x, y);
	case KList: return cmpi(((List *)x.w.p)->serial, ((List *)y.w.p)->serial);
	case KSet: return cmpi(((Set *)x.w.p)->serial, ((Set *)y.w.p)->serial);
	case KTable: return cmpi(((Table *)x.w.p)->serial, ((Table *)y.w.p)->serial);
	case KRecord: {
		Record *a = x.w.p, *b = y.w.p;
		int c = cmpbytes(a->t->name.s, (int64_t)a->t->name.n, b->t->name.s, (int64_t)b->t->name.n);
		return c ? c : cmpi(a->serial, b->serial);
	}
	}
	if (VKIND(x) == KProc || VKIND(x) == KBuiltin || VKIND(x) == KRecordType) {
		Str a = procname(x), b = procname(y);
		return cmpbytes(a.s, (int64_t)a.n, b.s, (int64_t)b.n);
	}
	return 0;
}

static bool nth(Value v, int64_t k, Value *out) {
	int64_t i;
	if (VKIND(v) == KRecord) {
		Record *r = v.w.p;
		if (cvpos(k, r->t->nfields, &i) && i <= r->t->nfields) {
			*out = r->f[i - 1];
			return true;
		}
	} else if (VKIND(v) == KList) {
		List *l = v.w.p;
		if (cvpos(k, l->n, &i) && i <= l->n) {
			*out = *list_at(l, i - 1);
			return true;
		}
	}
	return false;
}

static int64_t sortfield;

static int nthcmp(Value x, Value y) {
	if ((VKIND(x) == KRecord || VKIND(x) == KList) && order(x) == order(y)) {
		Value ex, ey;
		bool okx = nth(x, sortfield, &ex), oky = nth(y, sortfield, &ey);
		if (!okx && oky)
			return -1;
		if (okx && !oky)
			return 1;
		if (okx && oky) {
			int c = anycmp(ex, ey);
			if (c)
				return c;
		}
	}
	return anycmp(x, y);
}

/* msort sorts stably. */
static void msort(Value *v, int64_t n, int (*cmp)(Value, Value), Value *tmp) {
	if (n < 2)
		return;
	int64_t h = n / 2;
	msort(v, h, cmp, tmp);
	msort(v + h, n - h, cmp, tmp);
	int64_t i = 0, j = h, k = 0;
	while (i < h && j < n)
		tmp[k++] = cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
	while (i < h)
		tmp[k++] = v[i++];
	while (j < n)
		tmp[k++] = v[j++];
	memcpy(v, tmp, (size_t)n * sizeof(Value));
}

static void sortvalues(Value *v, int64_t n, int (*cmp)(Value, Value)) {
	Value *tmp = xmalloc((size_t)(n + 1) * sizeof(Value));
	msort(v, n, cmp, tmp);
	free(tmp);
}

static bool elements(Value v, Value **out, int64_t *np) {
	switch (VKIND(v)) {
	case KList: {
		List *l = v.w.p;
		*out = gcnew((size_t)(l->n + 1) * sizeof(Value), T_VALUES);
		memcpy(*out, list_at(l, 0), (size_t)l->n * sizeof(Value));
		*np = l->n;
		return true;
	}
	case KSet:
		*out = members(&((Set *)v.w.p)->m, np);
		return true;
	case KRecord: {
		Record *r = v.w.p;
		*out = gcnew((size_t)(r->t->nfields + 1) * sizeof(Value), T_VALUES);
		memcpy(*out, r->f, (size_t)r->t->nfields * sizeof(Value));
		*np = r->t->nfields;
		return true;
	}
	}
	return false;
}

static List *listof(Value *v, int64_t n) {
	List *l = newlist(n);
	if (n)
		memcpy(l->e, v, (size_t)n * sizeof(Value));
	return l;
}

/* ---- Functions ---- */

FN(f_list) {
	(void)b;
	int64_t k = 0;
	if (!ISNULL(ARG(0))) {
		k = mustint(a[0]);
		if (k < 0)
			runerrv(205, a[0]);
	}
	List *l = newlist(k);
	for (int64_t i = 0; i < k; i++)
		l->e[i] = ARG(1);
	*out = ptrval(KList, l);
	return true;
}

FN(f_put) {
	(void)b;
	List *l = mustlist(ARG(0));
	if (n < 2)
		list_append(l, NULLV);
	for (int i = 1; i < n; i++)
		list_append(l, a[i]);
	*out = ptrval(KList, l);
	return true;
}

FN(f_push) {
	(void)b;
	List *l = mustlist(ARG(0));
	if (n < 2)
		list_pushfront(l, NULLV);
	for (int i = 1; i < n; i++)
		list_pushfront(l, a[i]);
	*out = ptrval(KList, l);
	return true;
}

FN(f_get) {
	(void)b;
	List *l = mustlist(ARG(0));
	if (l->n == 0)
		return false;
	*out = l->e[l->off];
	l->off++;
	l->n--;
	return true;
}

FN(f_pull) {
	(void)b;
	List *l = mustlist(ARG(0));
	if (l->n == 0)
		return false;
	*out = l->e[l->off + l->n - 1];
	l->n--;
	return true;
}

FN(f_table) {
	(void)b;
	*out = ptrval(KTable, newtable(ARG(0)));
	return true;
}

FN(f_set) {
	(void)b;
	Set *s = newset();
	Value x = ARG(0);
	if (VKIND(x) == KList) {
		List *l = x.w.p;
		for (int64_t i = 0; i < l->n; i++)
			omap_put(&s->m, *list_at(l, i), NULLV);
	} else if (!ISNULL(x))
		runerrv(108, x);
	*out = ptrval(KSet, s);
	return true;
}

FN(f_insert) {
	(void)b;
	Value x = ARG(0);
	if (VKIND(x) == KSet) {
		omap_put(&((Set *)x.w.p)->m, ARG(1), NULLV);
	} else if (VKIND(x) == KTable) {
		omap_put(&((Table *)x.w.p)->m, ARG(1), ARG(2));
	} else
		runerrv(122, x);
	*out = x;
	return true;
}

FN(f_delete) {
	(void)b;
	Value x = ARG(0);
	if (VKIND(x) == KSet)
		omap_del(&((Set *)x.w.p)->m, ARG(1));
	else if (VKIND(x) == KTable)
		omap_del(&((Table *)x.w.p)->m, ARG(1));
	else
		runerrv(122, x);
	*out = x;
	return true;
}

FN(f_member) {
	(void)b;
	Value x = ARG(0);
	OMap *m;
	if (VKIND(x) == KSet)
		m = &((Set *)x.w.p)->m;
	else if (VKIND(x) == KTable)
		m = &((Table *)x.w.p)->m;
	else
		runerrv(122, x);
	*out = ARG(1);
	return omap_get(m, ARG(1), NULL);
}

GENFN(f_key) {
	(void)b;
	if (VKIND(ARG(0)) != KTable)
		runerrv(124, ARG(0));
	Table *t = a[0].w.p;
	return omapgen(&t->m, t, 1);
}

FN(f_sort) {
	(void)b;
	if (VKIND(ARG(0)) == KTable) {
		Table *t = a[0].w.p;
		int64_t i = 1;
		if (!ISNULL(ARG(1)))
			i = mustint(a[1]);
		if (i < 1 || i > 4)
			runerrv(205, intval(i));
		int64_t np = 0;
		Value *pairs = gcnew((size_t)(t->m.live * 2 + 2) * sizeof(Value), T_VALUES);
		for (int64_t k = 0; k < t->m.n; k++)
			if (VKIND(t->m.keys[k]) != KNone) {
				pairs[2 * np] = t->m.keys[k];
				pairs[2 * np + 1] = t->m.vals[k];
				np++;
			}
		int by = (i == 2 || i == 4) ? 1 : 0;
		/* Sort indices stably by the chosen element. */
		Value *keys = gcnew((size_t)(np + 1) * sizeof(Value), T_VALUES);
		for (int64_t k = 0; k < np; k++)
			keys[k] = idxval(KInt, NULL, 0), keys[k].w.i = k;
		/* insertion of a stable merge over pair indices */
		Value *tmp = gcnew((size_t)(np + 1) * sizeof(Value), T_VALUES);
		for (int64_t width = 1; width < np; width *= 2) {
			for (int64_t lo = 0; lo < np; lo += 2 * width) {
				int64_t mid = lo + width < np ? lo + width : np;
				int64_t hi = lo + 2 * width < np ? lo + 2 * width : np;
				int64_t p = lo, q = mid, k = lo;
				while (p < mid && q < hi)
					tmp[k++] = anycmp(pairs[2 * keys[q].w.i + by], pairs[2 * keys[p].w.i + by]) < 0 ? keys[q++] : keys[p++];
				while (p < mid)
					tmp[k++] = keys[p++];
				while (q < hi)
					tmp[k++] = keys[q++];
			}
			memcpy(keys, tmp, (size_t)np * sizeof(Value));
		}
		List *result = newlist(0);
		for (int64_t k = 0; k < np; k++) {
			Value key = pairs[2 * keys[k].w.i], val = pairs[2 * keys[k].w.i + 1];
			if (i <= 2) {
				List *p = newlist(2);
				p->e[0] = key;
				p->e[1] = val;
				list_append(result, ptrval(KList, p));
			} else {
				list_append(result, key);
				list_append(result, val);
			}
		}
		*out = ptrval(KList, result);
		return true;
	}
	Value *v;
	int64_t k;
	if (!elements(ARG(0), &v, &k))
		runerrv(115, ARG(0));
	sortvalues(v, k, anycmp);
	*out = ptrval(KList, listof(v, k));
	return true;
}

FN(f_sortf) {
	(void)b;
	Value *v;
	int64_t k;
	if (!elements(ARG(0), &v, &k))
		runerrv(125, ARG(0));
	int64_t f = 1;
	if (!ISNULL(ARG(1)))
		f = mustint(a[1]);
	if (f == 0)
		runerrv(205, intval(f));
	sortfield = f;
	sortvalues(v, k, nthcmp);
	*out = ptrval(KList, listof(v, k));
	return true;
}

FN(f_copy) {
	(void)b;
	Value x = ARG(0);
	switch (VKIND(x)) {
	case KExternal:
		*out = ext_copy(x);
		return true;
	case KList: {
		List *l = x.w.p;
		*out = ptrval(KList, listof(list_at(l, 0), l->n));
		return true;
	}
	case KTable: {
		Table *t = x.w.p, *c = newtable(t->dflt);
		omap_copy(&c->m, &t->m);
		*out = ptrval(KTable, c);
		return true;
	}
	case KSet: {
		Set *s = x.w.p, *c = newset();
		omap_copy(&c->m, &s->m);
		*out = ptrval(KSet, c);
		return true;
	}
	case KRecord: {
		Record *r = x.w.p;
		*out = ptrval(KRecord, construct(r->t, r->f, r->t->nfields));
		return true;
	}
	}
	*out = x;
	return true;
}

FN(f_serial) {
	(void)b;
	Value x = ARG(0);
	switch (VKIND(x)) {
	case KList: *out = intval(((List *)x.w.p)->serial); return true;
	case KSet: *out = intval(((Set *)x.w.p)->serial); return true;
	case KTable: *out = intval(((Table *)x.w.p)->serial); return true;
	case KRecord: *out = intval(((Record *)x.w.p)->serial); return true;
	case KCoexp: *out = intval(((Coexp *)x.w.p)->serial); return true;
	}
	return false;
}

typedef struct SeqGen {
	Gen g;
	int64_t i, j;
} SeqGen;

static bool seqnext(Gen *gg, Value *out) {
	SeqGen *g = (SeqGen *)gg;
	*out = intval(g->i);
	if (!addint(g->i, g->j, &g->i))
		runerr(203);
	return true;
}

GENFN(f_seq) {
	(void)b;
	SeqGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = seqnext;
	g->i = g->j = 1;
	if (!ISNULL(ARG(0)))
		g->i = mustint(a[0]);
	if (!ISNULL(ARG(1)))
		g->j = mustint(a[1]);
	if (g->j == 0)
		runerrv(211, intval(g->j));
	return &g->g;
}

/* x ||| y */
FN(o_lconcat) {
	(void)b;
	(void)n;
	if (VKIND(a[0]) != KList)
		runerrv(108, a[0]);
	if (VKIND(a[1]) != KList)
		runerrv(108, a[1]);
	List *x = a[0].w.p, *y = a[1].w.p;
	List *l = newlist(x->n + y->n);
	memcpy(l->e, list_at(x, 0), (size_t)x->n * sizeof(Value));
	memcpy(l->e + x->n, list_at(y, 0), (size_t)y->n * sizeof(Value));
	*out = ptrval(KList, l);
	return true;
}

/* [x, y, ...] */
FN(o_listlit) {
	(void)b;
	*out = ptrval(KList, listof(a, n));
	return true;
}

/* x ++ y, x ** y, x -- y: two sets; else, if either is a uset, two
 * usets (a string converted); else two csets. A cset and a uset never
 * mix (error 128): only uset(c) makes a cset a uset. */
static bool setcset(int op, Value *a, Value *out) {
	if (VKIND(a[0]) == KSet && VKIND(a[1]) == KSet) {
		*out = ptrval(KSet, setop(op, a[0].w.p, a[1].w.p));
		return true;
	}
	if (VKIND(a[0]) == KUset || VKIND(a[1]) == KUset) {
		if (VKIND(a[0]) == KCset)
			runerrv(128, a[0]);
		if (VKIND(a[1]) == KCset)
			runerrv(128, a[1]);
		Uset *x, *y;
		if (!cvuset(a[0], &x))
			runerrv(120, a[0]);
		if (!cvuset(a[1], &y))
			runerrv(120, a[1]);
		*out = ptrval(KUset, usetop(op, x, y));
		return true;
	}
	Cset *x, *y;
	if (!cvcset(a[0], &x))
		runerrv(120, a[0]);
	if (!cvcset(a[1], &y))
		runerrv(120, a[1]);
	*out = ptrval(KCset, csetop(op, x, y));
	return true;
}

FN(o_union) { (void)b; (void)n; return setcset('+', a, out); }
FN(o_inter) { (void)b; (void)n; return setcset('*', a, out); }
FN(o_diff) { (void)b; (void)n; return setcset('-', a, out); }

static Builtin fns[] = {
	{.name = "list", .fn = f_list},
	{.name = "put", .fn = f_put},
	{.name = "push", .fn = f_push},
	{.name = "get", .fn = f_get},
	{.name = "pop", .fn = f_get},
	{.name = "pull", .fn = f_pull},
	{.name = "table", .fn = f_table},
	{.name = "set", .fn = f_set},
	{.name = "insert", .fn = f_insert},
	{.name = "delete", .fn = f_delete},
	{.name = "member", .fn = f_member},
	{.name = "key", .gen = f_key},
	{.name = "sort", .fn = f_sort},
	{.name = "sortf", .fn = f_sortf},
	{.name = "copy", .fn = f_copy},
	{.name = "serial", .fn = f_serial},
	{.name = "seq", .gen = f_seq},
};

static Builtin ops[] = {
	{.name = "|||", .key = "|||", .fn = o_lconcat},
	{.name = "[...]", .key = "[...]", .fn = o_listlit},
	{.name = "++", .key = "++", .fn = o_union},
	{.name = "**", .key = "**", .fn = o_inter},
	{.name = "--", .key = "--", .fn = o_diff},
};

void struct_init(void) {
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
}
