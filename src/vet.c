/*
 * gcon vet (vet.h). The tree is walked with an explicit stack.
 */
#include "vet.h"
#include "doccomment.h"

#include "lit.h"

#include <stdlib.h>
#include <string.h>

typedef struct Kid {
	Token *t;
	Node *n;
} Kid;
typedef VEC(Kid) KidStack;

static void push(void *ctx, Token *t, Node *n) {
	Kid k = {t, n};
	VPUSH(*(KidStack *)ctx, k);
}

/* walk calls f for every node of the file, parents before children. */
typedef void NodeFn(void *ctx, Node *n);
static void walk(File *file, NodeFn *f, void *ctx) {
	KidStack stk = {0}, kids = {0};
	syn_file_children(file, push, &kids);
	for (size_t i = kids.n; i-- > 0;)
		if (kids.v[i].n)
			VPUSH(stk, kids.v[i]);
	while (stk.n) {
		Node *n = stk.v[--stk.n].n;
		f(ctx, n);
		kids.n = 0;
		syn_children(n, push, &kids);
		for (size_t i = kids.n; i-- > 0;)
			if (kids.v[i].n)
				VPUSH(stk, kids.v[i]);
	}
	free(stk.v);
	free(kids.v);
}

typedef struct SymUse {
	Symbol *sym;
	VEC(Token *) calls;
	bool assigned;
} SymUse;

typedef struct Vet {
	Program *p;
	SemaInfo *info;
	File *syn;
	VetOptions opts;
	VetFindings *out;
	bool whole; /* a complete program with main */
	VEC(SymUse) syms;
	VEC(Token *) strinv; /* "name"(...) */
	Node *proc;           /* the procedure being walked */
	VEC(Token *) fields;  /* field names no record has */
	VEC(Node *) fieldprocs; /* the procedure of each */
	VetResolver *resolve;
	void *ctx;
} Vet;

static size_t tokidx(Vet *v, Token *t) { return (size_t)(t - v->syn->toks.v); }
static const char *ttext(Vet *v, Token *t) { return v->syn->src + t->offset; }

static void report(Vet *v, Token *t, char *msg) {
	VetFinding f = {v->info->file, t->offset, t->offset + t->len, msg, false};
	VPUSH(*v->out, f);
}

/* quoted gives a literal as written, shortened if long. */
static char *quoted(Vet *v, Token *t) {
	Buf b = {0};
	if (t->len <= 24)
		bufadd(&b, ttext(v, t), t->len);
	else {
		bufadd(&b, ttext(v, t), 20);
		bufputs(&b, "...");
		bufputc(&b, ttext(v, t)[t->len - 1]);
	}
	return b.b;
}

static SymUse *symuse(Vet *v, Symbol *s) {
	for (size_t i = 0; i < v->syms.n; i++)
		if (v->syms.v[i].sym == s)
			return &v->syms.v[i];
	SymUse u = {0};
	u.sym = s;
	VPUSH(v->syms, u);
	return &v->syms.v[v->syms.n - 1];
}

static Symbol *identsym(Vet *v, Node *n) {
	if (!n || n->kind != N_IDENT || !n->ident.name)
		return NULL;
	return v->info->uses[tokidx(v, n->ident.name)];
}

static bool isnumcmp(TokKind k) {
	switch (k) {
	case T_NMLT: case T_NMLE: case T_NMEQ: case T_NMGT: case T_NMGE: case T_NMNE:
	case T_AUGNMLT: case T_AUGNMLE: case T_AUGNMEQ: case T_AUGNMGT: case T_AUGNMGE: case T_AUGNMNE:
		return true;
	default:
		return false;
	}
}

/* isassign: := :=: <- <-> and the augmented assignments. */
static bool isassign(TokKind k) {
	if (k == T_SWAP || k == T_REVASSIGN || k == T_REVSWAP)
		return true;
	const char *s = tok_name(k);
	size_t n = strlen(s);
	return tok_operator(k) && n >= 2 && !strcmp(s + n - 2, ":=");
}

/* nonnumeric reports whether a string or cset literal certainly does
 * not convert to a number: it has no digit. */
