/*
 * The builtin tables (gcon's builtins.go, builtinref.go, names.go,
 * invoke.go, keywords.go, errors.go): operators by their Ops key,
 * functions by name, and the builtins made on demand (field operators,
 * assignment markers, stubs).
 */
#include "vm.h"
#include "gfx.h"

#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define FN(f) static bool f(Builtin *b, Value *a, int n, Value *out)
#define GENFN(f) static Gen *f(Builtin *b, Value *a, int n)

/* ---- Registry ---- */

static Builtin **opstab, **fntab;
static int nops, nfns, capops, capfns;

void reg(Builtin *b, int table) {
	b->table = (uint8_t)table;
	if (!b->key)
		b->key = b->name;
	if (table == BuiltinOp) {
		if (nops == capops)
			opstab = realloc(opstab, (size_t)(capops = capops ? 2 * capops : 64) * sizeof *opstab);
		opstab[nops++] = b;
	} else {
		if (nfns == capfns)
			fntab = realloc(fntab, (size_t)(capfns = capfns ? 2 * capfns : 64) * sizeof *fntab);
		fntab[nfns++] = b;
	}
}

Builtin *findop(const char *key) {
	for (int i = 0; i < nops; i++)
		if (!strcmp(opstab[i]->key, key))
			return opstab[i];
	return NULL;
}

Builtin *findfn(const char *name, size_t n) {
	for (int i = 0; i < nfns; i++)
		if (strlen(fntab[i]->key) == n && !memcmp(fntab[i]->key, name, n))
			return fntab[i];
	return NULL;
}

static int cmpfn(const void *a, const void *b) {
	return strcmp((*(Builtin *const *)a)->name, (*(Builtin *const *)b)->name);
}

int nfunctions(void) { return nfns; }
Builtin *function_at(int i) { return fntab[i]; }

/* Builtins made on demand, one per name. */
typedef struct Memo {
	Builtin **b;
	int n, cap;
} Memo;

static Builtin *memo(Memo *m, Str name, Builtin *(*mk)(Str)) {
	for (int i = 0; i < m->n; i++)
		if (m->b[i]->field.n == name.n && !memcmp(m->b[i]->field.s, name.s, name.n))
			return m->b[i];
	if (m->n == m->cap)
		m->b = realloc(m->b, (size_t)(m->cap = m->cap ? 2 * m->cap : 16) * sizeof *m->b);
	Builtin *b = mk(name);
	b->field = name;
	m->b[m->n++] = b;
	return b;
}

static char *strz(Str s, const char *prefix) {
	size_t k = strlen(prefix);
	char *p = xmalloc(k + s.n + 1);
	memcpy(p, prefix, k);
	memcpy(p + k, s.s, s.n);
	p[k + s.n] = 0;
	return p;
}

/* fieldindex is recfield, remembered for each record type: a field
 * operator is shared by every use of its name, and record types are
 * fixed when the program is loaded. */
int fieldindex(Builtin *b, RecordType *t) {
	size_t k = (size_t)(t - vm.prog->records);
	if (!b->fidx) {
		b->fidx = xmalloc((size_t)vm.prog->nrecords * sizeof *b->fidx);
		for (int j = 0; j < vm.prog->nrecords; j++)
			b->fidx[j] = -2;
	}
	int i = b->fidx[k];
	if (i == -2)
		i = b->fidx[k] = recfield(t, b->field);
	return i;
}

FN(fieldfn) {
	(void)n;
	if (VKIND(a[0]) != KRecord)
		runerrv(107, a[0]);
	Record *r = a[0].w.p;
	int i = fieldindex(b, r->t);
	if (i < 0)
		runerrv(207, a[0]);
	*out = idxval(KField, r, i);
	return true;
}

static Builtin *mkfield(Str name) {
	Builtin *b = xcalloc(1, sizeof *b);
	b->name = strz(name, ".");
	b->key = b->name;
	b->table = BuiltinField;
	b->fn = fieldfn;
	b->intop = OpField;
	b->arity = -1;
	return b;
}

static Builtin *mkasgn(Str name) {
	Builtin *b = xcalloc(1, sizeof *b);
	b->name = strz(name, "");
	b->key = b->name;
	b->table = BuiltinAsgn;
	b->swap = !strcmp(b->name, ":=:") || !strcmp(b->name, "<->");
	b->arity = -1;
	return b;
}

