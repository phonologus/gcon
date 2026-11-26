/*
 * Built programs (prog.h): builtins by reference, csets as the Go
 * reference's vm/cset.go builds them, and the encoder (vm/encode.go).
 */
#include "prog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gcon-vm's tables of the builtins a program may name (generated from
 * the Go reference: ../vm/src/gen_tables.c). */
extern const char *const gen_ops[];
extern const int gen_nops;
extern const char *const gen_functions[];
extern const int gen_nfunctions;

/* ------------------------------------------------------------------ */
/* Builtins */

static StrMap builtins; /* kind byte + name -> Builtin * */

Builtin *builtin_ref(int kind, const char *name, size_t n) {
	Buf key = {0};
	bufputc(&key, kind);
	bufadd(&key, name, n);
	Builtin *b = strmap_get(&builtins, key.b, key.n);
	if (!b) {
		b = xcalloc(1, sizeof *b);
		b->kind = (uint8_t)kind;
		b->name = xstrndup(name, n);
		strmap_put(&builtins, key.b, key.n, b);
	}
	free(key.b);
	return b;
}

static bool intable(const char *const *tab, int n, const char *name, size_t len) {
	int lo = 0, hi = n;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		size_t m = strlen(tab[mid]);
		int c = memcmp(name, tab[mid], len < m ? len : m);
		if (c == 0)
			c = (len > m) - (len < m);
		if (c == 0)
			return true;
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return false;
}

Builtin *op_builtin(const char *key) {
	if (!intable(gen_ops, gen_nops, key, strlen(key))) {
		fprintf(stderr, "gcon: internal error: no operator %s\n", key);
		abort();
	}
	return builtin_ref(BuiltinOp, key, strlen(key));
}

Builtin *function_builtin(const char *name, size_t n) {
	return builtin_ref(intable(gen_functions, gen_nfunctions, name, n) ? BuiltinFunction : BuiltinUnsupported,
		name, n);
}

Builtin *apply_builtin(void) { return builtin_ref(BuiltinApply, "", 0); }

/* ------------------------------------------------------------------ */
/* Csets */

enum { SurrLo = 0xD800, SurrHi = 0xDFFF };

typedef VEC(CsetRange) Ranges;

static const CsetRange universe[] = {{0x80, SurrLo - 1}, {SurrHi + 1, MaxRune}};

Cset *cset_new(void) { return xcalloc(1, sizeof(Cset)); }

static void cset_set(Cset *c, unsigned b) { c->bits[b >> 6] |= (uint64_t)1 << (b & 63); }

/* normalize merges sorted ranges that overlap or touch. */
static Ranges normalize(const CsetRange *rs, size_t n) {
	Ranges out = {0};
	for (size_t i = 0; i < n; i++) {
		CsetRange r = rs[i];
		if (out.n > 0 && r.lo <= out.v[out.n - 1].hi + 1) {
			if (r.hi > out.v[out.n - 1].hi)
				out.v[out.n - 1].hi = r.hi;
			continue;
		}
		VPUSH(out, r);
	}
	return out;
}

/* A stable sort by lo (Go's sort.Slice is not stable, but ranges with
 * equal lo merge to the same result whatever their order). */
static void sortranges(CsetRange *rs, size_t n) {
	for (size_t i = 1; i < n; i++) {
		CsetRange r = rs[i];
		size_t j = i;
		for (; j > 0 && rs[j - 1].lo > r.lo; j--)
			rs[j] = rs[j - 1];
		rs[j] = r;
	}
}

static Ranges unionranges(const CsetRange *a, size_t na, const CsetRange *b, size_t nb) {
	CsetRange *all = xmalloc((na + nb + 1) * sizeof *all);
	if (na)
		memcpy(all, a, na * sizeof *all);
	if (nb)
		memcpy(all + na, b, nb * sizeof *all);
	sortranges(all, na + nb);
	Ranges out = normalize(all, na + nb);
	free(all);
	return out;
}

static Ranges interranges(const CsetRange *a, size_t na, const CsetRange *b, size_t nb) {
	Ranges out = {0};
	for (size_t i = 0, j = 0; i < na && j < nb;) {
		int32_t lo = a[i].lo > b[j].lo ? a[i].lo : b[j].lo;
		int32_t hi = a[i].hi < b[j].hi ? a[i].hi : b[j].hi;
		if (lo <= hi) {
			CsetRange r = {lo, hi};
			VPUSH(out, r);
		}
		if (a[i].hi < b[j].hi)
			i++;
		else
			j++;
	}
	return out;
}

