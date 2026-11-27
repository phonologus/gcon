/*
 * loadfunc: C functions loaded at run time, written to Icon 9.5's
 * icall.h (ipl/cfuncs), which gcon-vm ships in include/ (docs/design.md,
 * "loadfunc").
 *
 * A C function sees iconx's descriptors, so the shim builds them: for
 * each call, the arguments are converted to descriptors in iconx's
 * layout, in an arena that lives as long as the call, and the result is
 * converted back and copied into gcon-vm's heap. C code therefore never
 * holds a pointer into the heap, and a string it is given is a
 * NUL-terminated copy. Values C cannot take apart (csets, structures,
 * procedures, co-expressions) are passed as blocks with iconx's type
 * codes, which convert back to the values they stand for; a list's block
 * holds its size (ListLen) and cpslots reads its elements. A file is
 * lent as a stdio stream on a duplicate of its descriptor, closed after
 * the call. External values (alcexternal) are blocks in the heap, in
 * iconx's layout, kind KExternal.
 *
 * The callbacks icall.h declares are defined here and exported from the
 * executable, where a library built as icall.h's are (unresolved symbols
 * left to the loader) finds them.
 */
#include "vm.h"
#include "../include/icall.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)
#define EXPORT __attribute__((used, visibility("default")))

/* iconx's type codes beyond icall.h's. */
enum { T_Lrgint = 2, T_Cset = 4, T_Proc = 6, T_Record = 7, T_List = 8,
       T_Set = 10, T_Table = 12, T_Coexpr = 18 };

/* iconx's large integer block, as cfuncs/lgconv.c declares it: digits of
 * half a word, most significant first, from digits[msd] to digits[lsd]. */
typedef struct bignumblock {
	word title, blksize;
	word msd, lsd;
	int sign;
	unsigned int digits[];
} bignumblock;

/* A block standing for a value C cannot take apart. Its first words are
 * a listblock's, so ListLen reads a list's size. */
typedef struct opaqueblock {
	word title, size, id;
	void *head, *tail;
	Value v;
} opaqueblock;

/* ---- The arena and what a call lends ---- */

typedef struct Chunk {
	struct Chunk *next;
	size_t used, cap;
	_Alignas(16) char b[];
} Chunk;

/* What the running call has lent C: blocks that convert back to the
 * values they stand for, and the streams to close afterwards. */
typedef struct Loan {
	void *blk;
	Value v;
	FILE *fp;
} Loan;

static Chunk *arena;
static Loan *loans;
static int nloans, caploans;

static void *aalloc(size_t n) {
	n = (n + 15) & ~(size_t)15;
	if (!arena || arena->cap - arena->used < n) {
		size_t cap = n > 16384 ? n : 16384;
		Chunk *c = xmalloc(sizeof *c + cap);
		c->next = arena;
		c->used = 0;
		c->cap = cap;
		arena = c;
	}
	void *p = arena->b + arena->used;
	arena->used += n;
	return p;
}

static void lend(void *blk, Value v, FILE *fp) {
	if (nloans == caploans) {
		caploans = caploans ? 2 * caploans : 16;
		loans = realloc(loans, (size_t)caploans * sizeof *loans);
	}
	loans[nloans++] = (Loan){blk, v, fp};
}

static Loan *loaned(const void *blk) {
	for (int i = nloans - 1; i >= 0; i--)
		if (loans[i].blk == blk)
			return &loans[i];
	return NULL;
}

/* A scope is one call into C: what it allocates and lends is released
 * when it ends. Scopes nest (an external's image while converting). */
typedef struct Scope {
	Chunk *arena;
	size_t used;
	int nloans;
} Scope;

static void enter(Scope *s) {
	*s = (Scope){arena, arena ? arena->used : 0, nloans};
	flush_output();
}

static void leave(Scope *s) {
	fflush(stdout);
	fflush(stderr);
	for (int i = nloans - 1; i >= s->nloans; i--)
		if (loans[i].fp)
			fclose(loans[i].fp);
	nloans = s->nloans;
	while (arena != s->arena) {
		Chunk *c = arena;
		arena = c->next;
		free(c);
	}
	if (arena)
		arena->used = s->used;
}

/* ---- Values to descriptors ---- */

static int64_t extl_serial;

static void opaque(Value v, int code, word size, descriptor *d) {
	opaqueblock *o = aalloc(sizeof *o);
	*o = (opaqueblock){.title = code, .size = size, .v = v};
	lend(o, v, NULL);
	d->dword = (word)(code | D_Typecode | F_Ptr);
	d->vword = (word)o;
}

