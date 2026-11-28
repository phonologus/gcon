/*
 * The compiler (compile.h), transliterated from the Go reference's
 * internal/compile/compile.go, whose output it reproduces byte for byte.
 *
 * Goal-directed evaluation is compiled with the translation of Todd
 * Proebsting, "Simple Translation of Goal-Directed Evaluation" (PLDI
 * 1997), as in Jcon and Goaldi. Each expression has a start label, where
 * control enters to produce its first value, and a resume label, where
 * control enters to produce its next value; the expression is given the
 * labels to go to when it succeeds (its value then being in its result
 * operand) and when it fails. Within a procedure, backtracking is
 * therefore just jumps. Labels may be bound to other labels (aliases),
 * resolved when the procedure is finished.
 *
 * The Go compiles an expression by recursion. Here, as in the parser,
 * nothing recurses (design/design.md, "the compiler is iterative too"):
 * each Go function that compiles a subexpression is a kind of Frame on
 * the compiler's own stack, with a state saying where it resumes; a
 * frame calls another by setting its resume state and pushing the callee
 * (call), and returns its result (start, resume, value) by popping
 * itself (done). The output must be the Go's exactly, so each frame
 * makes its side effects (labels, temporaries, constants, globals,
 * instructions) in exactly the Go's order: the states follow the Go
 * function's statements, which are given beside them.
 */
#include "compile.h"

#include "lit.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t Label;

static const Operand NoOp = {NoOperand, 0};

/* ------------------------------------------------------------------ */
/* Program-wide state */

typedef struct Compiler {
	Prog *prog;
	StrMap globals; /* name -> slot + 1 */
	StrMap consts;  /* key -> pool index + 1 */
	StrVec *errs;
	/* For string invocation: the globals each procedure refers to
	 * (name -> StrMap of names), the names declared invocable, and
	 * invocable all. */
	StrMap refs;
	StrVec invoc;
	bool invocall;
	StrMap starts; /* Node * -> first-token offset + 2 (1: none) */
} Compiler;