/* invert returns the complement of rs within 0x80..MaxRune. */
static Ranges invert(const CsetRange *rs, size_t n) {
	Ranges out = {0};
	int32_t next = 0x80;
	for (size_t i = 0; i < n; i++) {
		if (rs[i].lo > next) {
			CsetRange r = {next, rs[i].lo - 1};
			VPUSH(out, r);
		}
		next = rs[i].hi + 1;
	}
	if (next <= MaxRune) {
		CsetRange r = {next, MaxRune};
		VPUSH(out, r);
	}
	return out;
}

Cset *cset_union(const Cset *a, const Cset *b) {
	Cset *r = cset_new();
	for (int i = 0; i < 4; i++)
		r->bits[i] = a->bits[i] | b->bits[i];
	Ranges u = unionranges(a->ranges.v, a->ranges.n, b->ranges.v, b->ranges.n);
	r->ranges.v = u.v, r->ranges.n = u.n, r->ranges.cap = u.cap;
	return r;
}

Cset *cset_complement(const Cset *c) {
	Cset *r = cset_new();
	Ranges inv = invert(c->ranges.v, c->ranges.n);
	Ranges cr = interranges(universe, 2, inv.v, inv.n);
	r->ranges.v = cr.v, r->ranges.n = cr.n, r->ranges.cap = cr.cap;
	for (int i = 0; i < 4; i++)
		r->bits[i] = ~c->bits[i];
	return r;
}

/* addrunes adds code points (>= 0x80, valid) to c's ranges. */
void cset_addrunes(Cset *c, const CsetRange *rs, size_t n) {
	CsetRange *sorted = xmalloc((n + 1) * sizeof *sorted);
	if (n)
		memcpy(sorted, rs, n * sizeof *sorted);
	sortranges(sorted, n);
	Ranges norm = normalize(sorted, n);
	Ranges in = interranges(universe, 2, norm.v, norm.n);
	Ranges u = unionranges(c->ranges.v, c->ranges.n, in.v, in.n);
	c->ranges.v = u.v, c->ranges.n = u.n, c->ranges.cap = u.cap;
	free(sorted);
}

Cset *cset_from(const char *s, size_t n, int unit) {
	Cset *c = cset_new();
	if (unit == 1) {
		for (size_t i = 0; i < n; i++)
			cset_set(c, (unsigned char)s[i]);
		return c;
	}
	Ranges rs = {0};
	for (size_t i = 0; i < n;) {
		unsigned char b = (unsigned char)s[i];
		if (b < RuneSelf) {
			cset_set(c, b);
			i++;
			continue;
		}
		int w;
		int32_t r = utf8decode((const uint8_t *)s + i, n - i, &w);
		if (r == RuneError && w == 1)
			cset_set(c, b);
		else {
			CsetRange x = {r, r};
			VPUSH(rs, x);
		}
		i += (size_t)w;
	}
	if (rs.n)
		cset_addrunes(c, rs.v, rs.n);
	return c;
}

/* ------------------------------------------------------------------ */
/* The encoder */

typedef struct Encoder {
	Buf b;
	StrMap builtins, records, procs; /* pointer (as bytes) -> index + 1 */
	VEC(Builtin *) blist;
	VEC(RecordType *) rlist;
	VEC(Proc *) plist;
} Encoder;

static void euint(Buf *b, uint64_t x) {
	while (x >= 0x80) {
		bufputc(b, (int)(x & 0x7f) | 0x80);
		x >>= 7;
	}
	bufputc(b, (int)x);
}

static void eint(Buf *b, int64_t x) {
	uint64_t ux = (uint64_t)x << 1;
	if (x < 0)
		ux = ~ux;
	euint(b, ux);
}

static void eu64(Buf *b, uint64_t x) {
	for (int i = 0; i < 8; i++)
		bufputc(b, (int)(x >> (8 * i) & 0xff));
}

static void estr(Buf *b, const char *s, size_t n) {
	euint(b, n);
	bufadd(b, s, n);
}

static void estrz(Buf *b, const char *s) { estr(b, s, strlen(s)); }

static size_t indexof(StrMap *m, const void *p) {
	return (size_t)(uintptr_t)strmap_get(m, (const char *)&p, sizeof p);
}

static bool number(StrMap *m, const void *p, size_t next) {
	if (indexof(m, p))
		return false;
	strmap_put(m, (const char *)&p, sizeof p, (void *)(uintptr_t)(next + 1));
	return true;
}

static void addbuiltin(Encoder *e, Builtin *b) {
	if (number(&e->builtins, b, e->blist.n))
		VPUSH(e->blist, b);
}