FN(unsupfn) {
	(void)b;
	(void)a;
	(void)n;
	(void)out;
	runerr(121);
}

static Builtin *mkunsup(Str name) {
	Builtin *b = xcalloc(1, sizeof *b);
	b->name = strz(name, "");
	b->key = b->name;
	b->table = BuiltinUnsupported;
	b->fn = unsupfn;
	b->arity = -1;
	return b;
}

static Memo fieldmemo, asgnmemo, unsupmemo;

Builtin *fieldop(Str name) { return memo(&fieldmemo, name, mkfield); }
Builtin *asgnop(Str name) { return memo(&asgnmemo, name, mkasgn); }
Builtin *unsupported(Str name) { return memo(&unsupmemo, name, mkunsup); }

Builtin applyop = {.name = "!", .key = "", .table = BuiltinApply, .arity = -1};

/* ---- Arity and argument adjustment (names.go) ---- */

static const struct {
	const char *name;
	int arity;
} funcarity[] = {
	{"abs", 1}, {"acos", 1}, {"any", 4}, {"args", 1}, {"asin", 1}, {"atan", 2},
	{"bal", 6}, {"center", 3}, {"char", 1}, {"chdir", 1}, {"close", 1}, {"collect", 2},
	{"copy", 1}, {"cos", 1}, {"cset", 1}, {"delay", 1}, {"delete", 2}, {"detab", -2},
	{"display", 2}, {"dtor", 1}, {"entab", -2}, {"errorclear", 0}, {"exit", 1}, {"exp", 1},
	{"find", 4}, {"flush", 1}, {"function", 0}, {"get", 1}, {"getch", 0}, {"getche", 0},
	{"getenv", 1}, {"iand", 2}, {"icom", 1}, {"image", 1}, {"insert", 3}, {"integer", 1},
	{"ior", 2}, {"ishift", 2}, {"ixor", 2}, {"kbhit", 0}, {"key", 1}, {"left", 3},
	{"list", 2}, {"loadfunc", 2}, {"log", 2}, {"many", 4}, {"map", 3}, {"match", 4},
	{"member", 2}, {"move", 1}, {"name", 1}, {"numeric", 1}, {"open", -3}, {"ord", 1},
	{"pop", 1}, {"pos", 1}, {"proc", 2}, {"pull", 1}, {"push", -2}, {"put", -2},
	{"read", 1}, {"reads", 2}, {"real", 1}, {"remove", 1}, {"rename", 2}, {"repl", 2},
	{"reverse", 1}, {"right", 3}, {"rtod", 1}, {"runerr", -2}, {"seek", 2}, {"seq", 2},
	{"serial", 1}, {"set", 1}, {"sin", 1}, {"sort", 2}, {"sortf", 2}, {"sqrt", 1},
	{"stop", -1}, {"string", 1}, {"system", 1}, {"tab", 1}, {"table", 1}, {"tan", 1},
	{"trim", 2}, {"type", 1}, {"upto", 4}, {"variable", 1}, {"where", 1}, {"write", -1},
	{"writes", -1},
	{"bytes", 1}, {"len", 1}, {"runes", 1}, {"uchar", 1}, {"utf8valid", 1},
	/* the graphics functions, as a graphics iconx declares them */
	{"Active", 0}, {"Alert", -1}, {"Bg", -1}, {"Clip", -1}, {"Clone", -1}, {"Color", -1},
	{"ColorValue", -1}, {"CopyArea", -1}, {"Couple", 2}, {"DrawArc", -1}, {"DrawCircle", -1},
	{"DrawCurve", -1}, {"DrawImage", -1}, {"DrawLine", -1}, {"DrawPoint", -1},
	{"DrawPolygon", -1}, {"DrawRectangle", -1}, {"DrawSegment", -1}, {"DrawString", -1},
	{"EraseArea", -1}, {"Event", -1}, {"Fg", -1}, {"FillArc", -1}, {"FillCircle", -1},
	{"FillPolygon", -1}, {"FillRectangle", -1}, {"Font", -1}, {"FreeColor", -1}, {"GotoRC", -1},
	{"GotoXY", -1}, {"Lower", -1}, {"NewColor", -1}, {"PaletteChars", -1},
	{"PaletteColor", -1}, {"PaletteKey", -1}, {"Pattern", -1}, {"Pending", -1}, {"Pixel", -1},
	{"QueryPointer", 1}, {"Raise", -1}, {"ReadImage", -1}, {"TextWidth", -1},
	{"Uncouple", 1}, {"WAttrib", -1}, {"WDefault", -1}, {"WFlush", -1}, {"WSync", 1},
	{"WriteImage", -1},
};