static bool nonnumeric(Vet *v, Token *t) {
	const char *src = ttext(v, t);
	if (t->kind == T_STRINGLIT) {
		char *s;
		size_t n;
		if (lit_string(src, t->len, &s, &n))
			return false;
		for (size_t i = 0; i < n; i++)
			if (s[i] >= '0' && s[i] <= '9')
				return false;
		return true;
	}
	if (t->kind == T_CSETLIT) {
		LitCset c;
		if (lit_cset(src, t->len, &c))
			return false;
		for (size_t i = 0; i < c.nsegs; i++)
			for (size_t j = 0; j < c.seglens[i]; j++)
				if (c.segs[i][j] >= '0' && c.segs[i][j] <= '9')
					return false;
		for (size_t i = 0; i < c.nranges; i++)
			if (c.ranges[i].lo <= '9' && c.ranges[i].hi >= '0')
				return false;
		return true;
	}
	return false;
}

/* ---- setmix: a cset and a uset mixed ---- */

enum { SetUnknown, SetCset, SetUset };

/* builtinnamed reports whether n names the built-in function name (not
 * hidden by a declaration). */
static bool builtinnamed(Vet *v, Node *n, const char *name) {
	Symbol *s = identsym(v, n);
	return s && s->kind == S_BUILTIN && !strcmp(s->name, name);
}

/* settype gives the kind of set n certainly produces, if it is a cset
 * or a uset: literals, the cset keywords and &uset, cset(...) and
 * uset(...), and ~, ++, -- and ** of these (a string literal takes the
 * kind of the other operand). */
static int settype(Vet *v, Node *n) {
	if (!n)
		return SetUnknown;
	switch (n->kind) {
	case N_PAREN:
		return n->group.list.elems.n == 1 ? settype(v, n->group.list.elems.v[0]) : SetUnknown;
	case N_LITERAL:
		if (!n->literal.tok)
			return SetUnknown;
		return n->literal.tok->kind == T_CSETLIT ? SetCset : n->literal.tok->kind == T_USETLIT ? SetUset : SetUnknown;
	case N_KEYWORD: {
		Token *t = n->keyword.name;
		if (!t)
			return SetUnknown;
		static const char *const csets[] = {"cset", "ascii", "digits", "letters", "lcase", "ucase"};
		for (size_t i = 0; i < sizeof csets / sizeof csets[0]; i++)
			if (t->len == strlen(csets[i]) && !memcmp(ttext(v, t), csets[i], t->len))
				return SetCset;
		if (t->len == 4 && !memcmp(ttext(v, t), "uset", 4))
			return SetUset;
		return SetUnknown;
	}
	case N_UNARY:
		return n->unary.op && n->unary.op->kind == T_TILDE ? settype(v, n->unary.x) : SetUnknown;
	case N_BINARY: {
		TokKind op = n->binary.op ? n->binary.op->kind : T_ILLEGAL;
		if (op != T_UNION && op != T_INTER && op != T_DIFF)
			return SetUnknown;
		int x = settype(v, n->binary.x), y = settype(v, n->binary.y);
		bool xs = n->binary.x && n->binary.x->kind == N_LITERAL && n->binary.x->literal.tok &&
			  n->binary.x->literal.tok->kind == T_STRINGLIT;
		bool ys = n->binary.y && n->binary.y->kind == N_LITERAL && n->binary.y->literal.tok &&
			  n->binary.y->literal.tok->kind == T_STRINGLIT;
		if (x == y || ys)
			return x;
		return xs ? y : SetUnknown;
	}
	case N_CALL:
		if (builtinnamed(v, n->call.fun, "cset"))
			return SetCset;
		if (builtinnamed(v, n->call.fun, "uset"))
			return SetUset;
		return SetUnknown;
	default:
		return SetUnknown;
	}
}

static bool mixed(int a, int b) { return (a == SetCset && b == SetUset) || (a == SetUset && b == SetCset); }

/* setmix reports a cset and a uset that meet where gcon raises run-time
 * error 128 (++, --, **, their assignments, bal's arguments, cset of a
 * uset), and === or ~=== of the two, which compare values that can
 * never be identical. */