static void bigdesc(Big *x, descriptor *d) {
	bignumblock *g = aalloc(sizeof *g + (size_t)x->n * sizeof g->digits[0]);
	g->title = T_Lrgint;
	g->blksize = (word)(sizeof *g + (size_t)x->n * sizeof g->digits[0]);
	g->msd = 0;
	g->lsd = x->n - 1;
	g->sign = x->neg;
	for (int i = 0; i < x->n; i++)
		g->digits[i] = x->d[x->n - 1 - i];
	d->dword = (word)(T_Lrgint | D_Typecode | F_Ptr);
	d->vword = (word)g;
}

static void filedesc(Value v, descriptor *d) {
	File *f = v.w.p;
	int stat;
	int fd = file_lend(f, &stat);
	FILE *fp = NULL;
	if (fd >= 0) {
		int k = dup(fd);
		const char *mode = (stat & 3) == 3 ? "r+" : (stat & 2) ? "w" : "r";
		if (k >= 0 && !(fp = fdopen(k, mode)))
			close(k);
	}
	if (!fp)
		stat = 0;
	fileblock *fb = aalloc(sizeof *fb);
	fb->title = T_File;
	fb->fp = fp;
	fb->stat = stat;
	fb->fname.dword = (word)f->name.n;
	fb->fname.vword = (word)alcstr((char *)f->name.s, (word)f->name.n);
	lend(fb, v, fp);
	d->dword = D_File;
	d->vword = (word)fb;
}

static void todesc(Value v, descriptor *d) {
	switch (VKIND(v)) {
	case KInt:
		d->dword = D_Integer;
		d->vword = (word)v.w.i;
		return;
	case KReal:
		d->dword = D_Real;
		d->vword = (word)alcreal(v.w.r);
		return;
	case KString: {
		String s = vstr(v);
		d->dword = (word)s.n;
		d->vword = (word)alcstr((char *)s.s, (word)s.n);
		return;
	}
	case KBig: bigdesc(v.w.p, d); return;
	case KFile: filedesc(v, d); return;
	case KExternal:
		d->dword = D_External;
		d->vword = (word)v.w.p;
		return;
	case KCset: opaque(v, T_Cset, 0, d); return;
	case KProc: case KBuiltin: case KRecordType: opaque(v, T_Proc, 0, d); return;
	case KRecord: opaque(v, T_Record, 0, d); return;
	case KList: opaque(v, T_List, (word)((List *)v.w.p)->n, d); return;
	case KSet: opaque(v, T_Set, 0, d); return;
	case KTable: opaque(v, T_Table, 0, d); return;
	case KCoexp: opaque(v, T_Coexpr, 0, d); return;
	}
	*d = nulldesc;
}

/* ---- Descriptors to values ---- */

/* A string from C: its bytes. */
static Value strfromc(const char *p, word n) { return strval(strcopy(p, n)); }

static Value bigfromc(const bignumblock *g) {
	int k = (int)(g->lsd - g->msd + 1);
	Big *x = gcnew(sizeof *x + (size_t)(k > 0 ? k : 1) * sizeof x->d[0], T_RAW);
	x->neg = g->sign != 0;
	for (int i = 0; i < k; i++)
		x->d[i] = g->digits[g->lsd - i];
	while (k > 0 && x->d[k - 1] == 0)
		k--;
	x->n = k;
	if (k == 0)
		return intval(0);
	return mkinteger(x);
}

/* tovalue converts d to a value; false if it is no value gcon-vm knows. */
static bool tovalue(const descriptor *d, Value *out) {
	if (d->dword >= 0) { /* a string */
		*out = strfromc((const char *)d->vword, d->dword);
		return true;
	}
	if ((d->dword & D_Typecode) != D_Typecode)
		return false;
	switch (d->dword & 31) {
	case T_Null:
		*out = NULLV;
		return true;
	case T_Integer:
		*out = intval((int64_t)d->vword);
		return true;
	case T_Real:
		*out = realval(((realblock *)d->vword)->rval);
		return true;
	case T_Lrgint:
		*out = bigfromc((const bignumblock *)d->vword);
		return true;
	case T_External:
		*out = ptrval(KExternal, (void *)d->vword);
		return true;
	case T_File: {
		Loan *l = loaned((void *)d->vword);
		if (l) {
			*out = l->v;
			return true;
		}
		fileblock *fb = (fileblock *)d->vword;
		if (!fb->fp)
			return false;
		Value name = NULLV;
		if (!tovalue(&fb->fname, &name) || VKIND(name) != KString)
			name = strval(textc(""));
		*out = ptrval(KFile, file_adopt(fb->fp, (int)fb->stat, vstr(name)));
		return true;
	}
	}
	Loan *l = loaned((void *)d->vword);
	if (!l)
		return false;
	*out = l->v;
	return true;
}