/* opsbyname: an operator's spelling and its implementations for 0, 1, 2
 * and 3 operands, for string invocation, as iconx's odefs.h. */
static const struct {
	const char *name;
	const char *impl[4];
} opsbyname[] = {
	{"+", {"", "u+", "+", ""}}, {"-", {"", "u-", "-", ""}},
	{"*", {"", "u*", "*", ""}}, {"/", {"", "u/", "/", ""}},
	{"%", {"", "", "%", ""}}, {"^", {"", "u^", "^", ""}},
	{"<", {"", "", "<", ""}}, {"<=", {"", "", "<=", ""}},
	{">", {"", "", ">", ""}}, {">=", {"", "", ">=", ""}},
	{"=", {"", "u=", "=", ""}}, {"~=", {"", "", "~=", ""}},
	{"==", {"", "", "==", ""}}, {"~==", {"", "", "~==", ""}},
	{"<<", {"", "", "<<", ""}}, {"<<=", {"", "", "<<=", ""}},
	{">>", {"", "", ">>", ""}}, {">>=", {"", "", ">>=", ""}},
	{"===", {"", "", "===", ""}}, {"~===", {"", "", "~===", ""}},
	{"||", {"", "", "||", ""}}, {"|||", {"", "", "|||", ""}},
	{"++", {"", "", "++", ""}}, {"--", {"", "", "--", ""}},
	{"**", {"", "", "**", ""}},
	{"~", {"", "u~", "", ""}}, {"!", {"", "u!", "", ""}}, {"?", {"", "u?", "", ""}},
	{"\\", {"", "u\\", "", ""}}, {".", {"", "u.", "", ""}},
	{"[]", {"", "", "[]", ""}}, {"[:]", {"", "", "", "[:]"}},
	{":=", {"", "", "n:=", ""}}, {":=:", {"", "", "n:=:", ""}},
	{"<-", {"", "", "n<-", ""}}, {"<->", {"", "", "n<->", ""}},
	{"...", {"", "", "", "to"}},
};

static int oparity(Builtin *b) {
	for (size_t i = 0; i < sizeof opsbyname / sizeof opsbyname[0]; i++)
		for (int k = 0; k < 4; k++)
			if (opsbyname[i].impl[k][0] && !strcmp(opsbyname[i].impl[k], b->key))
				return k;
	return -1;
}

static void setadjust(Builtin *b) {
	b->arity = -1;
	if (b->table == BuiltinFunction) {
		for (size_t i = 0; i < sizeof funcarity / sizeof funcarity[0]; i++)
			if (!strcmp(funcarity[i].name, b->name)) {
				int ar = funcarity[i].arity;
				b->arity = ar;
				if (ar < 0)
					b->adjmode = AdjMin, b->adjn = -ar - 1;
				else
					b->adjmode = AdjExact, b->adjn = ar;
				return;
			}
		return;
	}
	if (b->noderef) {
		b->adjmode = AdjExact, b->adjn = 2;
		return;
	}
	int k = oparity(b);
	if (k >= 0)
		b->adjmode = AdjExact, b->adjn = k;
}

/* strproc returns the procedure, function, record constructor or
 * operator that name denotes with nargs arguments, or NONE. */
Value strproc(String name, int nargs) {
	if (name.n == 0)
		return NONE;
	Program *p = vm.prog;
	for (int i = 0; i < p->nglobals; i++) {
		Str g = p->globalnames[i];
		if ((int64_t)g.n == name.n && !memcmp(g.s, name.s, g.n) && (!p->invocable || p->invocable[i])) {
			int k = VKIND(vm.globals[i]);
			if (k == KProc || k == KBuiltin || k == KRecordType)
				return vm.globals[i];
			return NONE;
		}
	}
	char c = name.s[0];
	if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
		if (nargs > 3)
			return NONE;
		for (size_t i = 0; i < sizeof opsbyname / sizeof opsbyname[0]; i++)
			if (streq_(name, opsbyname[i].name)) {
				const char *impl = opsbyname[i].impl[nargs];
				if (!impl[0])
					return NONE;
				return ptrval(KBuiltin, findop(impl));
			}
		return NONE;
	}
	Builtin *f = findfn(name.s, (size_t)name.n);
	return f ? ptrval(KBuiltin, f) : NONE;
}

