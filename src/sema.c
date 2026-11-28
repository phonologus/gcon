/*
 * Name resolution (sema.h), transliterated from the Go reference's
 * internal/sema, whose diagnostics are icont's: the same kind of local
 * declared twice is "redeclared identifier", different kinds
 * "inconsistent redeclaration" (tsym.c's install); two procedures or
 * records of a name, in any files, "inconsistent redeclaration"
 * (tsym.c, lglob.c); a duplicate record field "redeclared identifier";
 * an unknown keyword "invalid keyword"; an undeclared identifier a
 * warning (lsym.c, under -u).
 */
#include "sema.h"

#include <stdlib.h>
#include <string.h>

static const char *const kindnames[S_NKINDS] = {
	"parameter", "local", "static", "undeclared local", "global", "procedure",
	"record", "field", "built-in function", "preprocessor symbol",
};

const char *sym_kind_name(SymKind k) { return (unsigned)k < S_NKINDS ? kindnames[k] : "?"; }

static const char *text(SemaFile *f, Token *t) { return f->syn->src + t->offset; }

static size_t tokindex(SemaFile *f, Token *t) { return (size_t)(t - f->syn->toks.v); }

static Symbol *newsym(const char *name, size_t n, SymKind k, SemaFile *f, Token *decl, Node *node) {
	Symbol *s = xcalloc(1, sizeof *s);
	s->name = xstrndup(name, n);
	s->kind = k;
	s->file = f;
	s->decl = decl;
	s->node = node;
	return s;
}

static SymVec *vecat(StrMap *m, const char *name, size_t n) {
	SymVec *v = strmap_get(m, name, n);
	if (!v) {
		v = xcalloc(1, sizeof *v);
		strmap_put(m, name, n, v);
	}
	return v;
}

static void addglobal(Program *p, Token *t, SymKind k, SemaFile *f, Node *n) {
	if (!t || t->kind != T_IDENT)
		return;
	VPUSH(*vecat(&p->globals, text(f, t), t->len), newsym(text(f, t), t->len, k, f, t, n));
}

Program *sema_program(SemaFile **files, size_t n, bool complete) {
	Program *p = xcalloc(1, sizeof *p);
	p->files = files;
	p->nfiles = n;
	p->complete = complete;
	for (size_t i = 0; i < n; i++) {
		SemaFile *f = files[i];
		for (size_t j = 0; j < f->syn->decls.n; j++) {
			Node *d = f->syn->decls.v[j];
			switch (d->kind) {
			case N_GLOBAL:
				for (size_t k = 0; k < d->global.names.elems.n; k++)
					addglobal(p, d->global.names.elems.v[k], S_GLOBAL, f, d);
				break;
			case N_PROC:
				addglobal(p, d->proc.name, S_PROCEDURE, f, d);
				break;
			case N_RECORD:
				addglobal(p, d->record.name, S_RECORD, f, d);
				for (size_t k = 0; k < d->record.fields.elems.n; k++) {
					Token *t = d->record.fields.elems.v[k];
					if (t)
						VPUSH(*vecat(&p->fields, text(f, t), t->len),
							newsym(text(f, t), t->len, S_FIELD, f, t, d));
				}
				break;
			default:
				break;
			}
		}
	}
	return p;
}

static StrMap builtinsyms; /* name -> Symbol *, shared */

/* withgraphics: whether the graphics built-ins exist (sema_graphics) */
static bool withgraphics = true;

void sema_graphics(bool on) { withgraphics = on; }

Symbol *sema_global(Program *p, const char *name, size_t n) {
	SymVec *syms = strmap_get(&p->globals, name, n);
	if (syms) {
		for (size_t i = 0; i < syms->n; i++)
			if (syms->v[i]->kind != S_GLOBAL)
				return syms->v[i];
		if (syms->n)
			return syms->v[0];
	}
	const BuiltinFunc *bf = builtin_function(name, n);
	if (!bf || (bf->graphics && !withgraphics))
		return NULL;
	Symbol *s = strmap_get(&builtinsyms, name, n);
	if (!s) {
		s = newsym(name, n, S_BUILTIN, NULL, NULL, NULL);
		s->builtin = bf;
		strmap_put(&builtinsyms, name, n, s);
	}
	return s;
}

/* before reports whether a's declaration precedes b's, in the order of
 * the program's files and then of position. */