static void setmix(Vet *v, Node *n) {
	if (n->kind == N_BINARY && n->binary.op) {
		TokKind op = n->binary.op->kind;
		int x = settype(v, n->binary.x), y = settype(v, n->binary.y);
		if (!mixed(x, y))
			return;
		if (op == T_UNION || op == T_INTER || op == T_DIFF || op == T_AUGUNION || op == T_AUGINTER ||
		    op == T_AUGDIFF)
			report(v, n->binary.op, xstrndup("a cset and a uset mixed: run-time error 128 (uset(c) converts a cset)", strlen("a cset and a uset mixed: run-time error 128 (uset(c) converts a cset)")));
		else if (op == T_EQUIV || op == T_NEQUIV)
			report(v, n->binary.op, xstrndup("a cset and a uset are never identical", strlen("a cset and a uset are never identical")));
		return;
	}
	if (n->kind != N_CALL)
		return;
	NodeVec *args = &n->call.args.elems;
	if (builtinnamed(v, n->call.fun, "cset") && args->n >= 1 && settype(v, args->v[0]) == SetUset) {
		report(v, n->call.fun->ident.name, xstrndup("cset of a uset: run-time error 128", strlen("cset of a uset: run-time error 128")));
		return;
	}
	if (builtinnamed(v, n->call.fun, "bal")) {
		int seen = SetUnknown;
		for (size_t i = 0; i < args->n && i < 3; i++) {
			int t = settype(v, args->v[i]);
			if (mixed(seen, t)) {
				report(v, n->call.fun->ident.name, xstrndup("bal with a cset and a uset: run-time error 128", strlen("bal with a cset and a uset: run-time error 128")));
				return;
			}
			if (t != SetUnknown)
				seen = t;
		}
	}
}

static void visit(void *ctx, Node *n) {
	Vet *v = ctx;
	setmix(v, n);
	switch (n->kind) {
	case N_LINK:
		if (!v->opts.link)
			break;
		for (size_t i = 0; i < n->link.files.elems.n; i++) {
			Token *t = n->link.files.elems.v[i];
			if (!t)
				continue;
			VetLinkKind k = v->opts.link(v->opts.linkctx, v->info, t);
			Buf m = {0};
			if (k == VetLinkSelf)
				bufprintf(&m, "link %s: this is the program's own file, so nothing is linked", quoted(v, t));
			else if (k == VetLinkGcon && v->opts.icon95)
				bufprintf(&m, "link %s: a Gcon library module, not in Icon 9.5", quoted(v, t));
			else
				continue;
			report(v, t, m.b);
		}
		break;
	case N_PROC:
		v->proc = n;
		break;
	case N_BINARY: {
		TokKind op = n->binary.op ? n->binary.op->kind : T_ILLEGAL;
		if (isnumcmp(op)) {
			Node *sides[2] = {n->binary.x, n->binary.y};
			for (int i = 0; i < 2; i++) {
				Node *s = sides[i];
				if (s && s->kind == N_LITERAL && s->literal.tok && nonnumeric(v, s->literal.tok)) {
					Buf m = {0};
					bufprintf(&m, "numeric comparison with %s, which is not a number: run-time error 102",
						quoted(v, s->literal.tok));
					report(v, s->literal.tok, m.b);
				}
			}
		} else if (isassign(op)) {
			Symbol *s = identsym(v, n->binary.x);
			if (s)
				symuse(v, s)->assigned = true;
			if (op == T_SWAP || op == T_REVSWAP) {
				s = identsym(v, n->binary.y);
				if (s)
					symuse(v, s)->assigned = true;
			}
		}
		break;
	}
	case N_CALL: {
		Node *f = n->call.fun;
		if (!f)
			break;
		Symbol *s = identsym(v, f);
		if (s && (s->kind == S_LOCAL || s->kind == S_STATIC))
			VPUSH(symuse(v, s)->calls, f->ident.name);
		if (f->kind == N_LITERAL && f->literal.tok && f->literal.tok->kind == T_STRINGLIT)
			VPUSH(v->strinv, f->literal.tok);
		break;
	}
	case N_FIELD: {
		Token *t = n->field.name;
		if (!v->whole || !t)
			break;
		SymVec *fs = v->info->fielduses[tokidx(v, t)];
		if (fs && fs->n == 0 && v->proc) {
			VPUSH(v->fields, t);
			VPUSH(v->fieldprocs, v->proc);
		}
		break;
	}
	case N_KEYWORD: {
		Token *t = n->keyword.name;
		if (!v->opts.icon95 || !t)
			break;
		const Keyword *k = v->info->keywords[tokidx(v, t)];
		if (k && k->gcon) {
			Buf m = {0};
			bufprintf(&m, "&%s: a Gcon keyword, not in Icon 9.5", k->name);
			report(v, t, m.b);
		}
		break;
	}
	case N_IDENT: {
		Symbol *s = identsym(v, n);
		if (v->opts.icon95 && s && s->kind == S_BUILTIN && s->builtin && s->builtin->gcon) {
			Buf m = {0};
			bufprintf(&m, "%s: a Gcon built-in function, not in Icon 9.5", s->name);
			report(v, n->ident.name, m.b);
		}
		break;
	}
	default:
		break;
	}
}

/* literals95 reports gcon's literal syntax: \u and \U escapes, and cset
 * ranges. */