/* ---- Functions ---- */

FN(f_exit) {
	(void)b;
	(void)out;
	int code = 0;
	if (!ISNULL(ARG(0)))
		code = (int)mustint(a[0]);
	vm_exit(code);
}

FN(f_image) {
	(void)b;
	Buf r = {0};
	image(&r, ARG(0));
	*out = strval(bufstring(&r, TextUnit));
	return true;
}

FN(f_type) {
	(void)b;
	*out = strval(textc(typename_(ARG(0))));
	return true;
}

FN(f_string) {
	(void)b;
	String s;
	if (!cvstr(ARG(0), &s))
		return false;
	*out = strval(s);
	return true;
}

static bool isletter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

FN(f_proc) {
	(void)b;
	Value x = ARG(0);
	int k = VKIND(x);
	if (k == KProc || k == KBuiltin || k == KRecordType) {
		*out = x;
		return true;
	}
	String s;
	if (!cvstr(x, &s))
		return false;
	int64_t i = 1;
	if (!ISNULL(ARG(1)))
		i = mustint(a[1]);
	if (i < 0 || i > 3)
		runerrv(205, intval(i));
	if (i == 0) {
		Builtin *f = findfn(s.s, (size_t)s.n);
		if (f && s.n > 0 && isletter(s.s[0])) {
			*out = ptrval(KBuiltin, f);
			return true;
		}
		return false;
	}
	Value p = strproc(s, (int)i);
	*out = p;
	return VKIND(p) != KNone;
}

FN(f_args) {
	(void)b;
	Value x = ARG(0);
	switch (VKIND(x)) {
	case KProc: {
		Proc *p = x.w.p;
		*out = intval(p->variadic ? -p->nparams : p->nparams);
		return true;
	}
	case KRecordType:
		*out = intval(((RecordType *)x.w.p)->nfields);
		return true;
	case KBuiltin: {
		Builtin *f = x.w.p;
		if (f->cfunc) /* a loaded C function: variadic, as iconx's nparam -1 */
			*out = intval(-1);
		else if (f->table == BuiltinFunction)
			*out = intval(f->arity == -1 && f->adjmode == AdjNone ? 0 : f->arity);
		else if (f->noderef)
			*out = intval(2);
		else
			*out = intval(oparity(f));
		return true;
	}
	}
	runerrv(106, x);
}

typedef struct NamesGen {
	Gen g;
	int i, n;
	Builtin **b;
} NamesGen;

static void namesmark(Gen *g) { markptr(((NamesGen *)g)->b); }

static bool namesnext(Gen *gg, Value *out) {
	NamesGen *g = (NamesGen *)gg;
	if (g->i >= g->n)
		return false;
	*out = strval(textc(g->b[g->i++]->name));
	return true;
}

GENFN(f_function) {
	(void)b;
	(void)a;
	(void)n;
	NamesGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = namesnext;
	g->g.mark = namesmark;
	g->n = nfns;
	g->b = gcnew((size_t)nfns * sizeof *g->b, T_RAW); /* static builtins */
	memcpy(g->b, fntab, (size_t)nfns * sizeof *g->b);
	qsort(g->b, (size_t)nfns, sizeof *g->b, cmpfn);
	return &g->g;
}

FN(f_name) {
	(void)b;
	Value v = ARG(0);
	if (!isvar(v))
		runerrv(111, v);
	Buf r = {0};
	varname(&r, v);
	*out = strval(bufstring(&r, TextUnit));
	return true;
}

static Var errorvar = {VError, 0, 0, {0, {0}}};
static Var tracevar = {VTrace, 0, 0, {0, {0}}};
static Var subjvar = {VSubject, 0, 0, {0, {0}}};
static Var posv = {VPos, 0, 0, {0, {0}}};
static Var randv = {VRandom, 0, 0, {0, {0}}};