static void collect(Encoder *e, const Value *v) {
	switch (v->kind) {
	case V_BUILTIN:
		addbuiltin(e, v->builtin);
		break;
	case V_RECORD:
		if (number(&e->records, v->record, e->rlist.n))
			VPUSH(e->rlist, v->record);
		break;
	case V_PROC:
		if (number(&e->procs, v->proc, e->plist.n))
			VPUSH(e->plist, v->proc);
		break;
	default:
		break;
	}
}

/* begin starts a section; end puts its length before its body. */
static size_t begin(Encoder *e, const char *tag) {
	bufadd(&e->b, tag, 4);
	return e->b.n;
}

static void end(Encoder *e, size_t start) {
	size_t n = e->b.n - start;
	char *body = xmalloc(n + 1);
	memcpy(body, e->b.b + start, n);
	e->b.n = start;
	euint(&e->b, n);
	bufadd(&e->b, body, n);
	free(body);
}

static const char *operand(Encoder *e, Operand o) {
	if (o.index < 0)
		return "vm: negative operand index";
	euint(&e->b, (uint64_t)o.index << 3 | o.kind);
	return NULL;
}

static const char *proc(Encoder *e, const Proc *p) {
	Buf *b = &e->b;
	const char *err;
	estrz(b, p->name);
	estrz(b, p->file);
	euint(b, (uint64_t)p->nparams);
	bufputc(b, p->variadic ? 1 : 0);
	euint(b, (uint64_t)p->nlocals);
	euint(b, p->localnames.n);
	for (size_t i = 0; i < p->localnames.n; i++)
		estrz(b, p->localnames.v[i]);
	euint(b, p->statics.n);
	for (size_t i = 0; i < p->statics.n; i++)
		euint(b, (uint64_t)p->statics.v[i]);
	euint(b, p->files.n);
	for (size_t i = 0; i < p->files.n; i++)
		estrz(b, p->files.v[i]);
	euint(b, (uint64_t)p->ntemps);
	euint(b, p->code.n);
	int32_t line = 0;
	for (size_t i = 0; i < p->code.n; i++) {
		const Instr *in = &p->code.v[i];
		int flags = 0;
		if (in->dst.kind != NoOperand)
			flags |= InstrDst;
		if (in->x.kind != NoOperand)
			flags |= InstrX;
		if (in->h)
			flags |= InstrH;
		if (in->nargs > 0)
			flags |= InstrArgs;
		if (in->fn)
			flags |= InstrFn;
		if (in->l1)
			flags |= InstrL1;
		if (in->l2)
			flags |= InstrL2;
		if (in->file)
			flags |= InstrFile;
		bufputc(b, in->op);
		bufputc(b, flags);
		eint(b, (int64_t)in->line - line);
		line = in->line;
		if ((flags & InstrDst) && (err = operand(e, in->dst)))
			return err;
		if ((flags & InstrX) && (err = operand(e, in->x)))
			return err;
		if (flags & InstrH)
			euint(b, (uint64_t)in->h);
		if (flags & InstrArgs) {
			euint(b, (uint64_t)in->nargs);
			for (int k = 0; k < in->nargs; k++)
				if ((err = operand(e, in->args[k])))
					return err;
		}
		if (flags & InstrFn)
			euint(b, indexof(&e->builtins, in->fn) - 1);
		if (flags & InstrL1)
			euint(b, (uint64_t)in->l1);
		if (flags & InstrL2)
			euint(b, (uint64_t)in->l2);
		if (flags & InstrFile)
			euint(b, (uint64_t)in->file);
	}
	return NULL;
}

static void value(Encoder *e, const Value *v) {
	Buf *b = &e->b;
	switch (v->kind) {
	case V_NULL:
		bufputc(b, ValNull);
		break;
	case V_INT:
		bufputc(b, ValInt);
		eint(b, v->i);
		break;
	case V_BIG: {
		bufputc(b, ValBig);
		bufputc(b, v->neg ? 1 : 0);
		uint8_t *mag;
		size_t n = bignat_bytes(&v->big, &mag);
		estr(b, (const char *)mag, n);
		break;
	}
	case V_REAL: {
		uint64_t bits;
		memcpy(&bits, &v->r, 8);
		bufputc(b, ValReal);
		eu64(b, bits);
		break;
	}
	case V_STRING:
		bufputc(b, ValString);
		bufputc(b, v->unit);
		estr(b, v->s, v->n);
		break;
	case V_CSET:
		bufputc(b, ValCset);
		for (int i = 0; i < 4; i++)
			eu64(b, v->cset->bits[i]);
		euint(b, v->cset->ranges.n);
		for (size_t i = 0; i < v->cset->ranges.n; i++) {
			euint(b, (uint64_t)v->cset->ranges.v[i].lo);
			euint(b, (uint64_t)v->cset->ranges.v[i].hi);
		}
		break;
	case V_BUILTIN:
		bufputc(b, ValBuiltin);
		euint(b, indexof(&e->builtins, v->builtin) - 1);
		break;
	case V_PROC:
		bufputc(b, ValProc);
		euint(b, indexof(&e->procs, v->proc) - 1);
		break;
	case V_RECORD:
		bufputc(b, ValRecord);
		euint(b, indexof(&e->records, v->record) - 1);
		break;
	}
}