static void literals95(Vet *v) {
	for (size_t i = 0; i < v->syn->toks.n; i++) {
		Token *t = &v->syn->toks.v[i];
		if (!t->implicit && t->kind == T_USETLIT) {
			report(v, t, xstrndup("uset literal: not in Icon 9.5", 29));
			continue;
		}
		if (t->implicit || (t->kind != T_STRINGLIT && t->kind != T_CSETLIT))
			continue;
		const char *s = ttext(v, t);
		bool esc = false;
		for (size_t j = 1; j + 1 < t->len; j++) {
			if (s[j] == '\\') {
				if (s[j + 1] == 'u' || s[j + 1] == 'U') {
					Buf m = {0};
					bufprintf(&m, "\\%c escape: not in Icon 9.5", s[j + 1]);
					report(v, t, m.b);
					esc = true;
					break;
				}
				j++; /* the escaped character */
			}
		}
		if (esc || t->kind != T_CSETLIT)
			continue;
		LitCset c;
		if (!lit_cset(s, t->len, &c) && c.nranges)
			report(v, t, xstrndup("cset range: not in Icon 9.5", 27));
	}
}

typedef VEC(const char *) NameVec;

static void addname(void *ctx, const char *key, size_t n, void *val) {
	(void)val;
	VPUSH(*(NameVec *)ctx, xstrndup(key, n));
}

/* kept computes the procedures and records the linker keeps: main, the
 * names declared invocable, and everything they refer to; NULL if
 * "invocable all" keeps them all (unless ignoreall: then the procedures
 * the program names, as if it were absent). */
static StrMap *kept(Vet *v, bool ignoreall) {
	Program *p = v->p;
	NameVec work = {0};
	VPUSH(work, "main");
	StrMap refs = {0}; /* procedure name -> StrMap of global names */
	for (size_t i = 0; i < p->nfiles; i++) {
		SemaFile *sf = p->files[i];
		File *syn = sf->syn;
		for (size_t j = 0; j < syn->decls.n; j++) {
			Node *d = syn->decls.v[j];
			if (d->kind != N_INVOCABLE)
				continue;
			for (size_t k = 0; k < d->invocable.items.elems.n; k++) {
				Node *it = d->invocable.items.elems.v[k];
				Token *t = it ? it->invitem.name : NULL;
				if (!t)
					continue;
				if (t->kind == T_IDENT) {
					if (!ignoreall)
						return NULL; /* invocable all */
					continue;
				}
				char *s;
				size_t n;
				if (t->kind == T_STRINGLIT && !lit_string(syn->src + t->offset, t->len, &s, &n))
					VPUSH(work, xstrndup(s, n));
			}
		}
		SemaInfo *info = sf == v->info->file ? v->info : v->resolve(v->ctx, sf);
		for (size_t j = 0; j < info->locals.n; j++) {
			Node *proc = info->locals.v[j]->proc;
			Token *name = proc->proc.name;
			if (!name)
				continue;
			/* the procedure's tokens: from its name to its end (or, if it
			 * has none, the end of the file) */
			size_t lo = (size_t)(name - syn->toks.v), hi = syn->toks.n;
			if (proc->proc.end)
				hi = (size_t)(proc->proc.end - syn->toks.v) + 1;
			StrMap *r = strmap_get(&refs, syn->src + name->offset, name->len);
			if (!r) {
				r = xcalloc(1, sizeof *r);
				strmap_put(&refs, syn->src + name->offset, name->len, r);
			}
			for (size_t k = lo; k < hi; k++) {
				Symbol *s = info->uses[k];
				if (s && (s->kind == S_PROCEDURE || s->kind == S_RECORD || s->kind == S_GLOBAL))
					strmap_put(r, s->name, strlen(s->name), (void *)1);
			}
		}
	}
	StrMap *keep = xcalloc(1, sizeof *keep);
	while (work.n) {
		const char *name = work.v[--work.n];
		if (strmap_get(keep, name, strlen(name)))
			continue;
		strmap_put(keep, name, strlen(name), (void *)1);
		StrMap *r = strmap_get(&refs, name, strlen(name));
		if (r)
			strmap_each(r, addname, &work);
	}
	return keep;
}

/* named: another declaration of the file whose doc comment begins with
 * its own name (ctx: a StrMap of them). */
static bool named(void *ctx, const char *name, size_t n) {
	return strmap_get(ctx, name, n) != NULL;
}

/* docwarnings reports doc comments that will not read as their authors
 * meant (doccomment.h): warnings, which do not change vet's exit
 * status. */