FN(f_variable) {
	(void)b;
	String s = muststr(ARG(0));
	if (streq_(s, "&error")) { *out = ptrval(KVar, &errorvar); return true; }
	if (streq_(s, "&pos")) { *out = ptrval(KVar, &posv); return true; }
	if (streq_(s, "&random")) { *out = ptrval(KVar, &randv); return true; }
	if (streq_(s, "&subject")) { *out = ptrval(KVar, &subjvar); return true; }
	if (streq_(s, "&trace")) { *out = ptrval(KVar, &tracevar); return true; }
	if (s.n > 0 && s.s[0] == '&')
		return false;
	Program *p = vm.prog;
	Frame *f = vm.cur;
	if (f) {
		for (int i = 0; i < f->proc->nlocalnames; i++) {
			Str l = f->proc->localnames[i];
			if ((int64_t)l.n == s.n && !memcmp(l.s, s.s, l.n)) {
				*out = ptrval(KRef, &f->locals[i]);
				return true;
			}
		}
		Str pn = f->proc->name;
		for (int i = 0; i < p->nglobals; i++) {
			Str g = p->globalnames[i];
			if (g.n == pn.n + 1 + (size_t)s.n && !memcmp(g.s, pn.s, pn.n) && g.s[pn.n] == ' ' &&
			    !memcmp(g.s + pn.n + 1, s.s, (size_t)s.n)) {
				*out = ptrval(KRef, &vm.globals[i]);
				return true;
			}
		}
	}
	for (int i = 0; i < p->nglobals; i++) {
		Str g = p->globalnames[i];
		if ((int64_t)g.n == s.n && !memcmp(g.s, s.s, g.n) && (!p->invocable || p->invocable[i])) {
			*out = ptrval(KRef, &vm.globals[i]);
			return true;
		}
	}
	return false;
}

FN(f_collect) {
	(void)b;
	int64_t region = 0;
	if (!ISNULL(ARG(0)))
		region = mustint(a[0]);
	if (!ISNULL(ARG(1)) && mustint(a[1]) < 0)
		runerrv(205, a[1]);
	if (region < 0 || region > 3)
		runerrv(205, intval(region));
	vm.collections[0]++;
	if (region > 0)
		vm.collections[region]++;
	*out = NULLV;
	return true;
}

FN(f_chdir) {
	(void)b;
	*out = NULLV;
	return chdir(cstr(muststr(ARG(0)))) == 0;
}

FN(f_delay) {
	(void)b;
	int64_t k = mustint(ARG(0));
	flush_output();
	if (k > 0) {
		struct timespec ts = {(time_t)(k / 1000), (long)(k % 1000) * 1000000};
		nanosleep(&ts, NULL);
	}
	*out = NULLV;
	return true;
}

FN(f_display) {
	(void)b;
	int64_t k = vm.level;
	if (!ISNULL(ARG(0)))
		k = mustint(a[0]);
	File *f = vm.errfile;
	Value x = ARG(1);
	if (VKIND(x) == KFile)
		f = x.w.p;
	else if (!ISNULL(x))
		runerrv(105, x);
	if (!f->w || f->closed)
		runerrv(213, ptrval(KFile, f));
	if (k < 0)
		runerrv(205, intval(k));
	if (k > vm.level)
		k = vm.level;
	Buf r = {0};
	bufprintf(&r, "co-expression_%lld(%lld)\n\n", (long long)vm.co->serial, (long long)vm.co->results);
	listing(&r, k);
	emit(f, r.b, r.n, true);
	*out = NULLV;
	return true;
}

FN(f_errorclear) {
	(void)b;
	(void)a;
	(void)n;
	vm.errnum = 0;
	vm.errval = NULLV;
	vm.haveerrval = false;
	*out = NULLV;
	return true;
}

FN(f_runerr) {
	(void)b;
	(void)out;
	int64_t i = mustint(ARG(0));
	if (i <= 0)
		runerrv(205, intval(i));
	if (n > 1)
		runerrv((int)i, a[1]);
	runerr((int)i);
}

/* ---- Keywords ---- */

FN(k_error) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &errorvar); return true; }
FN(k_trace) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &tracevar); return true; }