/* ---- Calling C ---- */

typedef int (*CFunc)(int argc, descriptor *argv);

/* callc calls f with the values a and gives its status; argv[0] (its
 * result, or the offending value of an error) is converted to *r, and
 * *ok says whether it could be. */
static int callc(CFunc f, Value *a, int n, Value *r, bool *ok) {
	Scope s;
	enter(&s);
	descriptor *argv = aalloc((size_t)(n + 1) * sizeof *argv);
	argv[0] = nulldesc;
	for (int i = 0; i < n; i++)
		todesc(a[i], &argv[i + 1]);
	int st = f(n, argv);
	*ok = tovalue(&argv[0], r);
	leave(&s);
	return st;
}

static bool cfuncall(Builtin *b, Value *a, int n, Value *out) {
	Value r = NULLV;
	bool ok;
	int st = callc((CFunc)b->cfunc, a, n, &r, &ok);
	if (st < 0)
		return false;
	if (st > 0) { /* an error, with argv[0] its offending value unless null */
		if (ok && VKIND(r) != KNull)
			runerrv(st, r);
		runerr(st);
	}
	if (!ok)
		runerr(123); /* a result gcon-vm cannot represent */
	*out = r;
	return true;
}

FN(f_loadfunc) {
	(void)b;
	(void)n;
	char *file = cstr(muststr(a[0])), *name = cstr(muststr(a[1]));
	static char *curfile;
	static void *handle;
	/* A library handle is reused over successive calls, as iconx's. */
	if (!handle || !curfile || strcmp(file, curfile) != 0) {
		free(curfile);
		curfile = strdup(file);
		handle = dlopen(file, RTLD_LAZY | RTLD_GLOBAL);
	}
	void *fp = NULL;
	if (handle) {
		fp = dlsym(handle, name);
		if (!fp) { /* as iconx: try again with an underscore */
			size_t k = strlen(name);
			char *u = xmalloc(k + 2);
			u[0] = '_';
			memcpy(u + 1, name, k + 1);
			fp = dlsym(handle, u);
			free(u);
		}
	}
	if (!handle || !fp) {
		const char *e = dlerror();
		Buf m = {0};
		bufprintf(&m, "\nloadfunc(\"%s\",\"%s\"): %s\n", file, name, e ? e : "not found");
		emit(vm.errfile, m.b, m.n, true);
		free(m.b);
		runerr(216);
	}
	Builtin *f = xcalloc(1, sizeof *f);
	f->name = strdup(name);
	f->key = f->name;
	f->table = BuiltinFunction;
	f->fn = cfuncall;
	f->arity = -1;
	f->cfunc = fp;
	*out = ptrval(KBuiltin, f);
	return true;
}

/* ---- External values ---- */

static funclist extldefault; /* every function iconx's default */

static externalblock *extblk(Value v) { return v.w.p; }

/* callext calls an external's helper on x (and y), as iconx's
 * callextfunc; false if it fails or gives no value. */
static bool callext(int (*f)(int, descriptor *), Value x, Value *y, Value *r) {
	Value a[2] = {x, y ? *y : NULLV};
	bool ok;
	return callc(f, a, y ? 2 : 1, r, &ok) == 0 && ok;
}

static String extname(Value v) {
	Value r;
	funclist *fl = extblk(v)->funcs;
	if (fl->extlname && callext(fl->extlname, v, NULL, &r) && VKIND(r) == KString)
		return vstr(r);
	return textc("external");
}

/* ext_typename is type(x) for an external x. */
const char *ext_typename(Value v) { return cstr(extname(v)); }

/* ext_image is image(x): its helper's, else "name_id(words)". */
void ext_image(Buf *b, Value v) {
	externalblock *e = extblk(v);
	Value r;
	if (e->funcs->extlimage && callext(e->funcs->extlimage, v, NULL, &r) && VKIND(r) == KString) {
		String s = vstr(r);
		bufadd(b, s.s, (size_t)s.n);
		return;
	}
	String s = extname(v);
	bufadd(b, s.s, (size_t)s.n);
	long words = (long)((e->size - (word)offsetof(externalblock, data)) / (word)sizeof(word));
	bufprintf(b, "_%ld(%ld)", (long)e->id, words);
}

/* ext_copy is copy(x): its helper's, else x itself. */
Value ext_copy(Value v) {
	Value r;
	funclist *fl = extblk(v)->funcs;
	if (fl->extlcopy && callext(fl->extlcopy, v, NULL, &r) && VKIND(r) == KExternal)
		return r;
	return v;
}