static void docwarnings(Vet *v) {
	char *filedoc;
	DocDecls ds = doccomment_file(v->syn->src, v->syn->n, &filedoc);
	DocWarnings ws = {0};
	StrMap selfnamed = {0}; /* declarations whose comments begin with their names */
	for (size_t i = 0; i < ds.n; i++) {
		char *w = doccomment_firstword(&ds.v[i]);
		if (w && !strcmp(w, ds.v[i].name))
			strmap_put(&selfnamed, w, strlen(w), (void *)1);
	}
	for (size_t i = 0; i < ds.n; i++)
		doccomment_lint(&ds.v[i], named, &selfnamed, &ws);
	for (size_t i = 0; i < ws.n; i++) {
		/* the line's span: from its first byte to its end */
		size_t off = 0;
		for (int line = 1; line < ws.v[i].line && off < v->syn->n; off++)
			if (v->syn->src[off] == '\n')
				line++;
		size_t end = off;
		while (end < v->syn->n && v->syn->src[end] != '\n')
			end++;
		VetFinding f = {v->info->file, off, end, ws.v[i].msg, true};
		VPUSH(*v->out, f);
	}
}

static int cmpfinding(const void *a, const void *b) {
	const VetFinding *x = a, *y = b;
	return x->offset < y->offset ? -1 : x->offset > y->offset;
}

void vet_file(Program *p, SemaInfo *info, VetResolver *resolve, void *ctx, VetOptions opts, VetFindings *out) {
	Vet v = {0};
	v.p = p, v.info = info, v.syn = info->file->syn, v.opts = opts, v.resolve = resolve, v.ctx = ctx;
	Symbol *m = sema_global(p, "main", 4);
	v.whole = p->complete && m && m->kind == S_PROCEDURE;
	VetFindings mine = {0};
	v.out = &mine;
	walk(v.syn, visit, &v);
	for (size_t i = 0; i < v.syms.n; i++) {
		SymUse *u = &v.syms.v[i];
		if (u->assigned || !u->calls.n)
			continue;
		Symbol *g = sema_global(p, u->sym->name, strlen(u->sym->name));
		if (!g || (g->kind != S_PROCEDURE && g->kind != S_RECORD && g->kind != S_BUILTIN))
			continue;
		const char *what = g->kind == S_PROCEDURE ? "procedure" : g->kind == S_RECORD ? "record constructor"
												 : "built-in function";
		for (size_t j = 0; j < u->calls.n; j++) {
			Buf b = {0};
			bufprintf(&b, "call of %s \"%s\", which shadows the %s and is never assigned: run-time error 106",
				sym_kind_name(u->sym->kind), u->sym->name, what);
			report(&v, u->calls.v[j], b.b);
		}
	}
	/* Field references only in procedures the program reaches (ignoring
	 * "invocable all", which keeps everything for string invocation):
	 * the others almost certainly never run, and a library's records may
	 * be its callers'. */
	StrMap *reach = v.whole && v.fields.n ? kept(&v, true) : NULL;
	for (size_t i = 0; i < v.fields.n; i++) {
		Token *name = v.fieldprocs.v[i]->proc.name;
		if (!reach || !name || !strmap_get(reach, v.syn->src + name->offset, name->len))
			continue;
		Token *t = v.fields.v[i];
		Buf m = {0};
		bufprintf(&m, "no record has a field \"%.*s\": run-time error 207", (int)t->len, ttext(&v, t));
		report(&v, t, m.b);
	}
	if (v.whole && v.strinv.n) {
		StrMap *keep = kept(&v, false); /* NULL: invocable all */
		for (size_t i = 0; keep && i < v.strinv.n; i++) {
			Token *t = v.strinv.v[i];
			char *s;
			size_t n;
			if (lit_string(ttext(&v, t), t->len, &s, &n))
				continue;
			Symbol *g = sema_global(p, s, n);
			if (!g || (g->kind != S_PROCEDURE && g->kind != S_RECORD) || strmap_get(keep, s, n))
				continue;
			Buf b = {0};
			bufprintf(&b,
				"string invocation of %s \"%.*s\", which the linker discards: run-time error 106 "
				"(declare it invocable)",
				g->kind == S_PROCEDURE ? "procedure" : "record constructor", (int)n, s);
			report(&v, t, b.b);
		}
	}
	docwarnings(&v);
	if (opts.icon95)
		literals95(&v);
	if (mine.n)
		qsort(mine.v, mine.n, sizeof *mine.v, cmpfinding);
	for (size_t i = 0; i < mine.n; i++)
		VPUSH(*out, mine.v[i]);
}