FN(k_errornumber) {
	(void)b; (void)a; (void)n;
	if (vm.errnum == 0)
		return false;
	*out = intval(vm.errnum);
	return true;
}

FN(k_errortext) {
	(void)b; (void)a; (void)n;
	if (vm.errnum == 0)
		return false;
	*out = strval(textc(errtext(vm.errnum)));
	return true;
}

FN(k_errorvalue) {
	(void)b; (void)a; (void)n;
	if (!vm.haveerrval)
		return false;
	*out = vm.errval;
	return true;
}

FN(k_line) {
	(void)b; (void)a; (void)n;
	*out = intval(vm.cur->proc->code[vm.cur->pc].line);
	return true;
}

FN(k_file) {
	(void)b; (void)a; (void)n;
	size_t k;
	const char *s = fileat(vm.cur->proc, vm.cur->pc, &k);
	*out = strval(mkstr(s, (int64_t)k, TextUnit));
	return true;
}

FN(k_level) { (void)b; (void)a; (void)n; *out = intval(vm.level); return true; }

typedef struct StrsGen {
	Gen g;
	const char *const *s;
	int i, n;
	int64_t *v; /* or integers */
} StrsGen;

static void strsmark(Gen *g) { markptr(((StrsGen *)g)->v); }

static bool strsnext(Gen *gg, Value *out) {
	StrsGen *g = (StrsGen *)gg;
	if (g->i >= g->n)
		return false;
	if (g->v)
		*out = intval(g->v[g->i++]);
	else
		*out = strval(textc(g->s[g->i++]));
	return true;
}

static Gen *strsgen(const char *const *s, int64_t *v, int k) {
	StrsGen *g = gcnew(sizeof *g, T_GEN);
	g->g.next = strsnext;
	g->g.mark = strsmark;
	g->s = s, g->v = v, g->n = k;
	return &g->g;
}

static const char *const features[] = {
	"UNIX",
#ifdef __APPLE__
	"Macintosh",
#endif
	"ASCII", "co-expressions", "dynamic loading", "environment variables",
	"external values", "keyboard functions", "large integers", "pipes",
	"system function", "graphics", "SDL graphics",
};

GENFN(k_features) {
	(void)b; (void)a; (void)n;
	return strsgen(features, NULL, sizeof features / sizeof features[0]);
}

GENFN(k_collections) {
	(void)b; (void)a; (void)n;
	int64_t *v = gcnew(4 * sizeof *v, T_RAW);
	memcpy(v, vm.collections, 4 * sizeof *v);
	return strsgen(NULL, v, 4);
}

static int64_t regions[] = {0, 500000, 500000}, storage[] = {0, 0, 0}, allocated[] = {0, 0, 0, 0};

GENFN(k_regions) { (void)b; (void)a; (void)n; return strsgen(NULL, regions, 3); }
GENFN(k_storage) { (void)b; (void)a; (void)n; return strsgen(NULL, storage, 3); }
GENFN(k_allocated) { (void)b; (void)a; (void)n; return strsgen(NULL, allocated, 4); }

static Value timestr(const char *fmt) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char buf[64];
	strftime(buf, sizeof buf, fmt, &tm);
	return strval(strcopy(buf, (int64_t)strlen(buf), TextUnit));
}

FN(k_clock) { (void)b; (void)a; (void)n; *out = timestr("%H:%M:%S"); return true; }
FN(k_date) { (void)b; (void)a; (void)n; *out = timestr("%Y/%m/%d"); return true; }

FN(k_dateline) {
	(void)b; (void)a; (void)n;
	static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
					     "August", "September", "October", "November", "December"};
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	int h = tm.tm_hour;
	const char *merid = "am";
	if (h >= 12) {
		merid = "pm";
		if (h > 12)
			h -= 12;
	} else if (h < 1)
		h += 12;
	Buf r = {0};
	bufprintf(&r, "%s, %s %d, %d  %d:%02d %s", days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday,
		  tm.tm_year + 1900, h, tm.tm_min, merid);
	*out = strval(bufstring(&r, TextUnit));
	return true;
}

int64_t cpumillis(void) {
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return 0;
	return (int64_t)ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000 +
	       (int64_t)ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
}

FN(k_time) { (void)b; (void)a; (void)n; *out = intval(cpumillis()); return true; }