static void cerrorf(Compiler *c, const char *file, long line, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static void cerrorf(Compiler *c, const char *file, long line, const char *fmt, ...) {
	Buf b = {0};
	bufprintf(&b, "File %s; Line %ld # ", file, line);
	va_list ap;
	va_start(ap, fmt);
	char tmp[1];
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(tmp, 0, fmt, ap2);
	va_end(ap2);
	char *s = xmalloc((size_t)n + 1);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	bufputs(&b, s);
	free(s);
	VPUSH(*c->errs, b.b);
}

/* global returns the slot of the named global, creating it with value v. */
static int global(Compiler *c, const char *name, Value v) {
	size_t i = (size_t)(uintptr_t)strmap_get(&c->globals, name, strlen(name));
	if (i)
		return (int)i - 1;
	i = c->prog->globals.n;
	strmap_put(&c->globals, name, strlen(name), (void *)(uintptr_t)(i + 1));
	VPUSH(c->prog->globals, v);
	VPUSH(c->prog->globalnames, xstrndup(name, strlen(name)));
	return (int)i;
}

static Value nullv(void) {
	Value v = {0};
	v.kind = V_NULL;
	return v;
}

/* constant returns the pool index of a constant, sharing equal ones (by
 * key: one value for each key). */
static Operand constant(Compiler *c, const char *key, size_t kn, Value v) {
	size_t i = (size_t)(uintptr_t)strmap_get(&c->consts, key, kn);
	if (!i) {
		VPUSH(c->prog->consts, v);
		i = c->prog->consts.n;
		strmap_put(&c->consts, key, kn, (void *)(uintptr_t)i);
	}
	Operand o = {ConstOp, (int32_t)(i - 1)};
	return o;
}

static Operand constz(Compiler *c, const char *key, Value v) { return constant(c, key, strlen(key), v); }

static Operand intconst(Compiler *c, int64_t n) {
	char key[32];
	snprintf(key, sizeof key, "i%" PRId64, n);
	Value v = {0};
	v.kind = V_INT;
	v.i = n;
	return constz(c, key, v);
}

static Operand realconst(Compiler *c, const char *key, double r) {
	Value v = {0};
	v.kind = V_REAL;
	v.r = r;
	return constz(c, key, v);
}

/* start returns the offset of n's first token (as the Go's compiler,
 * remembering each node's), or -1 if it has none: an explicit stack
 * down the first children. */
static long start(Compiler *c, Node *n);

static long getstart(Compiler *c, Node *n, bool *known) {
	size_t v = (size_t)(uintptr_t)strmap_get(&c->starts, (const char *)&n, sizeof n);
	*known = v != 0;
	return (long)v - 2;
}

static void setstart(Compiler *c, Node *n, long s) {
	strmap_put(&c->starts, (const char *)&n, sizeof n, (void *)(uintptr_t)(s + 2));
}

/* The Go's start takes the first child that has a token: a child node
 * with none is passed over. */
typedef struct Kid {
	Token *tok;
	Node *node;
} Kid;
typedef struct KidsCtx {
	VEC(Kid) v;
} KidsCtx;

static void allkids(void *ctx, Token *t, Node *n) {
	Kid k = {t, n};
	VPUSH(((KidsCtx *)ctx)->v, k);
}

static long start(Compiler *c, Node *n) {
	bool known;
	long s = getstart(c, n, &known);
	if (known)
		return s;
	/* Walk down: a stack of (node, next child index). */
	typedef struct W {
		Node *n;
		KidsCtx kids;
		size_t i;
	} W;
	VEC(W) stk = {0};
	W w0 = {n, {{0}}, 0};
	syn_children(n, allkids, &w0.kids);
	VPUSH(stk, w0);
	long result = -1;
	while (stk.n) {
		W *w = &stk.v[stk.n - 1];
		if (w->i >= w->kids.v.n) {
			/* no child had a token */
			setstart(c, w->n, -1);
			free(w->kids.v.v);
			stk.n--;
			result = -1;
			if (stk.n)
				stk.v[stk.n - 1].i++;
			continue;
		}
		Kid k = w->kids.v.v[w->i];
		if (k.tok) {
			result = (long)k.tok->offset;
		} else {
			long cs = getstart(c, k.node, &known);
			if (!known) {
				W w1 = {k.node, {{0}}, 0};
				syn_children(k.node, allkids, &w1.kids);
				VPUSH(stk, w1);
				continue;
			}
			if (cs < 0) {
				w->i++;
				continue;
			}
			result = cs;
		}
		/* found: every node on the stack starts here */
		while (stk.n) {
			setstart(c, stk.v[stk.n - 1].n, result);
			free(stk.v[stk.n - 1].kids.v.v);
			stk.n--;
		}
	}
	free(stk.v);
	return result;
}

/* ------------------------------------------------------------------ */
/* Procedures */

/* loop is the context of break and next. */
typedef struct Loop {
	Label next;     /* where next goes */
	Label fail;     /* where the loop goes when it fails */
	Label succ;     /* where the loop goes when break succeeds */
	Operand result; /* the loop's result, set by break */
	Operand gate;   /* resume label of the break expression that produced the result */
	Label resume;   /* where resuming the loop goes */
	bool broken;    /* the loop has a break */
	size_t scans;   /* depth of enclosing scans at the loop */
} Loop;

/* An expression's compilation: its start and resume labels and its
 * value; for a sequence, the values of its elements. */
typedef struct Res {
	Label start, resume;
	Operand v;
	Operand *vs;
	int nvs;
} Res;

struct Frame;

/* The loops and scans enclosing the code being compiled are linked
 * stacks: break truncates them while its expression is compiled, and
 * create empties them, by pointing at an earlier node, and restores them
 * by pointing back, in constant time; pushing never overwrites. */
typedef struct LoopNode {
	Loop *l;
	struct LoopNode *next;
} LoopNode;

typedef struct ScanNode {
	Operand env;
	size_t depth; /* the number of scans, this one included */
	struct ScanNode *next;
} ScanNode;

typedef struct PComp {
	Compiler *c;
	SemaInfo *info;
	SemaFile *sf;
	const PPMap *map;
	Lines lines; /* when there is no map */
	const char *file; /* the file of the code being compiled (see emit) */
	Proc *proc;
	Node *decl;
	InstrVec code;
	VEC(int32_t) pos;   /* label -> code index, or -1 */
	VEC(int32_t) alias; /* label -> label it is bound to, or -1 */
	int ntemps;
	StrMap locals; /* Symbol * -> index + 1 */
	LoopNode *loops;
	Label pfail;  /* the procedure's failure */
	int inco;     /* depth of create expressions being compiled */
	ScanNode *scans; /* environments of the scans enclosing the code being compiled */
	int32_t line;
	VEC(struct Frame) stk; /* the machine's */
	Res ret;               /* the result of the frame that last returned */
} PComp;

static void locate(PComp *p, size_t off, const char **file, long *line) {
	if (p->map) {
		ipp_locate(p->map, off, file, line);
		return;
	}
	int l, col;
	lines_position(&p->lines, off, &l, &col);
	*file = p->sf->path;
	*line = l;
}

static size_t nscans(PComp *p) { return p->scans ? p->scans->depth : 0; }

static void pushscan(PComp *p, Operand env) {
	ScanNode *n = xmalloc(sizeof *n);
	n->env = env;
	n->depth = nscans(p) + 1;
	n->next = p->scans;
	p->scans = n;
}

static void perrorf(PComp *p, Node *n, const char *msg) {
	const char *file = p->file;
	long line = p->line;
	long s = start(p->c, n);
	if (s >= 0)
		locate(p, (size_t)s, &file, &line);
	cerrorf(p->c, file, line, "%s", msg);
}

/* setpos makes the source position of offset off the one instructions
 * are emitted with. */
static void setpos(PComp *p, size_t off) {
	long l;
	locate(p, off, &p->file, &l);
	p->line = (int32_t)l;
}

static void setline(PComp *p, Token *t) {
	if (t)
		setpos(p, t->offset);
}

static Label newlabel(PComp *p) {
	VPUSH(p->pos, -1);
	VPUSH(p->alias, -1);
	return (Label)(p->pos.n - 1);
}

/* place puts label l at the next instruction. */
static void place(PComp *p, Label l) { p->pos.v[l] = (int32_t)p->code.n; }

/* bind makes label a an alias for label b. */
static void bind(PComp *p, Label a, Label b) {
	if (a != b)
		p->alias.v[a] = b;
}

static Instr instr(int op) {
	Instr in = {0};
	in.op = (uint8_t)op;
	return in;
}

static Operand *ops1(Operand a) {
	Operand *v = xmalloc(sizeof *v);
	v[0] = a;
	return v;
}

static Operand *ops2(Operand a, Operand b) {
	Operand *v = xmalloc(2 * sizeof *v);
	v[0] = a, v[1] = b;
	return v;
}

static void emit(PComp *p, Instr in) {
	in.line = p->line;
	if (strcmp(p->file, p->proc->file) != 0 && p->proc->file[0]) {
		/* Code from another file (an $include within the procedure). */
		size_t k = 0;
		for (size_t i = 0; i < p->proc->files.n; i++)
			if (!strcmp(p->proc->files.v[i], p->file))
				k = i + 1;
		if (k == 0) {
			VPUSH(p->proc->files, p->file);
			k = p->proc->files.n;
		}
		in.file = (int32_t)k;
	}
	VPUSH(p->code, in);
}

static void jump(PComp *p, Label l) {
	Instr in = instr(OpGoto);
	in.l1 = l;
	emit(p, in);
}

static Operand temp(PComp *p) {
	p->ntemps++;
	Operand o = {TempOp, p->ntemps - 1};
	return o;
}

/* lasttemp returns the operand of the temp allocated n temps ago. */
static Operand lasttemp(PComp *p, int n) {
	Operand o = {TempOp, p->ntemps - n};
	return o;
}

static Operand null(PComp *p) { return constz(p->c, "n", nullv()); }

/* resolve returns the code index of label l, following aliases and
 * jumps to jumps. */
static int32_t resolvenothread(PComp *p, Label l) {
	while (p->alias.v[l] >= 0)
		l = p->alias.v[l];
	return p->pos.v[l];
}

static int32_t resolve(PComp *p, Label l) {
	for (size_t n = 0;; n++) {
		if (p->alias.v[l] >= 0 && n < p->alias.n) {
			l = p->alias.v[l];
			continue;
		}
		break;
	}
	int32_t i = p->pos.v[l];
	if (i < 0) {
		fprintf(stderr, "compile: label %d not placed\n", (int)l);
		abort();
	}
	for (int n = 0; n < 8 && p->code.v[i].op == OpGoto; n++) {
		int32_t j = resolvenothread(p, p->code.v[i].l1);
		if (j == i)
			break;
		i = j;
	}
	return i;
}

/* link resolves labels to code indices. */
static InstrVec link(PComp *p) {
	InstrVec code = {0};
	for (size_t i = 0; i < p->code.n; i++)
		VPUSH(code, p->code.v[i]);
	for (size_t i = 0; i < code.n; i++) {
		Instr *in = &code.v[i];
		switch (in->op) {
		case OpGoto: case OpSetLabel: case OpSuspend: case OpOnce: case OpCreate: case OpCoRet:
			in->l1 = resolve(p, in->l1);
			break;
		case OpOper: case OpCall: case OpResume: case OpIsNull: case OpActivate: case OpAssign:
			in->l1 = resolve(p, in->l1);
			in->l2 = resolve(p, in->l2);
			break;
		case OpScanEnter:
			in->l2 = resolve(p, in->l2);
			break;
		default:
			break;
		}
	}
	return code;
}

static Res res3(Label s, Label r, Operand v) {
	Res x = {s, r, v, NULL, 0};
	return x;
}

/* ref records that the procedure being compiled refers to a global. */
static void ref(PComp *p, const char *name) {
	Token *pn = p->decl->proc.name;
	const char *pname = p->sf->syn->src + pn->offset;
	StrMap *r = strmap_get(&p->c->refs, pname, pn->len);
	if (!r) {
		r = xcalloc(1, sizeof *r);
		strmap_put(&p->c->refs, pname, pn->len, r);
	}
	strmap_put(r, name, strlen(name), (void *)1);
}

static const char *procname(PComp *p) { return p->proc->name; }

static char *catname(const char *a, const char *b) {
	Buf x = {0};
	bufputs(&x, a);
	bufputc(&x, ' ');
	bufputs(&x, b);
	return x.b;
}

static Operand ident(PComp *p, Token *t) {
	Symbol *s = p->info->uses[t - p->sf->syn->toks.v];
	const char *text = p->sf->syn->src + t->offset;
	if (!s) {
		Node id = {0};
		id.kind = N_IDENT;
		id.ident.name = t;
		Buf m = {0};
		bufputs(&m, "unresolved identifier ");
		bufadd(&m, text, t->len);
		perrorf(p, &id, m.b);
		return null(p);
	}
	Operand o = {GlobalOp, 0};
	switch (s->kind) {
	case S_PARAM:
	case S_LOCAL:
	case S_IMPLICIT: {
		size_t i = (size_t)(uintptr_t)strmap_get(&p->locals, (const char *)&s, sizeof s);
		if (i) {
			o.kind = LocalOp;
			o.index = (int32_t)i - 1;
			return o;
		}
		break;
	}
	case S_STATIC:
		o.index = global(p->c, catname(procname(p), s->name), nullv());
		return o;
	case S_GLOBAL:
	case S_PROCEDURE:
	case S_RECORD:
		ref(p, s->name);
		o.index = global(p->c, s->name, nullv());
		return o;
	case S_BUILTIN: {
		ref(p, s->name);
		Value v = {0};
		v.kind = V_BUILTIN;
		v.builtin = function_builtin(s->name, strlen(s->name));
		o.index = global(p->c, s->name, v);
		return o;
	}
	default:
		break;
	}
	Node id = {0};
	id.kind = N_IDENT;
	id.ident.name = t;
	Buf m = {0};
	bufprintf(&m, "%s %s is not yet supported", sym_kind_name(s->kind), s->name);
	perrorf(p, &id, m.b);
	return null(p);
}

/* usetkey: a uset constant's key, its members. */
static Buf usetkey(const Uset *u) {
	Buf key = {0};
	bufputc(&key, 'u');
	bufadd(&key, u->bits, sizeof u->bits);
	bufadd(&key, u->ranges.v, u->ranges.n * sizeof *u->ranges.v);
	return key;
}

static Operand literal(PComp *p, Node *e) {
	Token *t = e->literal.tok;
	const char *text = p->sf->syn->src + t->offset;
	LitError *err = NULL;
	switch (t->kind) {
	case T_STRINGLIT: {
		char *s;
		size_t n;
		if ((err = lit_string(text, t->len, &s, &n)))
			break;
		Buf key = {0};
		bufputc(&key, 's');
		bufadd(&key, s, n);
		Value v = {0};
		v.kind = V_STRING;
		v.s = s;
		v.n = n;
		return constant(p->c, key.b, key.n, v);
	}
	case T_CSETLIT: {
		LitCset lc;
		if ((err = lit_cset(text, t->len, &lc)))
			break;
		Cset *cs = cset_new();
		for (size_t i = 0; i < lc.nsegs; i++)
			cs = cset_union(cs, cset_from(lc.segs[i], lc.seglens[i]));
		for (size_t i = 0; i < lc.nranges; i++)
			for (int32_t x = lc.ranges[i].lo; x <= lc.ranges[i].hi; x++)
				cs->bits[x >> 6] |= (uint64_t)1 << (x & 63);
		/* the key: the members */
		Buf key = {0};
		bufputc(&key, 'c');
		bufadd(&key, cs->bits, sizeof cs->bits);
		Value v = {0};
		v.kind = V_CSET;
		v.cset = cs;
		return constant(p->c, key.b, key.n, v);
	}
	case T_USETLIT: {
		LitUset lu;
		if ((err = lit_uset(text, t->len, &lu)))
			break;
		Uset *us = uset_new();
		memcpy(us->bits, lu.bits, sizeof us->bits);
		if (lu.nranges)
			uset_addrunes(us, (const UsetRange *)lu.ranges, lu.nranges);
		free(lu.ranges);
		Value v = {0};
		v.kind = V_USET;
		v.uset = us;
		Buf key = usetkey(us);
		return constant(p->c, key.b, key.n, v);
	}
	case T_INTLIT:
	case T_REALLIT: {
		LitNumber num;
		if ((err = lit_number(text, t->len, &num)))
			break;
		if (t->kind == T_REALLIT) {
			uint64_t bits;
			memcpy(&bits, &num.real, 8);
			char key[32];
			snprintf(key, sizeof key, "r%016" PRIx64, bits);
			return realconst(p->c, key, num.real);
		}
		if (!num.isbig)
			return intconst(p->c, num.i);
		Buf key = {0};
		bufprintf(&key, "I%s", bignat_decimal(&num.big));
		Value v = {0};
		int64_t small;
		if (bignat_int64(&num.big, &small)) {
			v.kind = V_INT;
			v.i = small;
		} else {
			v.kind = V_BIG;
			v.big = num.big;
		}
		return constant(p->c, key.b, key.n, v);
	}
	default:
		perrorf(p, e, "invalid literal");
		return null(p);
	}
	Buf m = {0};
	bufadd(&m, err->msg, err->len);
	perrorf(p, e, m.b);
	return null(p);
}

/* callinstr emits a resumable call of fn with args at the current
 * position: success goes to succ, failure to fail. It returns the
 * resume label; the result is in the temp allocated two before the
 * return (see lasttemp). */
static Label callinstr(PComp *p, Operand fn, Operand *args, int nargs, Label fail, Label succ) {
	Operand t = temp(p), h = temp(p);
	Instr in = instr(OpCall);
	in.dst = t, in.x = fn, in.h = h.index, in.args = args, in.nargs = nargs, in.l1 = succ, in.l2 = fail;
	emit(p, in);
	Label res = newlabel(p);
	place(p, res);
	in = instr(OpResume);
	in.dst = t, in.h = h.index, in.l1 = succ, in.l2 = fail;
	emit(p, in);
	return res;
}

static Value builtinv(Builtin *b) {
	Value v = {0};
	v.kind = V_BUILTIN;
	v.builtin = b;
	return v;
}

/* genop0 is genOp with no arguments (whose sequence compiles nothing). */
static Res genop0(PComp *p, Builtin *b, Label fail, Label succ) {
	Label a = newlabel(p);
	/* seq of nothing: (succ = a, fail) */
	place(p, a);
	Operand fn = constz(p->c, catname("op", b->name), builtinv(b));
	Label res = callinstr(p, fn, NULL, 0, fail, succ);
	return res3(a, res, lasttemp(p, 2));
}

static bool streq(const char *a, const char *b) { return !strcmp(a, b); }

static Cset *fullcset(int words) {
	Cset *c = cset_new();
	for (int i = 0; i < words; i++)
		c->bits[i] = ~(uint64_t)0;
	return c;
}

static Value csetv(Cset *c) {
	Value v = {0};
	v.kind = V_CSET;
	v.cset = c;
	return v;
}

static const char *const kwops[] = {
	"current", "source", "main", "subject", "pos", "input", "output", "errout",
	"error", "errornumber", "errortext", "errorvalue", "line", "file", "random",
	"trace", "level", "clock", "date", "dateline", "time", "host", "version",
	"progname", "dump", "eventcode", "eventsource", "eventvalue",
	/* The graphics keywords: gcon-vm has graphics; gcon's VM does not,
	 * and its keywords fail, as in an Icon without them. */
	"col", "row", "x", "y", "interval", "control", "shift", "meta", "window",
};

static const struct {
	const char *name;
	int value;
} mouseevents[] = {
	{"lpress", -1}, {"mpress", -2}, {"rpress", -3}, {"lrelease", -4}, {"mrelease", -5},
	{"rrelease", -6}, {"ldrag", -7}, {"mdrag", -8}, {"rdrag", -9}, {"resize", -10},
};

static Res keyword(PComp *p, Node *e, Label fail, Label succ) {
	Token *t = e->keyword.name;
	char *name = xstrndup(p->sf->syn->src + t->offset, t->len);
	Compiler *c = p->c;
	if (streq(name, "null"))
		return res3(succ, fail, null(p));
	if (streq(name, "fail"))
		return res3(fail, fail, null(p));
	if (streq(name, "pi"))
		return res3(succ, fail, realconst(c, "&pi", 3.14159265358979323846264338327950288419716939937510582097494459));
	if (streq(name, "e"))
		return res3(succ, fail, realconst(c, "&e", 2.71828182845904523536028747135266249775724709369995957496696763));
	if (streq(name, "phi"))
		return res3(succ, fail, realconst(c, "&phi", 1.61803398874989484820458683436563811772030917980576286213544862));
	if (streq(name, "cset"))
		return res3(succ, fail, constz(c, "&cset", csetv(fullcset(4))));
	if (streq(name, "ascii"))
		return res3(succ, fail, constz(c, "&ascii", csetv(fullcset(2))));
	if (streq(name, "digits"))
		return res3(succ, fail, constz(c, "&digits", csetv(cset_from("0123456789", 10))));
	if (streq(name, "lcase"))
		return res3(succ, fail, constz(c, "&lcase", csetv(cset_from("abcdefghijklmnopqrstuvwxyz", 26))));
	if (streq(name, "ucase"))
		return res3(succ, fail, constz(c, "&ucase", csetv(cset_from("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26))));
	if (streq(name, "letters"))
		return res3(succ, fail,
			constz(c, "&letters",
				csetv(cset_union(cset_from("abcdefghijklmnopqrstuvwxyz", 26),
					cset_from("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26)))));
	if (streq(name, "uset")) {
		Value v = {0};
		v.kind = V_USET;
		v.uset = uset_univ();
		return res3(succ, fail, constz(c, "&uset", v));
	}
	for (size_t i = 0; i < sizeof kwops / sizeof kwops[0]; i++)
		if (streq(name, kwops[i])) {
			Label s = newlabel(p);
			place(p, s);
			Operand tmp = temp(p);
			Instr in = instr(OpOper);
			char key[32];
			snprintf(key, sizeof key, "&%s", name);
			in.fn = op_builtin(key), in.dst = tmp, in.l1 = succ, in.l2 = fail;
			emit(p, in);
			return res3(s, fail, tmp);
		}
	if (streq(name, "features") || streq(name, "collections") || streq(name, "regions") ||
	    streq(name, "storage") || streq(name, "allocated")) {
		char key[32];
		snprintf(key, sizeof key, "&%s", name);
		return genop0(p, op_builtin(key), fail, succ);
	}
	if (streq(name, "column")) /* &column fails in iconx */
		return res3(fail, fail, null(p));
	for (size_t i = 0; i < sizeof mouseevents / sizeof mouseevents[0]; i++)
		if (streq(name, mouseevents[i].name))
			return res3(succ, fail, intconst(c, mouseevents[i].value));
	Buf m = {0};
	bufprintf(&m, "&%s is not yet supported", name);
	perrorf(p, e, m.b);
	return res3(fail, fail, null(p));
}

/* produce places at l code that records resume in gate, puts the value
 * v (as a variable, if it is one) in t, and goes to succ. */
static void produce(PComp *p, Label l, Operand gate, Label resume, Operand t, Operand v, Label succ) {
	place(p, l);
	Instr in = instr(OpSetLabel);
	in.dst = gate, in.l1 = resume;
	emit(p, in);
	in = instr(OpMoveVar);
	in.dst = t, in.x = v;
	emit(p, in);
	jump(p, succ);
}

static void scanswap(PComp *p, Operand env) {
	Instr in = instr(OpScanSwap);
	in.x = env;
	emit(p, in);
}

/* leave returns a label that leaves the scans entered since depth (the
 * innermost first), restoring their outer environments, and goes to
 * target. */
static Label leave(PComp *p, Label target, size_t depth) {
	if (nscans(p) == depth)
		return target;
	Label l = newlabel(p);
	place(p, l);
	for (ScanNode *n = p->scans; n && n->depth > depth; n = n->next)
		scanswap(p, n->env);
	jump(p, target);
	return l;
}

/* failat emits code that leaves the scans being compiled and fails the
 * procedure, with the line of t, and returns its label. */
static Label failat(PComp *p, Token *t) {
	int32_t save = p->line;
	setline(p, t);
	Label l = newlabel(p);
	place(p, l);
	for (ScanNode *n = p->scans; n; n = n->next)
		scanswap(p, n->env);
	emit(p, instr(OpFail));
	p->line = save;
	return l;
}

/* reenter emits code that re-enters the scans left by leave(_, 0), the
 * outermost first. */
static void reenter(PComp *p) {
	size_t n = nscans(p);
	Operand *envs = xmalloc((n + 1) * sizeof *envs);
	for (ScanNode *s = p->scans; s; s = s->next)
		envs[s->depth - 1] = s->env;
	for (size_t i = 0; i < n; i++)
		scanswap(p, envs[i]);
	free(envs);
}

/* ------------------------------------------------------------------ */
/* The machine */

enum {
	F_EXPR,     /* expr */
	F_SEQ,      /* seq */
	F_ENTER,    /* enter, with the frame it compiles */
	F_STMTS,    /* stmts */
	F_BLOCK,    /* block */
	F_OPER,     /* oper */
	F_GENOP,    /* genOp */
	F_UNARY,    /* unary */
	F_BINARY,   /* binary */
	F_REPALT,   /* repalt */
	F_ALT,      /* alt */
	F_IF,       /* ifExpr */
	F_LOOP,     /* loop */
	F_BREAK,    /* brk */
	F_RET,      /* ret */
	F_CASE,     /* caseExpr */
	F_CREATE,   /* create */
	F_ACTIVATE, /* activate */
	F_SCAN,     /* scan */
	F_LIMIT,    /* limit */
	F_INDEX,    /* the Index case of expr */
	F_SECTION,  /* the +: and -: cases of expr */
	F_CALLX,    /* the Call case of expr */
};

typedef struct Frame {
	uint8_t fn, st;
	Node *e;
	Label fail, succ;
	Label L[8];
	Operand O[6];
	Res R[3];
	int i;
	Node **es; /* F_SEQ, F_OPER, F_GENOP, F_STMTS, F_BLOCK */
	int nes;
	VEC(Operand) vs; /* F_SEQ */
	Builtin *b;      /* F_OPER, F_GENOP */
	Loop *loop;      /* F_LOOP, F_BREAK */
	struct Frame *child; /* F_ENTER */
	bool flag;
	/* saved loops and scans (F_BREAK, F_CREATE) */
	LoopNode *savedloops;
	ScanNode *savedscans;
	Node *x, *y; /* F_ACTIVATE, F_SCAN, F_LIMIT */
} Frame;

static Frame frame(int fn) {
	Frame f;
	memset(&f, 0, sizeof f);
	f.fn = (uint8_t)fn;
	return f;
}

static Frame fexpr(Node *e, Label fail, Label succ) {
	Frame f = frame(F_EXPR);
	f.e = e, f.fail = fail, f.succ = succ;
	return f;
}

static Frame fseq(Node **es, int n, Label fail, Label succ) {
	Frame f = frame(F_SEQ);
	f.es = es, f.nes = n, f.fail = fail, f.succ = succ;
	return f;
}

static Frame fenter(Frame child) {
	Frame f = frame(F_ENTER);
	f.child = xmalloc(sizeof *f.child);
	*f.child = child;
	return f;
}

/* stmt compiles e as a bounded expression: whether it succeeds or
 * fails, control goes to next, and it is never resumed. */
static Frame fstmt(Node *e, Label next) { return fenter(fexpr(e, next, next)); }

static Frame foper(Builtin *b, Node **es, int n, Label fail, Label succ) {
	Frame f = frame(F_OPER);
	f.b = b, f.es = es, f.nes = n, f.fail = fail, f.succ = succ;
	return f;
}

static Frame fgenop(Builtin *b, Node **es, int n, Label fail, Label succ) {
	Frame f = foper(b, es, n, fail, succ);
	f.fn = F_GENOP;
	return f;
}

static Node **nodes1(Node *a) {
	Node **v = xmalloc(sizeof *v);
	v[0] = a;
	return v;
}

static Node **nodes2(Node *a, Node *b) {
	Node **v = xmalloc(2 * sizeof *v);
	v[0] = a, v[1] = b;
	return v;
}

static Node **nodes3(Node *a, Node *b, Node *c) {
	Node **v = xmalloc(3 * sizeof *v);
	v[0] = a, v[1] = b, v[2] = c;
	return v;
}

/* prepend gives x followed by the n nodes of v. */
static Node **prepend(Node *x, Node **v, size_t n) {
	Node **w = xmalloc((n + 1) * sizeof *w);
	w[0] = x;
	for (size_t i = 0; i < n; i++)
		w[i + 1] = v[i];
	return w;
}

static Frame *top(PComp *p) { return &p->stk.v[p->stk.n - 1]; }

/* call resumes the current frame at state st once c has returned. The
 * push may move the stack: the caller must not touch its frame after. */
static void call(PComp *p, int st, Frame c) {
	top(p)->st = (uint8_t)st;
	VPUSH(p->stk, c);
}

static void done(PComp *p, Res r) {
	p->stk.n--;
	p->ret = r;
}

/* The operator text of a token: its spelling (tokens.inc). */
static const char *optext(Token *t) { return tok_name(t->kind); }

static bool isbinaryop(const char *op) {
	static const char *const ops[] = {
		"+", "-", "*", "/", "%", "^", "<", "<=", ">", ">=", "=", "~=", "==", "~==",
		"<<", "<<=", ">>", ">>=", "===", "~===", "||", "|||", "++", "--", "**",
	};
	for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
		if (streq(op, ops[i]))
			return true;
	return false;
}

/* compoundUnary: the tokens that, in prefix position, are sequences of
 * unary operators, applied right to left as icont's unopb does: ++x is
 * +(+x) and ~==x is ~(=(=x)). */
static int compound(const char *op, TokKind *seq) {
	static const struct {
		const char *op;
		int n;
		TokKind seq[4];
	} tab[] = {
		{"++", 2, {T_PLUS, T_PLUS}}, {"--", 2, {T_MINUS, T_MINUS}}, {"**", 2, {T_STAR, T_STAR}},
		{"==", 2, {T_NMEQ, T_NMEQ}}, {"===", 3, {T_NMEQ, T_NMEQ, T_NMEQ}},
		{"~=", 2, {T_TILDE, T_NMEQ}}, {"~==", 3, {T_TILDE, T_NMEQ, T_NMEQ}},
		{"~===", 4, {T_TILDE, T_NMEQ, T_NMEQ, T_NMEQ}},
	};
	for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
		if (streq(op, tab[i].op)) {
			memcpy(seq, tab[i].seq, (size_t)tab[i].n * sizeof *seq);
			return tab[i].n;
		}
	return 0;
}

static Node *synth(NodeKind k) {
	Node *n = xcalloc(1, sizeof *n);
	n->kind = k;
	return n;
}

static void saveloopsscans(PComp *p, Frame *f) {
	f->savedloops = p->loops;
	f->savedscans = p->scans;
}

static void restoreloopsscans(PComp *p, Frame *f) {
	p->loops = f->savedloops;
	p->scans = f->savedscans;
}

static void step(PComp *p) {
	Frame *f = top(p);
	Res r = p->ret;
	Instr in;
	switch (f->fn) {

	/* expr compiles e. See the package comment. */
	case F_EXPR: {
		Node *e = f->e;
		if (!e) {
			done(p, res3(f->succ, f->fail, null(p)));
			return;
		}
		if (f->st == 0) {
			long s = start(p->c, e);
			if (s >= 0)
				setpos(p, (size_t)s);
		}
		Label fail = f->fail, succ = f->succ;
		switch (f->st) {
		case 0:
			break;
		case 1: /* Paren of several: after seq */
			done(p, res3(r.start, r.resume, r.nvs == 0 ? null(p) : r.vs[r.nvs - 1]));
			return;
		default:
			/* a frame that became another returned here: pass on */
			done(p, r);
			return;
		}
		switch (e->kind) {
		case N_IDENT:
			done(p, res3(succ, fail, ident(p, e->ident.name)));
			return;
		case N_LITERAL:
			done(p, res3(succ, fail, literal(p, e)));
			return;
		case N_KEYWORD:
			done(p, keyword(p, e, fail, succ));
			return;
		case N_PAREN:
			if (e->group.list.elems.n == 1) {
				f->e = e->group.list.elems.v[0]; /* expr again */
				return;
			}
			call(p, 1, fseq(e->group.list.elems.v, (int)e->group.list.elems.n, fail, succ));
			return;
		case N_BLOCK: {
			Frame b = frame(F_BLOCK);
			b.es = e->group.list.elems.v, b.nes = (int)e->group.list.elems.n, b.fail = fail, b.succ = succ;
			*f = b;
			return;
		}
		case N_UNARY: {
			Frame u = frame(F_UNARY);
			u.e = e, u.fail = fail, u.succ = succ;
			*f = u;
			return;
		}
		case N_BINARY: {
			Frame b = frame(F_BINARY);
			b.e = e, b.fail = fail, b.succ = succ;
			*f = b;
			return;
		}
		case N_TOBY:
			if (e->toby.z)
				*f = fgenop(op_builtin("to"), nodes3(e->toby.x, e->toby.y, e->toby.z), 3, fail, succ);
			else
				*f = fgenop(op_builtin("to"), nodes2(e->toby.x, e->toby.y), 2, fail, succ);
			return;
		case N_LISTLIT:
			*f = foper(op_builtin("[...]"), e->group.list.elems.v, (int)e->group.list.elems.n, fail, succ);
			return;
		case N_INDEX: {
			Frame x = frame(F_INDEX);
			x.e = e, x.fail = fail, x.succ = succ;
			*f = x;
			return;
		}
		case N_SECTION:
			if (e->section.op->kind == T_COLON) {
				*f = foper(op_builtin("[:]"), nodes3(e->section.x, e->section.lo, e->section.hi), 3, fail,
					succ);
				return;
			}
			Frame s = frame(F_SECTION);
			s.e = e, s.fail = fail, s.succ = succ;
			*f = s;
			return;
		case N_CALL: {
			Frame x = frame(F_CALLX);
			x.e = e, x.fail = fail, x.succ = succ;
			*f = x;
			return;
		}
		case N_IF:
		case N_LOOP:
		case N_RETURN:
		case N_BREAK:
		case N_CASE:
		case N_CREATE: {
			static const int fns[] = {[N_IF] = F_IF, [N_LOOP] = F_LOOP, [N_RETURN] = F_RET,
				[N_BREAK] = F_BREAK, [N_CASE] = F_CASE, [N_CREATE] = F_CREATE};
			Frame x = frame(fns[e->kind]);
			x.e = e, x.fail = fail, x.succ = succ;
			*f = x;
			return;
		}
		case N_NEXT:
			if (!p->loops) {
				perrorf(p, e, "invalid context for next");
				done(p, res3(fail, fail, null(p)));
				return;
			}
			{
				Loop *l = p->loops->l;
				Label lv = leave(p, l->next, l->scans);
				done(p, res3(lv, fail, null(p)));
			}
			return;
		case N_PDCO: {
			/* p{e1, ..., en} is p([create e1, ..., create en]), as
			 * icont's Pdco rules build it; an empty ei is create
			 * &null. */
			Node *list = synth(N_LISTLIT);
			list->group.open = e->call.open;
			list->group.close = e->call.close;
			for (size_t i = 0; i < e->call.args.elems.n; i++) {
				Node *cr = synth(N_CREATE);
				cr->create.create = e->call.open;
				cr->create.x = e->call.args.elems.v[i];
				VPUSH(list->group.list.elems, cr);
			}
			Node *cl = synth(N_CALL);
			cl->call.fun = e->call.fun;
			cl->call.open = e->call.open;
			cl->call.close = e->call.close;
			VPUSH(cl->call.args.elems, list);
			f->e = cl; /* expr again */
			return;
		}
		case N_FIELD: {
			Token *t = e->field.name;
			Builtin *b = builtin_ref(BuiltinField, p->sf->syn->src + t->offset, t->len);
			*f = foper(b, nodes1(e->field.x), 1, fail, succ);
			return;
		}
		default:
			perrorf(p, e, "invalid expression");
			done(p, res3(fail, fail, null(p)));
			return;
		}
	}

	/* seq compiles es left to right as a chain: the failure of each
	 * resumes the one before it, and the first fails to fail. Control
	 * goes to succ when all have produced values. L[0] resume, L[1] s,
	 * L[2] start, L[3] prevSucc. */
	case F_SEQ:
		if (f->st == 0) {
			if (f->nes == 0) {
				Res x = res3(f->succ, f->fail, NoOp);
				done(p, x);
				return;
			}
			f->L[0] = f->fail;
			f->i = 0;
		} else {
			if (f->i == 0)
				f->L[2] = r.start;
			else
				bind(p, f->L[3], r.start);
			f->L[3] = f->L[1];
			f->L[0] = r.resume;
			VPUSH(f->vs, r.v);
			f->i++;
			if (f->i == f->nes) {
				bind(p, f->L[3], f->succ);
				Res x = res3(f->L[2], f->L[0], NoOp);
				x.vs = f->vs.v, x.nvs = (int)f->vs.n;
				done(p, x);
				return;
			}
		}
		f->L[1] = newlabel(p);
		call(p, 1, fexpr(f->es[f->i], f->L[0], f->L[1]));
		return;

	/* enter emits a jump from the current position to the start of the
	 * code that its frame emits. L[0] entry. */
	case F_ENTER:
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			jump(p, f->L[0]);
			call(p, 1, *f->child);
			return;
		}
		bind(p, f->L[0], r.start);
		done(p, r);
		return;

	/* stmts compiles a sequence of bounded expressions, then goes to
	 * next (f->succ). L[0] after. */
	case F_STMTS:
		if (f->st == 1) {
			place(p, f->L[0]);
			f->i++;
		}
		if (f->i < f->nes) {
			f->L[0] = newlabel(p);
			call(p, 1, fstmt(f->es[f->i], f->L[0]));
			return;
		}
		jump(p, f->succ);
		done(p, res3(0, 0, NoOp));
		return;

	/* block compiles { e1; ...; en }: all but the last are bounded; the
	 * block's results are those of the last. L[0] first, L[1] after. */
	case F_BLOCK:
		switch (f->st) {
		case 0:
			if (f->nes == 0) {
				done(p, res3(f->succ, f->fail, null(p)));
				return;
			}
			f->L[0] = newlabel(p);
			place(p, f->L[0]);
			f->i = 0;
			break;
		case 1:
			place(p, f->L[1]);
			f->i++;
			break;
		case 2:
			done(p, res3(f->L[0], r.resume, r.v));
			return;
		}
		if (f->i < f->nes - 1) {
			f->L[1] = newlabel(p);
			call(p, 1, fstmt(f->es[f->i], f->L[1]));
			return;
		}
		call(p, 2, fenter(fexpr(f->es[f->nes - 1], f->fail, f->succ)));
		return;

	/* oper compiles an operator that produces at most one result. L[0]
	 * a. */
	case F_OPER:
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			call(p, 1, fseq(f->es, f->nes, f->fail, f->L[0]));
			return;
		}
		place(p, f->L[0]);
		{
			Operand t = temp(p);
			in = instr(OpOper);
			in.fn = f->b, in.dst = t, in.args = r.vs, in.nargs = r.nvs, in.l1 = f->succ, in.l2 = r.resume;
			emit(p, in);
			done(p, res3(r.start, r.resume, t));
		}
		return;

	/* genOp compiles a generator operator applied to args. L[0] a. */
	case F_GENOP:
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			call(p, 1, fseq(f->es, f->nes, f->fail, f->L[0]));
			return;
		}
		place(p, f->L[0]);
		{
			Operand fn = constz(p->c, catname("op", f->b->name), builtinv(f->b));
			Label res = callinstr(p, fn, r.vs, r.nvs, r.resume, f->succ);
			done(p, res3(r.start, res, lasttemp(p, 2)));
		}
		return;

	case F_UNARY: {
		Node *e = f->e;
		Label fail = f->fail, succ = f->succ;
		const char *op = optext(e->unary.op);
		if (f->st == 0) {
			TokKind seq[4];
			int n = compound(op, seq);
			if (n) {
				Node *x = e->unary.x;
				for (int i = n - 1; i >= 0; i--) {
					Token *t = xmalloc(sizeof *t);
					*t = *e->unary.op;
					t->kind = seq[i];
					Node *u = synth(N_UNARY);
					u->unary.op = t;
					u->unary.x = x;
					x = u;
				}
				f->e = x; /* unary again (not expr: no position) */
				return;
			}
			if (streq(op, "|") || streq(op, "||") || streq(op, "|||")) {
				Frame ra = frame(F_REPALT);
				ra.x = e->unary.x, ra.fail = fail, ra.succ = succ;
				*f = ra;
				return;
			}
			const char *uop = streq(op, "-") ? "u-" : streq(op, "+") ? "u+" : streq(op, "*") ? "u*"
				: streq(op, "~") ? "u~" : NULL;
			if (uop) {
				*f = foper(op_builtin(uop), nodes1(e->unary.x), 1, fail, succ);
				return;
			}
			if (streq(op, "!") || streq(op, "=")) {
				*f = fgenop(op_builtin(streq(op, "!") ? "u!" : "u="), nodes1(e->unary.x), 1, fail, succ);
				return;
			}
			if (streq(op, "/") || streq(op, "\\")) {
				f->L[0] = newlabel(p);
				call(p, 1, fexpr(e->unary.x, fail, f->L[0]));
				return;
			}
			if (streq(op, "not")) {
				f->L[0] = newlabel(p);
				call(p, 2, fexpr(e->unary.x, f->L[0], fail));
				return;
			}
			if (streq(op, "@")) {
				Frame a = frame(F_ACTIVATE);
				a.x = NULL, a.y = e->unary.x, a.fail = fail, a.succ = succ;
				*f = a;
				return;
			}
			if (streq(op, "^") || streq(op, "?")) {
				*f = foper(op_builtin(streq(op, "^") ? "u^" : "u?"), nodes1(e->unary.x), 1, fail, succ);
				return;
			}
			if (streq(op, ".")) {
				f->L[0] = newlabel(p);
				call(p, 3, fexpr(e->unary.x, fail, f->L[0]));
				return;
			}
			Buf m = {0};
			bufprintf(&m, "unary %s is not yet supported", op);
			perrorf(p, e, m.b);
			done(p, res3(fail, fail, null(p)));
			return;
		}
		place(p, f->L[0]);
		switch (f->st) {
		case 1: /* / or \ */
			in = instr(OpIsNull);
			in.x = r.v, in.l1 = succ, in.l2 = r.resume;
			if (streq(op, "\\"))
				in.l1 = r.resume, in.l2 = succ;
			emit(p, in);
			done(p, res3(r.start, r.resume, r.v));
			return;
		case 2: /* not */
			jump(p, succ);
			done(p, res3(r.start, fail, null(p)));
			return;
		case 3: { /* . */
			Operand t = temp(p);
			in = instr(OpMove);
			in.dst = t, in.x = r.v;
			emit(p, in);
			jump(p, succ);
			done(p, res3(r.start, r.resume, t));
			return;
		}
		}
		return;
	}

	/* binary: L[0..3] the Go's a, b, rr, c */
	case F_BINARY: {
		Node *e = f->e;
		Label fail = f->fail, succ = f->succ;
		const char *op = optext(e->binary.op);
		Node *X = e->binary.x, *Y = e->binary.y;
		if (f->st == 0) {
			if (isbinaryop(op)) {
				*f = foper(op_builtin(op), nodes2(X, Y), 2, fail, succ);
				return;
			}
			if (streq(op, "&")) {
				call(p, 1, fseq(nodes2(X, Y), 2, fail, succ));
				return;
			}
			if (streq(op, "|")) {
				Frame a = frame(F_ALT);
				a.e = e, a.fail = fail, a.succ = succ;
				*f = a;
				return;
			}
			if (streq(op, "@")) {
				Frame a = frame(F_ACTIVATE);
				a.x = X, a.y = Y, a.fail = fail, a.succ = succ;
				*f = a;
				return;
			}
			if (streq(op, "\\")) {
				Frame l = frame(F_LIMIT);
				l.x = X, l.y = Y, l.fail = fail, l.succ = succ;
				*f = l;
				return;
			}
			if (streq(op, "?") || streq(op, "?:=")) {
				Frame s = frame(F_SCAN);
				s.x = X, s.y = Y, s.flag = streq(op, "?:="), s.fail = fail, s.succ = succ;
				*f = s;
				return;
			}
			size_t n = strlen(op);
			bool aug = false;
			if (n > 2 && streq(op + n - 2, ":=")) {
				char *base = xstrndup(op, n - 2);
				aug = isbinaryop(base);
			}
			if (streq(op, ":=") || streq(op, ":=:") || streq(op, "!") || streq(op, "&:=") || aug) {
				f->L[0] = newlabel(p);
				call(p, 2, fseq(nodes2(X, Y), 2, fail, f->L[0]));
				return;
			}
			if (streq(op, "@:=")) {
				f->L[0] = newlabel(p), f->L[1] = newlabel(p);
				call(p, 2, fseq(nodes2(X, Y), 2, fail, f->L[0]));
				return;
			}
			if (streq(op, "<-")) {
				f->L[0] = newlabel(p), f->L[2] = newlabel(p);
				call(p, 2, fseq(nodes2(X, Y), 2, fail, f->L[0]));
				return;
			}
			if (streq(op, "<->")) {
				f->L[0] = newlabel(p), f->L[1] = newlabel(p), f->L[2] = newlabel(p), f->L[3] = newlabel(p);
				call(p, 2, fseq(nodes2(X, Y), 2, fail, f->L[0]));
				return;
			}
			Buf m = {0};
			bufprintf(&m, "operator %s is not yet supported", op);
			perrorf(p, e, m.b);
			done(p, res3(fail, fail, null(p)));
			return;
		}
		if (f->st == 1) { /* & */
			done(p, res3(r.start, r.resume, r.vs[1]));
			return;
		}
		/* st 2: after the sequence of X and Y */
		Operand *vs = r.vs;
		Label st = r.start, rs = r.resume;
		place(p, f->L[0]);
		if (streq(op, ":=") || streq(op, "&:=")) {
			/* x &:= y is x := (x & y), which is x := y with x
			 * evaluated first. */
			in = instr(OpAssign);
			in.dst = vs[0], in.x = vs[1], in.l1 = succ, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, rs, vs[0]));
			return;
		}
		if (streq(op, ":=:")) {
			Operand t = temp(p);
			in = instr(OpMove);
			in.dst = t, in.x = vs[0];
			emit(p, in);
			Label b = newlabel(p);
			/* Each assignment names the other operand (Args), which
			 * shifts if both are substrings of one string. */
			in = instr(OpAssign);
			in.fn = builtin_ref(BuiltinAsgn, ":=:", 3), in.dst = vs[0], in.x = vs[1], in.args = ops1(vs[1]),
			in.nargs = 1, in.l1 = b, in.l2 = rs;
			emit(p, in);
			place(p, b);
			in = instr(OpAssign);
			in.dst = vs[1], in.x = t, in.args = ops1(vs[0]), in.nargs = 1, in.l1 = succ, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, rs, vs[0]));
			return;
		}
		if (streq(op, "!")) {
			/* p ! L calls p with the elements of list or record L. */
			Operand t = temp(p), h = temp(p);
			in = instr(OpCall);
			in.fn = apply_builtin(), in.dst = t, in.x = vs[0], in.h = h.index, in.args = vs + 1, in.nargs = 1,
			in.l1 = succ, in.l2 = rs;
			emit(p, in);
			Label res = newlabel(p);
			place(p, res);
			in = instr(OpResume);
			in.dst = t, in.h = h.index, in.l1 = succ, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, res, t));
			return;
		}
		if (streq(op, "@:=")) {
			/* x @:= c is x := x @ c. */
			Operand t = temp(p);
			in = instr(OpActivate);
			in.dst = t, in.x = vs[1], in.args = ops1(vs[0]), in.nargs = 1, in.l1 = f->L[1], in.l2 = rs;
			emit(p, in);
			place(p, f->L[1]);
			in = instr(OpAssign);
			in.dst = vs[0], in.x = t, in.l1 = succ, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, rs, vs[0]));
			return;
		}
		if (streq(op, "<-")) {
			/* x <- y assigns y to x; resumed, it restores x's old value
			 * and fails (iconx's rasgn). */
			Operand old = temp(p);
			in = instr(OpMove);
			in.dst = old, in.x = vs[0];
			emit(p, in);
			in = instr(OpAssign);
			in.fn = builtin_ref(BuiltinAsgn, "<-", 2), in.dst = vs[0], in.x = vs[1], in.l1 = succ, in.l2 = rs;
			emit(p, in);
			place(p, f->L[2]);
			in = instr(OpAssign);
			in.dst = vs[0], in.x = old, in.l1 = rs, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, f->L[2], vs[0]));
			return;
		}
		if (streq(op, "<->")) {
			/* x <-> y exchanges x and y; resumed, it restores both and
			 * fails (iconx's rswap). */
			Operand ox = temp(p), oy = temp(p);
			in = instr(OpMove);
			in.dst = ox, in.x = vs[0];
			emit(p, in);
			in = instr(OpMove);
			in.dst = oy, in.x = vs[1];
			emit(p, in);
			in = instr(OpAssign);
			in.fn = builtin_ref(BuiltinAsgn, "<->", 3), in.dst = vs[0], in.x = vs[1], in.args = ops1(vs[1]),
			in.nargs = 1, in.l1 = f->L[1], in.l2 = rs;
			emit(p, in);
			place(p, f->L[1]);
			in = instr(OpAssign);
			in.dst = vs[1], in.x = ox, in.args = ops1(vs[0]), in.nargs = 1, in.l1 = succ, in.l2 = rs;
			emit(p, in);
			place(p, f->L[2]);
			in = instr(OpAssign);
			in.dst = vs[0], in.x = ox, in.args = ops1(vs[1]), in.nargs = 1, in.l1 = f->L[3], in.l2 = rs;
			emit(p, in);
			place(p, f->L[3]);
			in = instr(OpAssign);
			in.dst = vs[1], in.x = oy, in.args = ops1(vs[0]), in.nargs = 1, in.l1 = rs, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, f->L[2], vs[0]));
			return;
		}
		{
			/* x op:= y is x := x op y, evaluating x once. */
			char *base = xstrndup(op, strlen(op) - 2);
			Operand t = temp(p);
			Label b = newlabel(p);
			in = instr(OpOper);
			in.fn = op_builtin(base), in.dst = t, in.args = vs, in.nargs = 2, in.l1 = b, in.l2 = rs;
			emit(p, in);
			place(p, b);
			in = instr(OpAssign);
			in.dst = vs[0], in.x = t, in.l1 = succ, in.l2 = rs;
			emit(p, in);
			done(p, res3(st, rs, vs[0]));
		}
		return;
	}

	/* repalt compiles |e, repeated alternation: e's results, then e's
	 * results again, and so on, until e produces none. O[0] produced,
	 * O[1] null; L[0..4] entry, again, a, efail, reset. */
	case F_REPALT:
		if (f->st == 0) {
			f->O[0] = temp(p);
			for (int i = 0; i < 5; i++)
				f->L[i] = newlabel(p);
			f->O[1] = null(p);
			place(p, f->L[0]);
			in = instr(OpMove);
			in.dst = f->O[0], in.x = f->O[1];
			emit(p, in);
			jump(p, f->L[1]);
			call(p, 1, fexpr(f->x, f->L[3], f->L[2]));
			return;
		}
		bind(p, f->L[1], r.start);
		place(p, f->L[3]);
		in = instr(OpIsNull);
		in.x = f->O[0], in.l1 = f->fail, in.l2 = f->L[4];
		emit(p, in);
		place(p, f->L[4]);
		in = instr(OpMove);
		in.dst = f->O[0], in.x = f->O[1];
		emit(p, in);
		jump(p, f->L[1]);
		place(p, f->L[2]);
		in = instr(OpMove);
		in.dst = f->O[0], in.x = intconst(p->c, 1);
		emit(p, in);
		{
			Operand t = temp(p);
			in = instr(OpMoveVar);
			in.dst = t, in.x = r.v;
			emit(p, in);
			jump(p, f->succ);
			done(p, res3(f->L[0], r.resume, t));
		}
		return;

	/* alt compiles x | y. A gate records which alternative to resume.
	 * O[0] t, O[1] gate; L[0..2] ys, a, b. */
	case F_ALT:
		switch (f->st) {
		case 0:
			f->O[0] = temp(p), f->O[1] = temp(p);
			f->L[0] = newlabel(p), f->L[1] = newlabel(p), f->L[2] = newlabel(p);
			call(p, 1, fexpr(f->e->binary.x, f->L[0], f->L[1]));
			return;
		case 1:
			f->R[0] = r;
			call(p, 2, fexpr(f->e->binary.y, f->fail, f->L[2]));
			return;
		}
		bind(p, f->L[0], r.start);
		produce(p, f->L[1], f->O[1], f->R[0].resume, f->O[0], f->R[0].v, f->succ);
		produce(p, f->L[2], f->O[1], r.resume, f->O[0], r.v, f->succ);
		{
			Label res = newlabel(p);
			place(p, res);
			in = instr(OpIGoto);
			in.x = f->O[1];
			emit(p, in);
			done(p, res3(f->R[0].start, res, f->O[0]));
		}
		return;

	/* ifExpr: O[0] t, O[1] gate; L[0] then, L[1] els, L[2] b, L[3] c;
	 * R[0] the condition's, R[1] the body's. */
	case F_IF: {
		Node *e = f->e;
		switch (f->st) {
		case 0:
			f->O[0] = temp(p), f->O[1] = temp(p);
			f->L[0] = newlabel(p);
			f->L[1] = f->fail;
			if (e->if_.alt)
				f->L[1] = newlabel(p);
			call(p, 1, fexpr(e->if_.cond, f->L[1], f->L[0]));
			return;
		case 1:
			f->R[0] = r;
			f->L[2] = newlabel(p);
			call(p, 2, fexpr(e->if_.body, f->fail, f->L[2]));
			return;
		case 2:
			bind(p, f->L[0], r.start);
			produce(p, f->L[2], f->O[1], r.resume, f->O[0], r.v, f->succ);
			if (e->if_.alt) {
				f->L[3] = newlabel(p);
				call(p, 3, fexpr(e->if_.alt, f->fail, f->L[3]));
				return;
			}
			break;
		case 3:
			bind(p, f->L[1], r.start);
			produce(p, f->L[3], f->O[1], r.resume, f->O[0], r.v, f->succ);
			break;
		}
		Label res = newlabel(p);
		place(p, res);
		in = instr(OpIGoto);
		in.x = f->O[1];
		emit(p, in);
		done(p, res3(f->R[0].start, res, f->O[0]));
		return;
	}

	/* loop: the loop is the context of break and next in both its
	 * control expression and its body ("while break" exits the while).
	 * L[0] body, L[1] top, L[2] cond; R[0] the result before the
	 * deferred code; i the loop's control expression's resume (every). */
	case F_LOOP: {
		Node *e = f->e;
		Loop *l = f->loop;
		TokKind k = e->loop.keyword->kind;
		Res out;
		switch (f->st) {
		case 0:
			l = f->loop = xcalloc(1, sizeof *l);
			l->next = newlabel(p);
			l->fail = f->fail;
			l->succ = f->succ;
			l->result = temp(p);
			l->gate = temp(p);
			l->resume = newlabel(p);
			l->scans = nscans(p);
			{
				LoopNode *ln = xmalloc(sizeof *ln);
				ln->l = l;
				ln->next = p->loops;
				p->loops = ln;
			}
			if (k == T_EVERY) {
				f->L[0] = newlabel(p);
				call(p, 1, fexpr(e->loop.cond, f->fail, f->L[0]));
				return;
			}
			if (k == T_WHILE || k == T_UNTIL || k == T_REPEAT) {
				f->L[1] = newlabel(p), f->L[2] = newlabel(p);
				place(p, f->L[1]);
				jump(p, f->L[2]);
				bind(p, l->next, f->L[1]);
				f->L[0] = newlabel(p);
				if (k == T_WHILE) {
					call(p, 3, fexpr(e->loop.cond, f->fail, f->L[0]));
					return;
				}
				if (k == T_UNTIL) {
					call(p, 3, fexpr(e->loop.cond, f->L[0], f->fail));
					return;
				}
				bind(p, f->L[2], f->L[0]);
				call(p, 4, fexpr(e->loop.body, f->L[1], f->L[1]));
				return;
			}
			perrorf(p, e, "invalid loop");
			out = res3(f->fail, f->fail, null(p));
			break;
		case 1: /* every: after the control expression */
			f->R[0] = r;
			bind(p, l->next, r.resume);
			call(p, 2, fexpr(e->loop.body, r.resume, r.resume));
			return;
		case 2:
			bind(p, f->L[0], r.start);
			out = res3(f->R[0].start, l->resume, l->result);
			break;
		case 3: /* while, until: after the control expression */
			bind(p, f->L[2], r.start);
			call(p, 4, fexpr(e->loop.body, f->L[1], f->L[1]));
			return;
		case 4:
			bind(p, f->L[0], r.start);
			out = res3(f->L[1], l->resume, l->result);
			break;
		default:
			return;
		}
		/* deferred: resuming the loop resumes the break expression that
		 * ended it */
		p->loops = p->loops->next;
		if (l->broken) {
			place(p, l->resume);
			in = instr(OpIGoto);
			in.x = l->gate;
			emit(p, in);
		} else
			bind(p, l->resume, l->fail);
		done(p, out);
		return;
	}

	/* brk compiles break [e]: the loop produces e's results, e being
	 * evaluated in the loop's context; resuming the loop resumes e.
	 * L[0] a. */
	case F_BREAK: {
		Node *e = f->e;
		if (f->st == 0) {
			if (!p->loops) {
				perrorf(p, e, "invalid context for break");
				done(p, res3(f->fail, f->fail, null(p)));
				return;
			}
			Loop *l = f->loop = p->loops->l;
			l->broken = true;
			f->L[0] = newlabel(p);
			saveloopsscans(p, f);
			p->loops = p->loops->next;
			while (nscans(p) > l->scans)
				p->scans = p->scans->next;
			call(p, 1, fexpr(e->break_.x, l->fail, f->L[0]));
			return;
		}
		Loop *l = f->loop;
		restoreloopsscans(p, f);
		place(p, f->L[0]);
		in = instr(OpSetLabel);
		in.dst = l->gate, in.l1 = r.resume;
		emit(p, in);
		in = instr(OpMoveVar);
		in.dst = l->result, in.x = r.v;
		emit(p, in);
		jump(p, l->succ);
		{
			Label lv = leave(p, r.start, l->scans);
			done(p, res3(lv, f->fail, null(p)));
		}
		return;
	}

	/* ret: leaving the procedure from inside scans restores the
	 * caller's scanning environment first, as iconx does; values are
	 * then dereferenced, so "return &subject" returns the caller's
	 * subject. The instructions that leave carry the line of the
	 * keyword, which is where &trace reports them, as iconx does. L[0]
	 * a; R[0] the value's. */
	case F_RET: {
		Node *e = f->e;
		Token *kw = e->ret.keyword;
		switch (f->st) {
		case 0:
			if (p->inco > 0) {
				/* As icont: a co-expression's body cannot leave its
				 * procedure. */
				perrorf(p, e, kw->kind == T_SUSPEND ? "invalid context for suspend"
								    : "invalid context for return or fail");
				done(p, res3(f->fail, f->fail, null(p)));
				return;
			}
			if (kw->kind == T_FAIL) {
				Label l = failat(p, kw);
				done(p, res3(l, f->fail, null(p)));
				return;
			}
			if (kw->kind == T_RETURN) {
				f->L[0] = newlabel(p);
				Label fl = failat(p, kw);
				call(p, 1, fexpr(e->ret.x, fl, f->L[0]));
				return;
			}
			if (kw->kind == T_SUSPEND) {
				f->L[0] = newlabel(p);
				call(p, 2, fexpr(e->ret.x, f->fail, f->L[0]));
				return;
			}
			perrorf(p, e, "invalid return");
			done(p, res3(f->fail, f->fail, null(p)));
			return;
		case 1: /* return */
			place(p, f->L[0]);
			for (ScanNode *n = p->scans; n; n = n->next)
				scanswap(p, n->env);
			setline(p, kw);
			in = instr(OpReturn);
			in.x = r.v;
			emit(p, in);
			done(p, res3(r.start, f->fail, null(p)));
			return;
		case 2: /* suspend: after the value */
			f->R[0] = r;
			f->R[1].start = r.resume; /* the resume */
			if (e->ret.body) {
				call(p, 3, fexpr(e->ret.body, r.resume, r.resume));
				return;
			}
			break;
		case 3:
			f->R[1].start = r.start;
			break;
		}
		place(p, f->L[0]);
		for (ScanNode *n = p->scans; n; n = n->next)
			scanswap(p, n->env);
		Label back = newlabel(p);
		setline(p, kw);
		in = instr(OpSuspend);
		in.x = f->R[0].v, in.l1 = back;
		emit(p, in);
		place(p, back);
		reenter(p);
		jump(p, f->R[1].start);
		done(p, res3(f->R[0].start, f->fail, null(p)));
		return;
	}

	/* caseExpr compiles case x of { ... }: x's first value is compared
	 * (===) with each clause's labels in turn, the labels being
	 * generators; the default clause is tried last. O[0] res, O[1]
	 * gate, O[2] x; L[0] a, L[1] next, L[2] after, L[3] m, L[4] bs, L[5]
	 * b (a body's success); R[0] x's; i the clause; x the default. */
	case F_CASE: {
		Node *e = f->e;
		NodeVec *cls = &e->case_.clauses.elems;
		switch (f->st) {
		case 0:
			f->O[0] = temp(p), f->O[1] = temp(p), f->O[2] = temp(p);
			f->L[0] = newlabel(p);
			call(p, 1, fexpr(e->case_.x, f->fail, f->L[0]));
			return;
		case 1:
			f->R[0] = r;
			place(p, f->L[0]);
			in = instr(OpMove);
			in.dst = f->O[2], in.x = r.v;
			emit(p, in);
			f->L[1] = newlabel(p);
			jump(p, f->L[1]);
			f->x = NULL;
			f->i = 0;
			break;
		case 2: { /* after a clause's label */
			bind(p, f->L[1], r.start);
			place(p, f->L[3]);
			f->L[4] = newlabel(p);
			in = instr(OpOper);
			Operand t = temp(p);
			in.fn = op_builtin("==="), in.dst = t, in.args = ops2(f->O[2], r.v), in.nargs = 2,
			in.l1 = f->L[4], in.l2 = r.resume;
			emit(p, in);
			/* body(cl) */
			f->L[5] = newlabel(p);
			call(p, 3, fexpr(cls->v[f->i]->clause.body, f->fail, f->L[5]));
			return;
		}
		case 3: /* after a clause's body */
			produce(p, f->L[5], f->O[1], r.resume, f->O[0], r.v, f->succ);
			bind(p, f->L[4], r.start);
			f->L[1] = f->L[2];
			f->i++;
			break;
		case 4: /* after the default's body */
			produce(p, f->L[5], f->O[1], r.resume, f->O[0], r.v, f->succ);
			bind(p, f->L[1], r.start);
			goto caseend;
		}
		for (; f->i < (int)cls->n; f->i++) {
			Node *cl = cls->v[f->i];
			if (!cl)
				continue;
			if (cl->clause.default_) {
				f->x = cl;
				continue;
			}
			f->L[2] = newlabel(p), f->L[3] = newlabel(p);
			call(p, 2, fexpr(cl->clause.label, f->L[2], f->L[3]));
			return;
		}
		if (f->x) {
			f->L[5] = newlabel(p);
			call(p, 4, fexpr(f->x->clause.body, f->fail, f->L[5]));
			return;
		}
		bind(p, f->L[1], f->fail);
	caseend: {
		Label rl = newlabel(p);
		place(p, rl);
		in = instr(OpIGoto);
		in.x = f->O[1];
		emit(p, in);
		done(p, res3(f->R[0].start, rl, f->O[0]));
	}
		return;
	}

	/* create compiles create e. The body is a separate region of the
	 * procedure's code, run in a frame of its own with copies of the
	 * creator's locals: each result is produced by OpCoRet, and
	 * exhaustion is OpCoFail. L[0] a, L[1] done. */
	case F_CREATE:
		if (f->st == 0) {
			saveloopsscans(p, f);
			p->loops = NULL; /* break and next cannot leave a co-expression */
			p->scans = NULL;
			p->inco++;
			f->L[0] = newlabel(p), f->L[1] = newlabel(p);
			call(p, 1, fexpr(f->e->create.x, f->L[1], f->L[0]));
			return;
		}
		p->inco--;
		restoreloopsscans(p, f);
		place(p, f->L[0]);
		in = instr(OpCoRet);
		in.x = r.v, in.l1 = r.resume;
		emit(p, in);
		place(p, f->L[1]);
		emit(p, instr(OpCoFail));
		{
			Label s = newlabel(p);
			place(p, s);
			Operand t = temp(p);
			in = instr(OpCreate);
			in.dst = t, in.l1 = r.start;
			emit(p, in);
			jump(p, f->succ);
			done(p, res3(s, f->fail, t));
		}
		return;

	/* activate compiles x @ c, or @c if x is NULL (transmitting &null).
	 * L[0] a. */
	case F_ACTIVATE:
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			if (f->x)
				call(p, 1, fseq(nodes2(f->x, f->y), 2, f->fail, f->L[0]));
			else
				call(p, 1, fseq(nodes1(f->y), 1, f->fail, f->L[0]));
			return;
		}
		place(p, f->L[0]);
		{
			Operand t = temp(p);
			in = instr(OpActivate);
			in.dst = t, in.x = r.vs[r.nvs - 1], in.l1 = f->succ, in.l2 = r.resume;
			if (f->x)
				in.args = ops1(r.vs[0]), in.nargs = 1;
			emit(p, in);
			done(p, res3(r.start, r.resume, t));
		}
		return;

	/* scan compiles x ? y (or x ?:= y when flag is set, which assigns the
	 * result to x), following Jcon: entering y saves the outer scanning
	 * environment; when y produces a value, the environments are
	 * exchanged, so the outer one is current; resuming y exchanges them
	 * back; when y fails, the outer one is restored. O[0] env; L[0] a,
	 * L[1] b, L[2] d, L[3] res; R[0] x's. */
	case F_SCAN:
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			call(p, 1, fexpr(f->x, f->fail, f->L[0]));
			return;
		}
		if (f->st == 1) {
			f->R[0] = r;
			f->O[0] = temp(p);
			f->L[1] = newlabel(p), f->L[2] = newlabel(p), f->L[3] = newlabel(p);
			place(p, f->L[0]);
			in = instr(OpScanEnter);
			in.dst = f->O[0], in.x = r.v, in.l2 = r.resume;
			emit(p, in);
			pushscan(p, f->O[0]);
			call(p, 2, fenter(fexpr(f->y, f->L[2], f->L[1])));
			return;
		}
		p->scans = p->scans->next;
		/* y succeeded: back to the outer environment. */
		place(p, f->L[1]);
		scanswap(p, f->O[0]);
		{
			Operand v = r.v;
			if (f->flag) {
				Operand t = temp(p);
				in = instr(OpMove);
				in.dst = t, in.x = r.v;
				emit(p, in);
				in = instr(OpAssign);
				in.dst = f->R[0].v, in.x = t, in.l1 = f->succ, in.l2 = f->L[3];
				emit(p, in);
				v = f->R[0].v;
			} else
				jump(p, f->succ);
			/* Resumed (or the assignment failed): back into y's
			 * environment. */
			place(p, f->L[3]);
			scanswap(p, f->O[0]);
			jump(p, r.resume);
			/* y failed: restore the outer environment. */
			place(p, f->L[2]);
			scanswap(p, f->O[0]);
			jump(p, f->R[0].resume);
			done(p, res3(f->R[0].start, f->L[3], v));
		}
		return;

	/* limit compiles e \ n (x \ y): n is evaluated first, then e
	 * produces at most n results; when e's results run out, n is
	 * resumed and e starts again. O[0..2] lim, cnt, junk; L[0..3] a, b,
	 * c, res; R[0] n's. */
	case F_LIMIT:
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			call(p, 1, fexpr(f->y, f->fail, f->L[0]));
			return;
		}
		if (f->st == 1) {
			f->R[0] = r;
			f->O[0] = temp(p), f->O[1] = temp(p), f->O[2] = temp(p);
			f->L[1] = newlabel(p), f->L[2] = newlabel(p), f->L[3] = newlabel(p);
			place(p, f->L[0]);
			in = instr(OpOper);
			in.fn = op_builtin("limit"), in.dst = f->O[0], in.args = ops1(r.v), in.nargs = 1, in.l1 = f->L[1],
			in.l2 = r.resume;
			emit(p, in);
			place(p, f->L[1]);
			in = instr(OpMove);
			in.dst = f->O[1], in.x = intconst(p->c, 0);
			emit(p, in);
			call(p, 2, fenter(fexpr(f->x, r.resume, f->L[2])));
			return;
		}
		/* e produced a result: count it. */
		place(p, f->L[2]);
		in = instr(OpOper);
		in.fn = op_builtin("+"), in.dst = f->O[1];
		{
			Operand one = intconst(p->c, 1);
			in.args = ops2(f->O[1], one), in.nargs = 2, in.l1 = f->succ, in.l2 = f->succ;
		}
		emit(p, in);
		/* Resumed: resume e if the limit allows, else resume n. */
		place(p, f->L[3]);
		in = instr(OpOper);
		in.fn = op_builtin("<"), in.dst = f->O[2], in.args = ops2(f->O[1], f->O[0]), in.nargs = 2,
		in.l1 = r.resume, in.l2 = f->R[0].resume;
		emit(p, in);
		done(p, res3(f->R[0].start, f->L[3], r.v));
		return;

	/* Index: x[i, j] is x[i][j]; x[] is x[&null], as in icont, whose
	 * empty subscript list is one empty expression. L[0] a. */
	case F_INDEX: {
		Node *e = f->e;
		if (f->st == 0) {
			Node **subs = e->index.list.elems.v;
			size_t n = e->index.list.elems.n;
			if (n == 0) {
				subs = nodes1(NULL);
				n = 1;
			}
			f->L[0] = newlabel(p);
			call(p, 1, fseq(prepend(e->index.x, subs, n), (int)n + 1, f->fail, f->L[0]));
			return;
		}
		place(p, f->L[0]);
		Operand x = r.vs[0];
		for (int i = 1; i < r.nvs; i++) {
			Operand t = temp(p);
			Label next = newlabel(p);
			in = instr(OpOper);
			in.fn = op_builtin("[]"), in.dst = t, in.args = ops2(x, r.vs[i]), in.nargs = 2, in.l1 = next,
			in.l2 = r.resume;
			emit(p, in);
			place(p, next);
			x = t;
		}
		jump(p, f->succ);
		done(p, res3(r.start, r.resume, x));
		return;
	}

	/* x[i+:j] is x[i:i+j] and x[i-:j] is x[i:i-j], the arithmetic being
	 * an operation of its own, as icont compiles it. L[0] a, L[1] b. */
	case F_SECTION: {
		Node *e = f->e;
		if (f->st == 0) {
			f->L[0] = newlabel(p), f->L[1] = newlabel(p);
			call(p, 1, fseq(nodes3(e->section.x, e->section.lo, e->section.hi), 3, f->fail, f->L[0]));
			return;
		}
		place(p, f->L[0]);
		Operand hi = temp(p);
		in = instr(OpOper);
		in.fn = op_builtin(e->section.op->kind == T_MCOLON ? "-" : "+"), in.dst = hi,
		in.args = ops2(r.vs[1], r.vs[2]), in.nargs = 2, in.l1 = f->L[1], in.l2 = r.resume;
		emit(p, in);
		place(p, f->L[1]);
		Operand t = temp(p);
		in = instr(OpOper);
		Operand *a3 = xmalloc(3 * sizeof *a3);
		a3[0] = r.vs[0], a3[1] = r.vs[1], a3[2] = hi;
		in.fn = op_builtin("[:]"), in.dst = t, in.args = a3, in.nargs = 3, in.l1 = f->succ, in.l2 = r.resume;
		emit(p, in);
		done(p, res3(r.start, r.resume, t));
		return;
	}

	/* Call: L[0] a. */
	case F_CALLX: {
		Node *e = f->e;
		if (f->st == 0) {
			f->L[0] = newlabel(p);
			call(p, 1, fseq(prepend(e->call.fun, e->call.args.elems.v, e->call.args.elems.n),
					   (int)e->call.args.elems.n + 1, f->fail, f->L[0]));
			return;
		}
		place(p, f->L[0]);
		setline(p, e->call.open); /* the call's line, for &trace */
		Label res = callinstr(p, r.vs[0], r.vs + 1, r.nvs - 1, r.resume, f->succ);
		done(p, res3(r.start, res, lasttemp(p, 2)));
		return;
	}
	}
}