/* ext_cmp orders two externals for sorting: by the helper they share,
 * else by type name and then serial number. */
int ext_cmp(Value x, Value y) {
	externalblock *a = extblk(x), *b = extblk(y);
	Value r;
	if (a->funcs == b->funcs && a->funcs->extlcmp && callext(a->funcs->extlcmp, x, &y, &r) &&
	    VKIND(r) == KInt)
		return (r.w.i > 0) - (r.w.i < 0);
	String na = extname(x), nb = extname(y);
	int64_t k = na.n < nb.n ? na.n : nb.n;
	int c = memcmp(na.s, nb.s, (size_t)k);
	if (c)
		return c < 0 ? -1 : 1;
	if (na.n != nb.n)
		return na.n < nb.n ? -1 : 1;
	return (a->id > b->id) - (a->id < b->id);
}

/* ---- icall.h's callbacks ---- */

EXPORT descriptor nulldesc = {D_Null, 0};

EXPORT char *alcstr(char *s, word len) {
	char *p = aalloc((size_t)len + 1);
	if (s)
		memcpy(p, s, (size_t)len);
	p[len] = 0;
	return p;
}

EXPORT realblock *alcreal(double v) {
	realblock *r = aalloc(sizeof *r);
	r->title = T_Real;
	r->rval = v;
	return r;
}

EXPORT fileblock *alcfile(FILE *fp, int stat, descriptor *name) {
	fileblock *f = aalloc(sizeof *f);
	f->title = T_File;
	f->fp = fp;
	f->stat = stat;
	f->fname = name ? *name : nulldesc;
	return f;
}

EXPORT externalblock *alcexternal(long nbytes, funclist *f, void *data) {
	long hdr = (long)offsetof(externalblock, data);
	if (nbytes == 0) /* as iconx: a block with no data */
		nbytes = (long)sizeof(externalblock);
	if (nbytes < hdr) {
		fprintf(stderr, "System error: alcexternal: invalid size\n");
		exit(1);
	}
	long size = (nbytes + (long)sizeof(word) - 1) & ~((long)sizeof(word) - 1);
	externalblock *e = gcnew((size_t)size, T_RAW);
	e->title = T_External;
	e->size = size;
	e->id = ++extl_serial;
	e->funcs = f ? f : &extldefault;
	if (data)
		memcpy(e->data, data, (size_t)(nbytes - hdr));
	return e;
}

EXPORT int cnv_int(descriptor *s, descriptor *d) {
	Value v;
	int64_t i;
	if (!tovalue(s, &v) || !cvint(v, &i))
		return 0;
	d->dword = D_Integer;
	d->vword = (word)i;
	return 1;
}

EXPORT int cnv_real(descriptor *s, descriptor *d) {
	Value v;
	double r;
	if (!tovalue(s, &v) || !cvreal(v, &r))
		return 0;
	d->dword = D_Real;
	d->vword = (word)alcreal(r);
	return 1;
}

EXPORT int cnv_str(descriptor *s, descriptor *d) {
	if (s->dword >= 0) { /* already a string (and NUL-terminated by alcstr or C) */
		*d = *s;
		return 1;
	}
	Value v;
	String t;
	if (!tovalue(s, &v) || !cvstr(v, &t))
		return 0;
	d->dword = (word)t.n;
	d->vword = (word)alcstr((char *)t.s, (word)t.n);
	return 1;
}

EXPORT int cnv_c_str(descriptor *s, descriptor *d) {
	if (s->dword >= 0) { /* a string whose bytes may not end in NUL */
		char *p = alcstr((char *)s->vword, s->dword);
		d->dword = s->dword;
		d->vword = (word)p;
		return 1;
	}
	return cnv_str(s, d);
}

EXPORT double getdbl(descriptor *d) {
	if (d->dword == (word)D_Real)
		return ((realblock *)d->vword)->rval;
	if (d->dword == (word)D_Integer)
		return (double)d->vword;
	return 0;
}

/* cpslots copies the elements i to j-1 (from 1) of the list d to slots,
 * as iconx's (cfuncs/ilists.c uses it). */
EXPORT void cpslots(descriptor *d, descriptor *slots, word i, word j) {
	Loan *l = loaned((void *)d->vword);
	if (!l || VKIND(l->v) != KList)
		return;
	List *L = l->v.w.p;
	for (word k = i; k < j && k - 1 < L->n; k++)
		todesc(*list_at(L, k - 1), &slots[k - i]);
}

static Builtin fns[] = {
	{.name = "loadfunc", .fn = f_loadfunc},
};

void cfunc_init(void) {
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
}