static bool before(Program *p, Symbol *a, Symbol *b) {
	if (a->file != b->file) {
		for (size_t i = 0; i < p->nfiles; i++) {
			if (p->files[i] == a->file)
				return true;
			if (p->files[i] == b->file)
				return false;
		}
	}
	return a->decl->offset < b->decl->offset;
}

/* ------------------------------------------------------------------ */

typedef struct Resolver {
	Program *p;
	SemaInfo *info;
	SemaFile *f;
	Node *proc;
	ProcLocals *locals;
	StrMap macros; /* MacroDef * (as bytes) -> Symbol *: one per definition */
} Resolver;

/* diag records a diagnostic at t: "\"<t's text>\": msg". */
static Diagnostic *diag(Resolver *r, Token *t, Severity sev, const char *msg) {
	Buf b = {0};
	bufputc(&b, '"');
	bufadd(&b, text(r->f, t), t->len);
	bufprintf(&b, "\": %s", msg);
	Diagnostic d = {t->offset, t->offset + t->len, sev, b.b, NULL};
	VPUSH(r->info->diags, d);
	return &r->info->diags.v[r->info->diags.n - 1];
}

static void use(Resolver *r, Token *t, Symbol *s) { r->info->uses[tokindex(r->f, t)] = s; }
static Symbol *usedby(Resolver *r, Token *t) { return r->info->uses[tokindex(r->f, t)]; }

/* globaldeclname records the symbol declared by t and reports
 * inconsistent redeclarations: two procedures or records of the same
 * name, in any files of the program, as icont does. A global
 * declaration may share its name with anything (it is the procedure's
 * or record's global). */
static void globaldeclname(Resolver *r, Token *t, SymKind k) {
	if (!t || t->kind != T_IDENT)
		return;
	SymVec *syms = strmap_get(&r->p->globals, text(r->f, t), t->len);
	Symbol *mine = NULL;
	for (size_t i = 0; syms && i < syms->n; i++)
		if (syms->v[i]->decl == t)
			mine = syms->v[i];
	if (!mine)
		return;
	use(r, t, mine);
	/* Report at each conflicting declaration after the first. */
	for (size_t i = 0; i < syms->n; i++) {
		Symbol *s = syms->v[i];
		if (s == mine || s->kind == S_GLOBAL || k == S_GLOBAL)
			continue;
		if (before(r->p, s, mine)) {
			diag(r, t, SEV_ERROR, "inconsistent redeclaration")->related = s;
			return;
		}
	}
}

/* macro returns the symbol for a preprocessor symbol in force at t, if
 * any: one defined in (or included into) the file, or predefined. */
static StrMap predefdefs; /* name -> MacroDef *, shared */

static Symbol *macro(Resolver *r, Token *t) {
	if (r->f->preprocessed)
		return NULL;
	MacroDef *def = NULL;
	const char *name = text(r->f, t);
	for (size_t i = 0; i < r->f->nmacros; i++) {
		MacroScope *m = &r->f->macros[i];
		if (strlen(m->def->name) == t->len && !memcmp(m->def->name, name, t->len) &&
		    m->from <= t->offset && t->offset < m->until)
			def = m->def;
	}
	if (!def) {
		const Predefined *pd = builtin_predefined(name, t->len);
		if (!pd)
			return NULL;
		def = strmap_get(&predefdefs, name, t->len);
		if (!def) {
			def = xcalloc(1, sizeof *def);
			def->name = pd->name;
			def->value = "1";
			def->path = "";
			def->predefined = pd;
			strmap_put(&predefdefs, name, t->len, def);
		}
	}
	char key[sizeof def];
	memcpy(key, &def, sizeof def);
	Symbol *s = strmap_get(&r->macros, key, sizeof key);
	if (!s) {
		s = newsym(def->name, strlen(def->name), S_MACRO, NULL, NULL, NULL);
		s->macro = def;
		strmap_put(&r->macros, key, sizeof key, s);
	}
	return s;
}

static Symbol *getlocal(Resolver *r, Token *t) {
	return r->locals ? strmap_get(&r->locals->byname, text(r->f, t), t->len) : NULL;
}

static void putlocal(Resolver *r, Symbol *s) {
	strmap_put(&r->locals->byname, s->name, strlen(s->name), s);
	VPUSH(r->locals->syms, s);
}