/* run runs the machine from frame start to its result. */
static Res run(PComp *p, Frame start) {
	p->stk.n = 0;
	VPUSH(p->stk, start);
	while (p->stk.n)
		step(p);
	return p->ret;
}

/* ------------------------------------------------------------------ */

static int cmplocals(const void *a, const void *b) {
	const Symbol *x = *(Symbol *const *)a, *y = *(Symbol *const *)b;
	if (x->decl && y->decl && x->decl->offset != y->decl->offset)
		return x->decl->offset < y->decl->offset ? -1 : 1;
	return strcmp(x->name, y->name);
}

static Proc *compileproc(Compiler *c, Node *d, CompileFile *cf) {
	PComp pc = {0};
	PComp *p = &pc;
	p->c = c;
	p->info = cf->info;
	p->sf = cf->info->file;
	p->map = cf->map;
	if (!p->map)
		p->lines = lines_new(p->sf->syn->src, p->sf->syn->n);
	p->file = p->sf->path;
	p->decl = d;
	Proc *proc = xcalloc(1, sizeof *proc);
	proc->name = xstrndup(p->sf->syn->src + d->proc.name->offset, d->proc.name->len);
	proc->variadic = d->proc.lbrack != NULL;
	proc->file = "";
	p->proc = proc;
	setline(p, d->proc.name);
	proc->file = p->file;

	/* Parameters first, then the other locals in name order; statics
	 * live in globals. */
	VEC(Symbol *) order = {0}; /* the locals by index */
	for (size_t i = 0; i < d->proc.params.elems.n; i++) {
		Token *t = d->proc.params.elems.v[i];
		Symbol *s = t ? p->info->uses[t - p->sf->syn->toks.v] : NULL;
		if (s) {
			if (!strmap_get(&p->locals, (const char *)&s, sizeof s))
				VPUSH(order, s);
			strmap_put(&p->locals, (const char *)&s, sizeof s, (void *)(uintptr_t)order.n);
		}
	}
	proc->nparams = (int)order.n;
	/* Locals and statics in the order of their declarations (for an
	 * implicit local, its first use), the order in which iconx's
	 * display() lists them. */
	ProcLocals *pl = sema_locals(p->info, d);
	size_t ns = pl ? pl->syms.n : 0;
	Symbol **syms = xmalloc((ns + 1) * sizeof *syms);
	for (size_t i = 0; i < ns; i++)
		syms[i] = pl->syms.v[i];
	if (ns)
		qsort(syms, ns, sizeof *syms, cmplocals);
	for (size_t i = 0; i < ns; i++) {
		Symbol *s = syms[i];
		if (strmap_get(&p->locals, (const char *)&s, sizeof s))
			continue;
		switch (s->kind) {
		case S_STATIC:
			VPUSH(proc->statics, global(c, catname(proc->name, s->name), nullv()));
			break;
		case S_LOCAL:
		case S_IMPLICIT:
			VPUSH(order, s);
			strmap_put(&p->locals, (const char *)&s, sizeof s, (void *)(uintptr_t)order.n);
			break;
		default:
			break;
		}
	}
	proc->nlocals = (int)order.n;
	for (size_t i = 0; i < order.n; i++)
		VPUSH(proc->localnames, order.v[i]->name);

	p->pfail = newlabel(p);
	Label entry = newlabel(p);
	place(p, entry);
	if (d->proc.initial) {
		/* initial: run once, on the first call. */
		int flag = global(c, catname(proc->name, "initial"), nullv());
		Label after = newlabel(p);
		Instr in = instr(OpOnce);
		in.x.kind = GlobalOp, in.x.index = flag, in.l1 = after;
		emit(p, in);
		run(p, fstmt(d->proc.initial->initial.x, after));
		place(p, after);
	}
	Frame body = frame(F_STMTS);
	body.es = d->proc.body.elems.v, body.nes = (int)d->proc.body.elems.n, body.succ = p->pfail;
	run(p, body);
	place(p, p->pfail);
	setline(p, d->proc.end); /* falling off the end fails at "end", for &trace */
	emit(p, instr(OpFail));

	proc->code = link(p);
	proc->ntemps = p->ntemps;
	return proc;
}