FN(k_host) {
	(void)b; (void)a; (void)n;
	char h[256] = "";
	gethostname(h, sizeof h - 1);
	*out = strval(strcopy(h, (int64_t)strlen(h), TextUnit));
	return true;
}

FN(k_version) { (void)b; (void)a; (void)n; *out = strval(textc("Icon Version 9.5 (gcon)")); return true; }

static Var prognamevar = {VProgname, 0, 0, {0, {0}}};
static Var dumpvar = {VDump, 0, 0, {0, {0}}};
static Var eventcodevar = {VValue, &vm.eventcode, 0, {0, {0}}};
static Var eventsourcevar = {VValue, &vm.eventsource, 0, {0, {0}}};
static Var eventvaluevar = {VValue, &vm.eventvalue, 0, {0, {0}}};

FN(k_progname) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &prognamevar); return true; }
FN(k_dump) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &dumpvar); return true; }
FN(k_eventcode) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &eventcodevar); return true; }
FN(k_eventsource) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &eventsourcevar); return true; }
FN(k_eventvalue) { (void)b; (void)a; (void)n; *out = ptrval(KVar, &eventvaluevar); return true; }

static Builtin ops[] = {
	{.name = "&error", .key = "&error", .fn = k_error},
	{.name = "&errornumber", .key = "&errornumber", .fn = k_errornumber},
	{.name = "&errortext", .key = "&errortext", .fn = k_errortext},
	{.name = "&errorvalue", .key = "&errorvalue", .fn = k_errorvalue},
	{.name = "&line", .key = "&line", .fn = k_line},
	{.name = "&file", .key = "&file", .fn = k_file},
	{.name = "&features", .key = "&features", .gen = k_features},
	{.name = "&trace", .key = "&trace", .fn = k_trace},
	{.name = "&level", .key = "&level", .fn = k_level},
	{.name = "&clock", .key = "&clock", .fn = k_clock},
	{.name = "&date", .key = "&date", .fn = k_date},
	{.name = "&dateline", .key = "&dateline", .fn = k_dateline},
	{.name = "&time", .key = "&time", .fn = k_time},
	{.name = "&host", .key = "&host", .fn = k_host},
	{.name = "&version", .key = "&version", .fn = k_version},
	{.name = "&progname", .key = "&progname", .fn = k_progname},
	{.name = "&dump", .key = "&dump", .fn = k_dump},
	{.name = "&eventcode", .key = "&eventcode", .fn = k_eventcode},
	{.name = "&eventsource", .key = "&eventsource", .fn = k_eventsource},
	{.name = "&eventvalue", .key = "&eventvalue", .fn = k_eventvalue},
	{.name = "&collections", .key = "&collections", .gen = k_collections},
	{.name = "&regions", .key = "&regions", .gen = k_regions},
	{.name = "&storage", .key = "&storage", .gen = k_storage},
	{.name = "&allocated", .key = "&allocated", .gen = k_allocated},
};

static Builtin fns[] = {
	{.name = "exit", .fn = f_exit}, {.name = "image", .fn = f_image},
	{.name = "type", .fn = f_type}, {.name = "string", .fn = f_string},
	{.name = "proc", .fn = f_proc}, {.name = "args", .fn = f_args},
	{.name = "function", .gen = f_function},
	{.name = "name", .fn = f_name, .reffirst = true},
	{.name = "variable", .fn = f_variable}, {.name = "collect", .fn = f_collect},
	{.name = "chdir", .fn = f_chdir}, {.name = "delay", .fn = f_delay},
	{.name = "display", .fn = f_display}, {.name = "errorclear", .fn = f_errorclear},
	{.name = "runerr", .fn = f_runerr},
};

void ops_init(void);
void cfunc_init(void);

void builtins_init(void) {
	gc_init();
	cset_init();
	num_init();
	struct_init();
	scan_init();
	strfn_init();
	file_fns_init();
	graphics_init();
	ops_init();
	cfunc_init();
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		reg(&ops[i], BuiltinOp);
	for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
		reg(&fns[i], BuiltinFunction);
	for (int i = 0; i < nops; i++)
		setadjust(opstab[i]);
	for (int i = 0; i < nfns; i++)
		setadjust(fntab[i]);
}