/* ident resolves an identifier used in an expression. A preprocessor
 * symbol takes precedence over everything: icont substitutes it before
 * parsing. */
static void ident(Resolver *r, Token *t) {
	Symbol *s;
	if ((s = macro(r, t)) || (s = getlocal(r, t)) || (s = sema_global(r->p, text(r->f, t), t->len))) {
		use(r, t, s);
		return;
	}
	s = newsym(text(r->f, t), t->len, S_IMPLICIT, r->f, t, r->proc);
	putlocal(r, s);
	use(r, t, s);
	if (r->p->complete)
		diag(r, t, SEV_WARNING, "undeclared identifier");
}

/* isassignop reports whether k assigns to its left operand. */
static bool isassignop(TokKind k) {
	const char *s = tok_name(k);
	size_t n = strlen(s);
	return !strcmp(s, ":=:") || !strcmp(s, "<-") || !strcmp(s, "<->") || (n >= 2 && !strcmp(s + n - 2, ":="));
}

/* An entry of the expression walk's stack: a node to visit, or (post)
 * one whose children have been visited. */
typedef struct Pending {
	Node *n;
	bool post;
} Pending;

static void pushchild(void *ctx, Token *t, Node *n) {
	(void)t;
	if (n) {
		Pending e = {n, false};
		VPUSH(*(VEC(Pending) *)ctx, e);
	}
}

/* expr resolves an expression and everything in it, in source order. */
static void expr(Resolver *r, Node *x) {
	if (!x)
		return;
	VEC(Pending) stk = {0}, kids = {0};
	Pending root = {x, false};
	VPUSH(stk, root);
	while (stk.n) {
		Pending e = stk.v[--stk.n];
		Node *n = e.n;
		if (e.post) {
			/* Assigning to a built-in function is legal, but replaces
			 * it throughout the program: usually a variable that
			 * happens to share its name (e.g. "name" or "pos").
			 * Assigning to procedures and record constructors is an
			 * Icon idiom, so is not reported. */
			Node *id = n->binary.x;
			Symbol *s;
			if (id && id->kind == N_IDENT && id->ident.name && n->binary.op &&
			    isassignop(n->binary.op->kind) && (s = usedby(r, id->ident.name)) && s->kind == S_BUILTIN) {
				Buf b = {0};
				bufprintf(&b, "assignment to built-in function \"%s\" replaces it throughout the program", s->name);
				Diagnostic d = {id->ident.name->offset, id->ident.name->offset + id->ident.name->len,
					SEV_WARNING, b.b, NULL};
				VPUSH(r->info->diags, d);
			}
			continue;
		}
		switch (n->kind) {
		case N_IDENT:
			if (n->ident.name)
				ident(r, n->ident.name);
			continue;
		case N_KEYWORD: {
			Token *t = n->keyword.name;
			if (!t)
				continue;
			const Keyword *k = builtin_keyword(text(r->f, t), t->len);
			if (!k)
				diag(r, t, SEV_ERROR, "invalid keyword");
			r->info->keywords[tokindex(r->f, t)] = k;
			r->info->iskeyword[tokindex(r->f, t)] = true;
			continue;
		}
		case N_FIELD:
			if (n->field.name) {
				Token *t = n->field.name;
				SymVec *v = strmap_get(&r->p->fields, text(r->f, t), t->len);
				static SymVec none;
				r->info->fielduses[tokindex(r->f, t)] = v ? v : &none;
			}
			break;
		case N_BINARY: {
			Pending post = {n, true};
			VPUSH(stk, post);
			break;
		}
		default:
			break;
		}
		/* Visit the child nodes in source order. */
		kids.n = 0;
		syn_children(n, pushchild, &kids);
		for (size_t i = kids.n; i-- > 0;)
			VPUSH(stk, kids.v[i]);
	}
	free(stk.v);
	free(kids.v);
}

static void procdecl(Resolver *r, Node *d);