static void collectname(void *ctx, const char *key, size_t n, void *val) {
	(void)val;
	VPUSH(*(StrVec *)ctx, xstrndup(key, n));
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct ProcInfo {
	Node *decl;
	CompileFile *cf;
} ProcInfo;

/* invocable records which globals string invocation can find. icont's
 * linker discards the globals that main does not reach, directly or
 * through the procedures it refers to, unless they are declared
 * invocable (lglob.c, scanrefs); "invocable all" keeps them all. */
static void invocable(Compiler *c) {
	if (c->invocall)
		return;
	size_t n = c->prog->globals.n;
	bool *keep = xcalloc(n + 1, sizeof *keep);
	StrVec work = {0};
	VPUSH(work, "main");
	for (size_t i = 0; i < c->invoc.n; i++)
		VPUSH(work, c->invoc.v[i]);
	while (work.n) {
		const char *name = work.v[--work.n];
		size_t i = (size_t)(uintptr_t)strmap_get(&c->globals, name, strlen(name));
		if (!i || keep[i - 1])
			continue;
		keep[i - 1] = true;
		StrMap *r = strmap_get(&c->refs, name, strlen(name));
		if (r)
			strmap_each(r, collectname, &work);
	}
	c->prog->invocable = keep;
}

Prog *compile_program(Program *sp, CompileFile *files, size_t nfiles, StrVec *errs) {
	Compiler cc = {0};
	Compiler *c = &cc;
	c->prog = xcalloc(1, sizeof *c->prog);
	c->prog->main = -1;
	c->errs = errs;

	/* Globals, procedures and records, in name order. */
	StrVec names = {0};
	strmap_each(&sp->globals, collectname, &names);
	if (names.n)
		qsort(names.v, names.n, sizeof *names.v, cmpstr);
	for (size_t i = 0; i < names.n; i++)
		global(c, names.v[i], nullv());

	VEC(ProcInfo) procs = {0};
	for (size_t k = 0; k < nfiles; k++) {
		CompileFile *cf = &files[k];
		SemaFile *sf = cf->info->file;
		File *syn = sf->syn;
		for (size_t j = 0; j < syn->decls.n; j++) {
			Node *d = syn->decls.v[j];
			switch (d->kind) {
			case N_PROC: {
				ProcInfo pi = {d, cf};
				VPUSH(procs, pi);
				break;
			}
			case N_GLOBAL:
			case N_LINK: /* resolved by the driver, which passes the linked files in */
				break;
			case N_RECORD: {
				if (!d->record.name)
					break;
				RecordType *rt = xcalloc(1, sizeof *rt);
				rt->name = xstrndup(syn->src + d->record.name->offset, d->record.name->len);
				for (size_t i = 0; i < d->record.fields.elems.n; i++) {
					Token *t = d->record.fields.elems.v[i];
					if (t)
						VPUSH(rt->fields, xstrndup(syn->src + t->offset, t->len));
				}
				Value v = {0};
				v.kind = V_RECORD;
				v.record = rt;
				c->prog->globals.v[global(c, rt->name, nullv())] = v;
				break;
			}
			case N_INVOCABLE:
				for (size_t i = 0; i < d->invocable.items.elems.n; i++) {
					Node *it = d->invocable.items.elems.v[i];
					if (!it || !it->invitem.name)
						continue;
					Token *t = it->invitem.name;
					char *name = xstrndup(syn->src + t->offset, t->len);
					if (t->kind == T_STRINGLIT) {
						char *s;
						size_t sn;
						if (!lit_string(syn->src + t->offset, t->len, &s, &sn))
							name = xstrndup(s, sn);
						else
							name = "";
					}
					if (t->kind == T_IDENT) {
						/* As in icont (addinvk), the only identifier
						 * allowed is all. */
						if (!streq(name, "all")) {
							PComp tmp = {0};
							tmp.c = c, tmp.sf = sf, tmp.map = cf->map;
							if (!tmp.map)
								tmp.lines = lines_new(syn->src, syn->n);
							const char *file;
							long line;
							locate(&tmp, t->offset, &file, &line);
							cerrorf(c, file, line, "\"%s\": invalid operand to invocable", name);
						}
						c->invocall = true;
					}
					VPUSH(c->invoc, name);
				}
				break;
			default: {
				PComp tmp = {0};
				tmp.c = c, tmp.sf = sf, tmp.map = cf->map;
				if (!tmp.map)
					tmp.lines = lines_new(syn->src, syn->n);
				long s = start(c, d);
				if (s >= 0) {
					const char *file;
					long line;
					locate(&tmp, (size_t)s, &file, &line);
					cerrorf(c, file, line, "invalid declaration");
				}
			}
			}
		}
	}
	for (size_t i = 0; i < procs.n; i++) {
		if (!procs.v[i].decl->proc.name)
			continue;
		Proc *proc = compileproc(c, procs.v[i].decl, procs.v[i].cf);
		Value v = {0};
		v.kind = V_PROC;
		v.proc = proc;
		size_t g = (size_t)(uintptr_t)strmap_get(&c->globals, proc->name, strlen(proc->name));
		c->prog->globals.v[g - 1] = v;
	}
	size_t m = (size_t)(uintptr_t)strmap_get(&c->globals, "main", 4);
	if (m && c->prog->globals.v[m - 1].kind == V_PROC)
		c->prog->main = (int64_t)m - 1;
	invocable(c);
	return c->prog;
}