static uint32_t crc32(const uint8_t *p, size_t n) {
	static uint32_t tab[256];
	if (!tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
			tab[i] = c;
		}
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++)
		c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

const char *encode(const Prog *p, const char *const *infokeys, const char *const *infovals, size_t ninfo,
	Buf *out) {
	Encoder e = {0};
	const char *err;
	/* Number the builtins, record types and procedures in the order
	 * they are first met, so that the encoding is deterministic. */
	for (size_t i = 0; i < p->globals.n; i++)
		collect(&e, &p->globals.v[i]);
	for (size_t i = 0; i < p->consts.n; i++)
		collect(&e, &p->consts.v[i]);
	for (size_t k = 0; k < e.plist.n; k++) {
		Proc *pr = e.plist.v[k];
		for (size_t i = 0; i < pr->code.n; i++)
			if (pr->code.v[i].fn)
				addbuiltin(&e, pr->code.v[i].fn);
	}

	bufadd(&e.b, FORMAT_MAGIC, 8);
	euint(&e.b, FormatVersion);
	if (ninfo > 0) {
		size_t *ord = xmalloc(ninfo * sizeof *ord);
		for (size_t i = 0; i < ninfo; i++)
			ord[i] = i;
		for (size_t i = 1; i < ninfo; i++)
			for (size_t j = i; j > 0 && strcmp(infokeys[ord[j - 1]], infokeys[ord[j]]) > 0; j--) {
				size_t t = ord[j];
				ord[j] = ord[j - 1];
				ord[j - 1] = t;
			}
		size_t start = begin(&e, TAG_INFO);
		euint(&e.b, ninfo);
		for (size_t i = 0; i < ninfo; i++) {
			estrz(&e.b, infokeys[ord[i]]);
			estrz(&e.b, infovals[ord[i]]);
		}
		end(&e, start);
		free(ord);
	}

	size_t start = begin(&e, TAG_BUILTINS);
	euint(&e.b, e.blist.n);
	for (size_t i = 0; i < e.blist.n; i++) {
		bufputc(&e.b, e.blist.v[i]->kind);
		estrz(&e.b, e.blist.v[i]->name);
	}
	end(&e, start);

	start = begin(&e, TAG_RECORDS);
	euint(&e.b, e.rlist.n);
	for (size_t i = 0; i < e.rlist.n; i++) {
		estrz(&e.b, e.rlist.v[i]->name);
		euint(&e.b, e.rlist.v[i]->fields.n);
		for (size_t k = 0; k < e.rlist.v[i]->fields.n; k++)
			estrz(&e.b, e.rlist.v[i]->fields.v[k]);
	}
	end(&e, start);

	start = begin(&e, TAG_PROCS);
	euint(&e.b, e.plist.n);
	for (size_t i = 0; i < e.plist.n; i++)
		if ((err = proc(&e, e.plist.v[i])))
			return err;
	end(&e, start);

	start = begin(&e, TAG_CONSTS);
	euint(&e.b, p->consts.n);
	for (size_t i = 0; i < p->consts.n; i++)
		value(&e, &p->consts.v[i]);
	end(&e, start);

	start = begin(&e, TAG_GLOBALS);
	euint(&e.b, p->globals.n);
	for (size_t i = 0; i < p->globals.n; i++) {
		estrz(&e.b, p->globalnames.v[i]);
		value(&e, &p->globals.v[i]);
	}
	eint(&e.b, p->main);
	if (!p->invocable)
		bufputc(&e.b, 0); /* all */
	else {
		bufputc(&e.b, 1);
		size_t nb = (p->globals.n + 7) / 8;
		uint8_t *bits = xcalloc(nb + 1, 1);
		for (size_t i = 0; i < p->globals.n; i++)
			if (p->invocable[i])
				bits[i / 8] |= (uint8_t)(1 << (i % 8));
		bufadd(&e.b, bits, nb);
		free(bits);
	}
	end(&e, start);

	uint32_t crc = crc32((const uint8_t *)e.b.b, e.b.n);
	bufadd(&e.b, TAG_END, 4);
	euint(&e.b, 4);
	for (int i = 0; i < 4; i++)
		bufputc(&e.b, (int)(crc >> (8 * i) & 0xff));
	*out = e.b;
	return NULL;
}