static void decl(Resolver *r, Node *d) {
	switch (d->kind) {
	case N_GLOBAL:
		for (size_t i = 0; i < d->global.names.elems.n; i++)
			globaldeclname(r, d->global.names.elems.v[i], S_GLOBAL);
		break;
	case N_RECORD: {
		globaldeclname(r, d->record.name, S_RECORD);
		StrMap seen = {0};
		for (size_t i = 0; i < d->record.fields.elems.n; i++) {
			Token *t = d->record.fields.elems.v[i];
			if (!t)
				continue;
			SymVec *v = strmap_get(&r->p->fields, text(r->f, t), t->len);
			for (size_t j = 0; v && j < v->n; j++)
				if (v->v[j]->decl == t)
					use(r, t, v->v[j]);
			/* As icont, which installs fields as identifiers. */
			if (strmap_get(&seen, text(r->f, t), t->len))
				diag(r, t, SEV_ERROR, "redeclared identifier");
			strmap_put(&seen, text(r->f, t), t->len, t);
		}
		break;
	}
	case N_INVOCABLE:
		for (size_t i = 0; i < d->invocable.items.elems.n; i++) {
			Token *t = d->invocable.items.elems.v[i]->invitem.name;
			Symbol *s;
			if (t && t->kind == T_IDENT && (s = sema_global(r->p, text(r->f, t), t->len)))
				use(r, t, s);
		}
		break;
	case N_PROC:
		globaldeclname(r, d->proc.name, S_PROCEDURE);
		procdecl(r, d);
		break;
	default:
		break;
	}
}

/* declare declares a parameter, local or static: as icont's install,
 * the same kind twice is redeclared, different kinds inconsistent. */
static void declare(Resolver *r, Token *t, SymKind k) {
	if (!t || t->kind != T_IDENT)
		return;
	Symbol *old = getlocal(r, t);
	if (old) {
		diag(r, t, SEV_ERROR, old->kind == k ? "redeclared identifier" : "inconsistent redeclaration")
			->related = old;
		use(r, t, old);
		return;
	}
	Symbol *s = newsym(text(r->f, t), t->len, k, r->f, t, r->proc);
	putlocal(r, s);
	use(r, t, s);
}

static void procdecl(Resolver *r, Node *d) {
	r->proc = d;
	r->locals = xcalloc(1, sizeof *r->locals);
	r->locals->proc = d;
	VPUSH(r->info->locals, r->locals);
	for (size_t i = 0; i < d->proc.params.elems.n; i++)
		declare(r, d->proc.params.elems.v[i], S_PARAM);
	for (size_t i = 0; i < d->proc.locals.n; i++) {
		Node *l = d->proc.locals.v[i];
		SymKind k = l->local.keyword && l->local.keyword->kind == T_STATIC ? S_STATIC : S_LOCAL;
		for (size_t j = 0; j < l->local.names.elems.n; j++)
			declare(r, l->local.names.elems.v[j], k);
	}
	if (d->proc.initial)
		expr(r, d->proc.initial->initial.x);
	for (size_t i = 0; i < d->proc.body.elems.n; i++)
		expr(r, d->proc.body.elems.v[i]);
	r->proc = NULL;
	r->locals = NULL;
}

/* A stable sort of the diagnostics by offset (insertion: they are
 * nearly in order already). */
static void sortdiags(SemaInfo *info) {
	for (size_t i = 1; i < info->diags.n; i++) {
		Diagnostic d = info->diags.v[i];
		size_t j = i;
		for (; j > 0 && info->diags.v[j - 1].offset > d.offset; j--)
			info->diags.v[j] = info->diags.v[j - 1];
		info->diags.v[j] = d;
	}
}

SemaInfo *sema_resolve(Program *p, SemaFile *f) {
	size_t nt = f->syn->toks.n;
	SemaInfo *info = xcalloc(1, sizeof *info);
	info->file = f;
	info->uses = xcalloc(nt, sizeof *info->uses);
	info->fielduses = xcalloc(nt, sizeof *info->fielduses);
	info->keywords = xcalloc(nt, sizeof *info->keywords);
	info->iskeyword = xcalloc(nt, sizeof *info->iskeyword);
	Resolver r = {.p = p, .info = info, .f = f};
	for (size_t i = 0; i < f->syn->decls.n; i++)
		decl(&r, f->syn->decls.v[i]);
	sortdiags(info);
	return info;
}

ProcLocals *sema_locals(SemaInfo *info, Node *proc) {
	for (size_t i = 0; i < info->locals.n; i++)
		if (info->locals.v[i]->proc == proc)
			return info->locals.v[i];
	return NULL;
}
