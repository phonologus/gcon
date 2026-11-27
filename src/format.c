/*
 * The formatter (format.h), transliterated from icontools'
 * internal/format (format.go, align.go, vib.go) and cmd/icfmt, whose
 * output it reproduces. The Go formats by recursion over the tree; here,
 * as in the rest of the C front end, a stack of frames does (each Go
 * function that formats a subexpression is a kind of frame, its states
 * following the function's statements).
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#include "format.h"

#include "help.h"
#include "syntax.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Indent is one level of indentation. */
static const char Indent[] = "   ";

/* marks records alignment points on an output line (byte offsets, -1 if
 * absent): the end of a case label's colon; the end of the code before
 * a binary operator that ends the line; and the end of the code before
 * a trailing comment. */
typedef struct Marks {
	bool present;
	int caseat, casegroup;
	int opat;
	const char *optext;
	size_t oplen;
	int opindent;
	bool oppadded; /* the source had padding before the operator */
	int assignat;  /* before the operator of an assignment statement */
	int assignindent;
	int assignanchor; /* offset within the operator of the column to align */
	bool assignpadded;
	int commentat;
} Marks;

/* tok describes how to place one token. */
typedef struct Place {
	int indent;      /* indentation if the token starts a line */
	bool space;      /* separated from the previous token on the same line */
	bool join;       /* pull onto the previous line if only blank lines intervene */
	bool newline;    /* start a new line even if the source does not */
	bool markop;     /* record an operator alignment point before the token */
	bool markassign; /* record an assignment alignment point before the token */
	int comment;     /* indentation for comment lines before it (-1: use indent) */
} Place;

struct Frame;

/* printer writes tokens, deciding for each whether it starts a new line,
 * its indentation if so, and the spacing before it if not. */
typedef struct Printer {
	File *f;
	Token *toks;
	size_t ntoks;

	Buf b;
	int line;         /* current output line (0-based) */
	size_t linestart; /* offset in b of the current line */
	Token *prev;
	int lineindent;   /* indentation of the current output line */
	Token *linefirst; /* first token on the current output line */
	int *tokline;     /* by token index; -1: not written */

	/* Frame of the innermost statement: continuation lines are indented
	 * to cont. setcont is set until the statement's first token has
	 * been written, which fixes cont relative to that token's line. */
	int cont;
	bool setcont;

	bool hasspace, spaceval; /* if set, overrides the spacing of the next token written */
	bool hasstart;
	int startval; /* if set, indentation for the next token if it starts a line */
	bool hascmt;
	int cmtval;   /* if set, indentation for comment lines before the next token */
	bool forcenl; /* the next token starts a new line */
	bool markop;  /* record the next token as a line-final operator */

	/* assign is the assignment statement being printed, if any; its
	 * operator position is recorded for alignment. */
	Node *assign;
	bool markassign;

	VEC(Marks) marks; /* alignment points, by output line */
	int cases;        /* counter identifying case expressions */

	VEC(struct Frame) stk;
} Printer;

static const char *text(Printer *p, Token *t) { return p->f->src + t->offset; }
static size_t tindex(Printer *p, Token *t) { return (size_t)(t - p->toks); }

static Marks *mark(Printer *p, int line) {
	while ((size_t)line >= p->marks.n) {
		Marks m = {0};
		VPUSH(p->marks, m);
	}
	Marks *m = &p->marks.v[line];
	if (!m->present) {
		m->present = true;
		m->caseat = m->opat = m->assignat = m->commentat = -1;
	}
	return m;
}

static Marks *getmark(Printer *p, int line) {
	if (line < 0 || (size_t)line >= p->marks.n || !p->marks.v[line].present)
		return NULL;
	return &p->marks.v[line];
}

/* The token's output line, as a Go map lookup: 0 if not written. */
static int toklineof(Printer *p, Token *t) {
	if (!t)
		return 0;
	int l = p->tokline[tindex(p, t)];
	return l < 0 ? 0 : l;
}

static void emitstr(Printer *p, const char *s, size_t n) {
	bufadd(&p->b, s, n);
	for (size_t i = 0; i < n; i++)
		if (s[i] == '\n') {
			p->line++;
			p->linestart = p->b.n - (n - i - 1);
		}
}

static void writez(Printer *p, const char *s) { emitstr(p, s, strlen(s)); }

static void indent(Printer *p, int n) {
	for (int i = 0; i < n; i++)
		writez(p, Indent);
}

static int col(Printer *p) { return (int)(p->b.n - p->linestart); }

/* newlines ends the current line and writes a blank line if n > 1. At
 * the start of the file it writes nothing. */
static void newlines(Printer *p, int n) {
	if (p->b.n == 0)
		return;
	writez(p, "\n");
	if (n > 1)
		writez(p, "\n");
}

/* wouldmerge reports whether writing b directly after a would lex
 * differently, e.g. "-" "-x" becoming "--x", or "1" ".x" becoming "1." "x". */
static bool wouldmerge(Printer *p, Token *a, Token *b) {
	Buf s = {0};
	bufadd(&s, text(p, a), a->len);
	bufadd(&s, text(p, b), b->len);
	Tokens ts = lex_tokenize(s.b, s.n);
	bool merge = ts.n != 3 || ts.v[0].len != a->len || memcmp(s.b + ts.v[0].offset, text(p, a), a->len) ||
		ts.v[1].len != b->len || memcmp(s.b + ts.v[1].offset, text(p, b), b->len);
	free(s.b);
	return merge;
}

/* padded reports whether t is preceded in the source, on the same line,
 * by more than a single space: a sign of deliberate alignment. */
static bool padded(Printer *p, Token *t) {
	for (size_t i = 0; i < t->nlead; i++) {
		Trivia *tr = &t->lead[i];
		if (tr->kind == TV_SPACE && (tr->len > 1 || memchr(p->f->src + tr->offset, '\t', tr->len)))
			return true;
	}
	return false;
}

static void emit(Printer *p, Token *t, Place o) {
	if (t->implicit)
		return;
	if (o.comment < 0)
		o.comment = o.indent;
	int nl = 0; /* newlines pending */
	bool commented = false;
	bool col0 = p->prev == NULL; /* at column 0 of a source line */
	for (size_t i = 0; i < t->nlead; i++) {
		Trivia *tr = &t->lead[i];
		switch (tr->kind) {
		case TV_SPACE:
			col0 = false;
			break;
		case TV_NEWLINE:
			nl++;
			col0 = true;
			break;
		case TV_COMMENT:
		case TV_DIRECTIVE: {
			commented = true;
			const char *s = p->f->src + tr->offset;
			size_t n = tr->len;
			while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
				n--;
			if (nl == 0 && p->prev) {
				/* A trailing comment, on the line of the previous token. */
				mark(p, p->line)->commentat = col(p);
				writez(p, " ");
				emitstr(p, s, n);
			} else {
				newlines(p, nl);
				/* Comments are re-indented, except those starting in
				 * column 0, which are kept there (directives always
				 * are). */
				if (tr->kind == TV_COMMENT && !col0)
					indent(p, o.comment);
				emitstr(p, s, n);
			}
			nl = 0;
			col0 = false;
			break;
		}
		}
	}
	if (o.join && !commented)
		nl = 0;
	if (o.newline && nl < 1)
		nl = 1;
	if (t->kind == T_EOF)
		return;
	if (nl > 0 || (commented && p->prev)) {
		newlines(p, nl > 1 ? nl : 1);
		indent(p, o.indent);
		p->lineindent = o.indent;
		p->linefirst = t;
	} else if (!p->prev) {
		p->lineindent = 0;
		p->linefirst = t;
	} else {
		if (o.markop) {
			Marks *m = mark(p, p->line);
			m->opat = col(p), m->optext = text(p, t), m->oplen = t->len, m->opindent = p->lineindent;
			m->oppadded = padded(p, t);
		}
		if (o.markassign) {
			Marks *m = mark(p, p->line);
			m->assignat = col(p), m->assignindent = p->lineindent;
			const char *c = memchr(text(p, t), ':', t->len);
			m->assignanchor = c ? (int)(c - text(p, t)) : 0;
			m->assignpadded = padded(p, t);
		}
		if (o.space || wouldmerge(p, p->prev, t))
			writez(p, " ");
	}
	p->tokline[tindex(p, t)] = p->line;
	emitstr(p, text(p, t), t->len);
	p->prev = t;
	if (p->setcont) {
		p->cont = p->lineindent + 1;
		p->setcont = false;
	}
}

/* put writes t with the default placement: a continuation of the current
 * statement, separated by a space unless sp is false. Pending settings
 * from next, at, cmtat and forcenl apply. */
static void put(Printer *p, Token *t, bool sp) {
	if (!t)
		return;
	Place o = {p->cont, sp, false, false, false, false, -1};
	if (p->hasstart)
		o.indent = p->startval, p->hasstart = false;
	if (p->hasspace)
		o.space = p->spaceval, p->hasspace = false;
	if (p->hascmt)
		o.comment = p->cmtval, p->hascmt = false;
	if (p->forcenl && !t->implicit)
		o.newline = true, p->forcenl = false;
	o.markop = p->markop, p->markop = false;
	o.markassign = p->markassign, p->markassign = false;
	emit(p, t, o);
}

/* next sets the spacing for the next token written; at its indentation,
 * if it starts a line; cmtat the indentation of comment lines before it. */
static void next(Printer *p, bool sp) { p->hasspace = true, p->spaceval = sp; }
static void at(Printer *p, int ind) { p->hasstart = true, p->startval = ind; }
static void cmtat(Printer *p, int ind) { p->hascmt = true, p->cmtval = ind; }

/* multiline reports whether the source has a line break anywhere between
 * tokens a and b (exclusive of the trivia before a). */
static bool multiline(Printer *p, Token *a, Token *b) {
	size_t i = tindex(p, a), j = tindex(p, b);
	for (size_t k = i; k <= j; k++) {
		Token *t = &p->toks[k];
		if (k > i)
			for (size_t m = 0; m < t->nlead; m++)
				if (t->lead[m].kind == TV_NEWLINE)
					return true;
		if (k < j && memchr(text(p, t), '\n', t->len))
			return true;
	}
	return false;
}

/* endsline reports whether t is the last token on its source line. */
static bool endsline(Printer *p, Token *t) {
	for (size_t i = tindex(p, t) + 1; i < p->ntoks; i++) {
		if (p->toks[i].implicit)
			continue;
		for (size_t m = 0; m < p->toks[i].nlead; m++)
			if (p->toks[i].lead[m].kind == TV_NEWLINE)
				return true;
		return false;
	}
	return false;
}

/* prevwritten returns the last non-implicit token before index i. */
static Token *prevwritten(Printer *p, size_t i) {
	while (i-- > 0)
		if (!p->toks[i].implicit)
			return &p->toks[i];
	return NULL;
}

/* startsline reports whether t was written as the first token of a line. */
static bool startsline(Printer *p, Token *t) {
	int l = p->tokline[tindex(p, t)];
	if (l < 0)
		return false;
	size_t i = tindex(p, t);
	return i == 0 || toklineof(p, prevwritten(p, i)) != l;
}

/* joinable reports whether an opening brace t may be pulled up onto the
 * previous line: only if it does not begin a statement, i.e. the source
 * token before it is not a (possibly implicit) semicolon. */
static bool joinable(Printer *p, Token *t) {
	size_t i = tindex(p, t);
	return i > 0 && p->toks[i - 1].kind != T_SEMICOL;
}

/* openbrace writes the "{" of a block or case and returns the
 * indentation of its line. If the braces enclose a line break, the
 * contents are forced onto a new line. */
static int openbrace(Printer *p, Token *lb, Token *rb, bool sp, bool *multi) {
	int ind = p->cont;
	if (p->hasstart)
		ind = p->startval, p->hasstart = false;
	if (p->hasspace)
		sp = p->spaceval, p->hasspace = false;
	Place o = {ind, sp, joinable(p, lb), false, false, false, -1};
	emit(p, lb, o);
	*multi = multiline(p, lb, rb);
	p->forcenl = *multi;
	return p->lineindent;
}

/* closebrace writes "}" Go style: on a line of its own (if the group
 * spans lines) at the indentation of the line holding "{", with comments
 * before it at the contents' indentation. */
static void closebrace(Printer *p, Token *rb, int outer, bool multi, bool sp) {
	p->forcenl = multi;
	at(p, outer);
	cmtat(p, outer + 1);
	put(p, rb, sp);
}

/* keywordat returns the indentation for a "do" that starts a line, given
 * the keyword just written: aligned with it if it begins its line,
 * otherwise a continuation line. */
static int keywordat(Printer *p, Token *kw) { return p->linefirst == kw ? p->lineindent : p->cont; }

/* isassign reports whether k is an assignment operator: :=, :=:, <-,
 * <-> or an augmented assignment such as +:=. */
static bool isassign(TokKind k) {
	const char *s = tok_name(k);
	size_t n = strlen(s);
	return !strcmp(s, ":=:") || !strcmp(s, "<-") || !strcmp(s, "<->") || (n >= 2 && !strcmp(s + n - 2, ":="));
}

/* The first and last tokens of a node (syntax.Tokens(n)[0], [len-1]),
 * down its first or last children. */
typedef struct EndCtx {
	Token *tok;
	Node *node;
	bool first, found;
} EndCtx;

static void endkid(void *ctx, Token *t, Node *n) {
	EndCtx *e = ctx;
	if (e->first && e->found)
		return;
	e->tok = t, e->node = n, e->found = true;
}

static Token *edgetok(Node *n, bool first) {
	/* A node whose chosen child has no tokens would need the next one;
	 * the nodes formatted here always have tokens at both ends. */
	for (;;) {
		EndCtx e = {NULL, NULL, first, false};
		syn_children(n, endkid, &e);
		if (!e.found)
			return NULL;
		if (e.tok)
			return e.tok;
		n = e.node;
	}
}

/* ------------------------------------------------------------------ */
/* The machine */

enum {
	F_EXPR,       /* expr */
	F_STMT,       /* stmt */
	F_SEQ,        /* seq */
	F_ASSIGNSTMT, /* assignStmt */
	F_EXPRS,      /* exprs */
	F_BRACKETED,  /* bracketed */
	F_CASE,       /* caseExpr */
};

typedef struct Frame {
	uint8_t fn, st;
	Node *x;
	NodeList *list;
	Token *open, *close;
	int ind, i, outer, l0, elseat, line, atcol, savecont;
	bool sp, savesetcont, multi;
	Node *saveassign;
	Token *first;
} Frame;

static Frame frame(int fn) {
	Frame f;
	memset(&f, 0, sizeof f);
	f.fn = (uint8_t)fn;
	return f;
}

static Frame fexpr(Node *x) {
	Frame f = frame(F_EXPR);
	f.x = x;
	return f;
}

static Frame fstmt(Node *x, int ind, bool sp) {
	Frame f = frame(F_STMT);
	f.x = x, f.ind = ind, f.sp = sp;
	return f;
}

static Frame fseq(NodeList *l, int ind, bool sp) {
	Frame f = frame(F_SEQ);
	f.list = l, f.ind = ind, f.sp = sp;
	return f;
}

static Frame fbracketed(Token *open, NodeList *l, Token *close) {
	Frame f = frame(F_BRACKETED);
	f.open = open, f.list = l, f.close = close;
	return f;
}

static Frame *top(Printer *p) { return &p->stk.v[p->stk.n - 1]; }

static void call(Printer *p, int st, Frame c) {
	top(p)->st = (uint8_t)st;
	VPUSH(p->stk, c);
}

static void done(Printer *p) { p->stk.n--; }

static void step(Printer *p) {
	Frame *f = top(p);
	switch (f->fn) {

	/* stmt formats x as a statement (or control-structure body) whose
	 * first token, if it starts a line, is indented to ind.
	 * Continuation lines are indented one level beyond the line on which
	 * x starts. */
	case F_STMT:
		if (f->st == 0) {
			if (!f->x) {
				done(p);
				return;
			}
			f->savecont = p->cont, f->savesetcont = p->setcont;
			at(p, f->ind);
			next(p, f->sp);
			p->setcont = true;
			call(p, 1, fexpr(f->x));
			return;
		}
		p->cont = f->savecont, p->setcont = f->savesetcont;
		done(p);
		return;

	/* seq formats a semicolon-separated sequence of statements at ind. */
	case F_SEQ:
		if (f->st == 1) {
			if ((size_t)f->i < f->list->seps.n && f->list->seps.v[f->i]) {
				at(p, f->ind);
				put(p, f->list->seps.v[f->i], false);
			}
			f->i++;
		}
		if ((size_t)f->i < f->list->elems.n) {
			Frame a = frame(F_ASSIGNSTMT);
			a.x = f->list->elems.v[f->i], a.ind = f->ind, a.sp = f->sp || f->i > 0;
			call(p, 1, a);
			return;
		}
		done(p);
		return;

	/* assignStmt is stmt, but if x is an assignment written on a line of
	 * its own, it records the position of the assignment operator for
	 * alignment. */
	case F_ASSIGNSTMT: {
		Node *b = f->x;
		if (f->st == 0) {
			if (!b || b->kind != N_BINARY || !isassign(b->binary.op->kind)) {
				*f = fstmt(b, f->ind, f->sp);
				return;
			}
			f->saveassign = p->assign;
			p->assign = b;
			call(p, 1, fstmt(b, f->ind, f->sp));
			return;
		}
		p->assign = f->saveassign;
		Token *first = edgetok(b, true), *last = edgetok(b, false);
		int line = toklineof(p, b->binary.op);
		Marks *m = getmark(p, line);
		if (m && m->assignat >= 0 &&
		    !(startsline(p, first) && toklineof(p, first) == line && toklineof(p, last) == line &&
			    !memchr(text(p, last), '\n', last->len)))
			m->assignat = -1; /* not a single-line statement on a line of its own */
		done(p);
		return;
	}

	case F_EXPRS:
		if (f->st == 1) {
			if ((size_t)f->i < f->list->seps.n)
				put(p, f->list->seps.v[f->i], false);
			f->i++;
		}
		while ((size_t)f->i < f->list->elems.n) {
			Node *x = f->list->elems.v[f->i];
			if (x) {
				next(p, f->i > 0);
				call(p, 1, fexpr(x));
				return;
			}
			if ((size_t)f->i < f->list->seps.n)
				put(p, f->list->seps.v[f->i], false);
			f->i++;
		}
		done(p);
		return;

	/* bracketed writes open, the list, and close. As in Go, if open ends
	 * its line, the contents are indented one level beyond the line
	 * holding open; and if close starts a line it is indented like the
	 * line holding open. */
	case F_BRACKETED:
		if (f->st == 0) {
			put(p, f->open, false);
			f->outer = p->lineindent;
			f->savecont = p->cont;
			if (endsline(p, f->open))
				p->cont = f->outer + 1;
			Frame e = frame(F_EXPRS);
			e.list = f->list;
			call(p, 1, e);
			return;
		}
		p->cont = f->savecont;
		at(p, f->outer);
		put(p, f->close, false);
		done(p);
		return;

	case F_CASE: {
		Node *x = f->x;
		NodeVec *cls = &x->case_.clauses.elems;
		switch (f->st) {
		case 0:
			put(p, x->case_.case_, false);
			next(p, true);
			call(p, 1, fexpr(x->case_.x));
			return;
		case 1:
			put(p, x->case_.of, true);
			f->outer = openbrace(p, x->case_.lbrace, x->case_.rbrace, true, &f->multi);
			p->cases++;
			f->i = 0;
			break;
		case 2: /* after a label */
			put(p, cls->v[f->i]->clause.colon, false);
			f->line = p->line, f->atcol = col(p);
			call(p, 3, fstmt(cls->v[f->i]->clause.body, f->outer + 2, true));
			return;
		case 3: { /* after a clause's body */
			Node *c = cls->v[f->i];
			p->cont = f->savecont, p->setcont = f->savesetcont;
			/* Record an alignment point for the body of a clause that is
			 * on a line of its own. */
			if (c->clause.body && toklineof(p, f->first) == f->line && startsline(p, f->first) &&
			    p->line == f->line && toklineof(p, edgetok(c->clause.body, true)) == f->line) {
				Marks *m = mark(p, f->line);
				m->caseat = f->atcol, m->casegroup = p->cases;
			}
			if ((size_t)f->i < x->case_.clauses.seps.n && x->case_.clauses.seps.v[f->i])
				put(p, x->case_.clauses.seps.v[f->i], false);
			f->i++;
			break;
		}
		}
		if ((size_t)f->i < cls->n) {
			Node *c = cls->v[f->i];
			f->savecont = p->cont, f->savesetcont = p->setcont;
			at(p, f->outer + 1);
			next(p, f->i > 0 || !f->multi);
			p->setcont = true;
			f->first = c->clause.default_;
			if (f->first) {
				put(p, c->clause.default_, false);
				f->st = 2;
				return; /* on to the colon */
			}
			f->first = edgetok(c->clause.label, true);
			call(p, 2, fexpr(c->clause.label));
			return;
		}
		closebrace(p, x->case_.rbrace, f->outer, f->multi, true);
		done(p);
		return;
	}

	case F_EXPR: {
		Node *x = f->x;
		switch (x->kind) {
		case N_IDENT:
			put(p, x->ident.name, false);
			done(p);
			return;
		case N_LITERAL:
			put(p, x->literal.tok, false);
			done(p);
			return;
		case N_KEYWORD:
			put(p, x->keyword.amp, false);
			put(p, x->keyword.name, false);
			done(p);
			return;
		case N_UNARY:
			put(p, x->unary.op, false);
			next(p, x->unary.op->kind == T_NOT);
			*f = fexpr(x->unary.x);
			return;
		case N_BINARY:
			if (f->st == 0) {
				call(p, 1, fexpr(x->binary.x));
				return;
			}
			p->markop = endsline(p, x->binary.op);
			p->markassign = x == p->assign;
			put(p, x->binary.op, true);
			next(p, true);
			*f = fexpr(x->binary.y);
			return;
		case N_TOBY:
			switch (f->st) {
			case 0:
				call(p, 1, fexpr(x->toby.x));
				return;
			case 1:
				put(p, x->toby.to, true);
				next(p, true);
				if (x->toby.by) {
					call(p, 2, fexpr(x->toby.y));
					return;
				}
				*f = fexpr(x->toby.y);
				return;
			case 2:
				put(p, x->toby.by, true);
				next(p, true);
				*f = fexpr(x->toby.z);
				return;
			}
			return;
		case N_PAREN:
		case N_LISTLIT:
			*f = fbracketed(x->group.open, &x->group.list, x->group.close);
			return;
		case N_BLOCK:
			if (f->st == 0) {
				f->outer = openbrace(p, x->group.open, x->group.close, false, &f->multi);
				call(p, 1, fseq(&x->group.list, f->outer + 1, true));
				return;
			}
			closebrace(p, x->group.close, f->outer, f->multi, x->group.list.elems.n > 0);
			done(p);
			return;
		case N_INDEX:
			if (f->st == 0) {
				call(p, 1, fexpr(x->index.x));
				return;
			}
			*f = fbracketed(x->index.lbrack, &x->index.list, x->index.rbrack);
			return;
		case N_SECTION:
			switch (f->st) {
			case 0:
				call(p, 1, fexpr(x->section.x));
				return;
			case 1:
				put(p, x->section.lbrack, false);
				f->outer = p->lineindent;
				call(p, 2, fexpr(x->section.lo));
				return;
			case 2:
				put(p, x->section.op, false);
				call(p, 3, fexpr(x->section.hi));
				return;
			case 3:
				at(p, f->outer);
				put(p, x->section.rbrack, false);
				done(p);
				return;
			}
			return;
		case N_CALL:
		case N_PDCO:
			if (f->st == 0) {
				call(p, 1, fexpr(x->call.fun));
				return;
			}
			*f = fbracketed(x->call.open, &x->call.args, x->call.close);
			return;
		case N_FIELD:
			if (f->st == 0) {
				call(p, 1, fexpr(x->field.x));
				return;
			}
			put(p, x->field.dot, false);
			put(p, x->field.name, false);
			done(p);
			return;
		case N_CREATE:
			put(p, x->create.create, false);
			next(p, true);
			*f = fexpr(x->create.x);
			return;
		case N_IF:
			switch (f->st) {
			case 0: {
				Token *before = p->prev;
				put(p, x->if_.if_, false);
				f->l0 = p->lineindent;
				/* "then" and "else" starting a line align with "if"
				 * when the if begins its line, or follows an "else"
				 * that does (an else-if chain). Otherwise they are
				 * continuation lines. */
				f->elseat = p->cont;
				if (p->linefirst == x->if_.if_ ||
				    (before && before->kind == T_ELSE && p->linefirst == before))
					f->elseat = f->l0;
				next(p, true);
				call(p, 1, fexpr(x->if_.cond));
				return;
			}
			case 1:
				at(p, f->elseat);
				put(p, x->if_.then, true);
				call(p, 2, fstmt(x->if_.body, f->l0 + 1, true));
				return;
			case 2:
				if (x->if_.else_) {
					at(p, f->elseat);
					put(p, x->if_.else_, true);
					*f = fstmt(x->if_.alt, f->l0 + 1, true);
					return;
				}
				done(p);
				return;
			}
			return;
		case N_LOOP:
		case N_RETURN: {
			Token *kw = x->kind == N_LOOP ? x->loop.keyword : x->ret.keyword;
			Node *head = x->kind == N_LOOP ? x->loop.cond : x->ret.x;
			Token *dotok = x->kind == N_LOOP ? x->loop.do_ : x->ret.do_;
			Node *body = x->kind == N_LOOP ? x->loop.body : x->ret.body;
			if (f->st == 0) {
				put(p, kw, false);
				f->l0 = p->lineindent, f->elseat = keywordat(p, kw);
				if (head) {
					next(p, true);
					call(p, 1, fexpr(head));
					return;
				}
			}
			at(p, f->elseat);
			put(p, dotok, true);
			*f = fstmt(body, f->l0 + 1, true);
			return;
		}
		case N_BREAK:
			put(p, x->break_.break_, false);
			if (x->break_.x) {
				next(p, true);
				*f = fexpr(x->break_.x);
				return;
			}
			done(p);
			return;
		case N_NEXT:
			put(p, x->next.next, false);
			done(p);
			return;
		case N_CASE: {
			Frame c = frame(F_CASE);
			c.x = x;
			*f = c;
			return;
		}
		default:
			fprintf(stderr, "format: unexpected %s\n", node_name(x->kind));
			abort();
		}
	}
	}
}

static void run(Printer *p, Frame start) {
	p->stk.n = 0;
	VPUSH(p->stk, start);
	while (p->stk.n)
		step(p);
}

/* ------------------------------------------------------------------ */
/* Declarations */

/* toklist writes a comma-separated list of single tokens, the first
 * preceded by a space unless next(false) was called. */
static void toklist(Printer *p, TokList *l) {
	for (size_t i = 0; i < l->elems.n; i++) {
		put(p, l->elems.v[i], true);
		if (i < l->seps.n)
			put(p, l->seps.v[i], false);
	}
}

static void proc(Printer *p, Node *d) {
	at(p, 0);
	put(p, d->proc.procedure, false);
	put(p, d->proc.name, true);
	put(p, d->proc.lparen, false);
	next(p, false);
	toklist(p, &d->proc.params);
	put(p, d->proc.lbrack, false);
	put(p, d->proc.rbrack, false);
	at(p, 0);
	put(p, d->proc.rparen, false);
	put(p, d->proc.semi, false);
	for (size_t i = 0; i < d->proc.locals.n; i++) {
		Node *l = d->proc.locals.v[i];
		p->cont = 2;
		at(p, 1);
		put(p, l->local.keyword, true);
		toklist(p, &l->local.names);
		put(p, l->local.semi, false);
	}
	if (d->proc.initial) {
		Node *in = d->proc.initial;
		p->cont = 2;
		at(p, 1);
		put(p, in->initial.initial, true);
		run(p, fstmt(in->initial.x, 2, true));
		put(p, in->initial.semi, false);
	}
	run(p, fseq(&d->proc.body, 1, true));
	Place o = {0, true, false, false, false, false, 1};
	emit(p, d->proc.end, o);
}

static void decl(Printer *p, Node *d) {
	p->cont = 1;
	switch (d->kind) {
	case N_LINK:
		at(p, 0);
		put(p, d->link.link, false);
		toklist(p, &d->link.files);
		break;
	case N_GLOBAL:
		at(p, 0);
		put(p, d->global.global, false);
		toklist(p, &d->global.names);
		break;
	case N_RECORD:
		at(p, 0);
		put(p, d->record.record, false);
		put(p, d->record.name, true);
		put(p, d->record.lparen, false);
		next(p, false);
		toklist(p, &d->record.fields);
		at(p, 0);
		put(p, d->record.rparen, false);
		break;
	case N_INVOCABLE:
		at(p, 0);
		put(p, d->invocable.invocable, false);
		for (size_t i = 0; i < d->invocable.items.elems.n; i++) {
			Node *it = d->invocable.items.elems.v[i];
			put(p, it->invitem.name, true);
			put(p, it->invitem.colon, false);
			put(p, it->invitem.arity, false);
			if (i < d->invocable.items.seps.n)
				put(p, d->invocable.items.seps.v[i], false);
		}
		break;
	case N_PROC:
		proc(p, d);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Alignment (align.go): horizontal space adjusted at the alignment
 * points recorded while printing, in the manner of gofmt. */

typedef struct Line {
	char *s;
	size_t n;
} Line;
typedef VEC(Line) LineVec;

/* width returns the display width of s, counting each rune (or invalid
 * byte) as one column and tabs to the next multiple of 8. */
static int width(const char *s, size_t n) {
	int w = 0;
	for (size_t i = 0; i < n;) {
		int sz;
		int32_t r = utf8decode((const uint8_t *)s + i, n - i, &sz);
		if (r == '\t')
			w = (w / 8 + 1) * 8;
		else
			w++;
		i += (size_t)sz;
	}
	return w;
}

/* padto replaces the blanks at offset at in line so that the text after
 * them starts at column c (0-based). */
static void padto(Line *l, int at, int c) {
	size_t r = (size_t)at;
	while (r < l->n && (l->s[r] == ' ' || l->s[r] == '\t'))
		r++;
	int k = c - width(l->s, (size_t)at);
	if (k < 0)
		k = 0;
	Buf b = {0};
	bufadd(&b, l->s, (size_t)at);
	for (int i = 0; i < k; i++)
		bufputc(&b, ' ');
	bufadd(&b, l->s + r, l->n - r);
	l->s = b.b ? b.b : "";
	l->n = b.n;
}

/* pad: the text after the blanks at offset at starts at column w+1. */
static void pad(Line *l, int at, int w) { padto(l, at, w + 1); }

typedef bool HasFn(Printer *p, Line *ls, int l, long *group);
typedef void RunFn(Printer *p, Line *ls, int *run, size_t n);

/* runs calls f for each maximal run of consecutive marked lines that
 * have a mark (per has) with equal group. */
static void runs(Printer *p, Line *ls, HasFn *has, RunFn *f) {
	VEC(int) run = {0};
	long group = 0;
	for (size_t l = 0; l < p->marks.n; l++) {
		if (!p->marks.v[l].present)
			continue;
		long g;
		if (!has(p, ls, (int)l, &g)) {
			if (run.n)
				f(p, ls, run.v, run.n);
			run.n = 0;
			continue;
		}
		if (run.n && ((int)l != run.v[run.n - 1] + 1 || g != group)) {
			f(p, ls, run.v, run.n);
			run.n = 0;
		}
		VPUSH(run, (int)l);
		group = g;
	}
	if (run.n)
		f(p, ls, run.v, run.n);
	free(run.v);
}

static bool hascase(Printer *p, Line *ls, int l, long *g) {
	(void)ls;
	*g = p->marks.v[l].casegroup;
	return p->marks.v[l].caseat >= 0;
}

static void runcase(Printer *p, Line *ls, int *run, size_t n) {
	int w = 0;
	for (size_t i = 0; i < n; i++) {
		int x = width(ls[run[i]].s, (size_t)p->marks.v[run[i]].caseat);
		if (x > w)
			w = x;
	}
	for (size_t i = 0; i < n; i++) {
		Marks *m = &p->marks.v[run[i]];
		size_t old = ls[run[i]].n;
		pad(&ls[run[i]], m->caseat, w);
		if (m->commentat > m->caseat)
			m->commentat += (int)(ls[run[i]].n - old);
	}
}

static bool hasassign(Printer *p, Line *ls, int l, long *g) {
	(void)ls;
	*g = p->marks.v[l].assignindent;
	return p->marks.v[l].assignat >= 0;
}

static void runassign(Printer *p, Line *ls, int *run, size_t n) {
	/* As for line-final operators, align only runs the author aligned. */
	bool deliberate = false;
	for (size_t i = 0; i < n; i++)
		deliberate = deliberate || p->marks.v[run[i]].assignpadded;
	if (n < 2 || !deliberate)
		return;
	/* Align the anchor of each operator: its ":", or its first
	 * character for "<-", "<->". */
	int c = 0;
	for (size_t i = 0; i < n; i++) {
		Marks *m = &p->marks.v[run[i]];
		int x = width(ls[run[i]].s, (size_t)m->assignat) + 1 + m->assignanchor;
		if (x > c)
			c = x;
	}
	for (size_t i = 0; i < n; i++) {
		Marks *m = &p->marks.v[run[i]];
		size_t old = ls[run[i]].n;
		padto(&ls[run[i]], m->assignat, c - m->assignanchor);
		int d = (int)(ls[run[i]].n - old);
		if (m->opat > m->assignat)
			m->opat += d;
		if (m->commentat > m->assignat)
			m->commentat += d;
	}
}

/* endswithop reports whether the operator recorded in m is the last code
 * on the line (it may not be, if a following "{" was joined to it). */
static bool endswithop(Line *l, Marks *m) {
	int end = (int)l->n;
	if (m->commentat >= 0)
		end = m->commentat;
	if (m->opat > end)
		return false;
	const char *s = l->s + m->opat;
	size_t n = (size_t)(end - m->opat);
	while (n && strchr(" \t\n\v\f\r", *s))
		s++, n--;
	while (n && strchr(" \t\n\v\f\r", s[n - 1]))
		n--;
	return n == m->oplen && !memcmp(s, m->optext, n);
}

static VEC(char *) opgroups; /* "indent op" keys, numbered by position + 1 */

static bool hasop(Printer *p, Line *ls, int l, long *g) {
	Marks *m = &p->marks.v[l];
	if (m->opat < 0 || !endswithop(&ls[l], m))
		return false;
	Buf key = {0};
	bufprintf(&key, "%d ", m->opindent);
	bufadd(&key, m->optext, m->oplen);
	for (size_t i = 0; i < opgroups.n; i++)
		if (!strcmp(opgroups.v[i], key.b)) {
			*g = (long)i + 1;
			return true;
		}
	VPUSH(opgroups, key.b);
	*g = (long)opgroups.n;
	return true;
}

static void runop(Printer *p, Line *ls, int *run, size_t n) {
	/* Align only runs the author aligned: at least two lines, at least
	 * one of which had padding before its operator. */
	bool deliberate = false;
	for (size_t i = 0; i < n; i++)
		deliberate = deliberate || p->marks.v[run[i]].oppadded;
	if (n < 2 || !deliberate)
		return;
	int w = 0;
	for (size_t i = 0; i < n; i++) {
		int x = width(ls[run[i]].s, (size_t)p->marks.v[run[i]].opat);
		if (x > w)
			w = x;
	}
	for (size_t i = 0; i < n; i++) {
		Marks *m = &p->marks.v[run[i]];
		size_t old = ls[run[i]].n;
		pad(&ls[run[i]], m->opat, w);
		if (m->commentat > m->opat)
			m->commentat += (int)(ls[run[i]].n - old);
	}
}

static bool hascomment(Printer *p, Line *ls, int l, long *g) {
	(void)ls;
	*g = 0;
	return p->marks.v[l].commentat >= 0;
}

static void runcomment(Printer *p, Line *ls, int *run, size_t n) {
	int w = 0;
	for (size_t i = 0; i < n; i++) {
		int x = width(ls[run[i]].s, (size_t)p->marks.v[run[i]].commentat);
		if (x > w)
			w = x;
	}
	for (size_t i = 0; i < n; i++)
		pad(&ls[run[i]], p->marks.v[run[i]].commentat, w);
}

static LineVec splitlines(const char *s, size_t n) {
	LineVec ls = {0};
	size_t start = 0;
	for (size_t i = 0; i <= n; i++)
		if (i == n || s[i] == '\n') {
			Line l = {xstrndup(s + start, i - start), i - start};
			VPUSH(ls, l);
			start = i + 1;
		}
	return ls;
}

static void align(Printer *p, Buf *out) {
	bool any = false;
	for (size_t l = 0; l < p->marks.n; l++)
		any = any || p->marks.v[l].present;
	if (!any)
		return;
	LineVec ls = splitlines(out->b, out->n);
	runs(p, ls.v, hascase, runcase);
	runs(p, ls.v, hasassign, runassign);
	opgroups.n = 0;
	runs(p, ls.v, hasop, runop);
	runs(p, ls.v, hascomment, runcomment);
	Buf b = {0};
	for (size_t i = 0; i < ls.n; i++) {
		if (i)
			bufputc(&b, '\n');
		bufadd(&b, ls.v[i].s, ls.v[i].n);
	}
	if (!b.b)
		bufputs(&b, "");
	*out = b;
}

/* ------------------------------------------------------------------ */
/* VIB (vib.go): the IPL's visual interface builder maintains a section
 * of a program between two marker comments, rewriting it whenever the
 * interface is saved; the formatter leaves such sections as they are. */

static const char *const vibmarkers[][2] = {
	{"#===<<vib:begin>>===", "#===<<vib:end>>==="},
	{"#===<<xie:begin>>===", "#===<<xie:end>>==="}, /* older files */
};

typedef VEC(int) Ints;

/* vibsections returns the line ranges [begin, end] (0-based, inclusive)
 * of the VIB sections of src, as pairs. */
static Ints vibsections(const char *src, size_t n) {
	Tokens ts = lex_tokenize(src, n);
	Lines lines = lines_new(src, n);
	Ints secs = {0};
	int begin = -1, kind = -1;
	for (size_t i = 0; i < ts.n; i++) {
		Token *t = &ts.v[i];
		bool atlinestart = i == 0;
		for (size_t k = 0; k < t->nlead; k++) {
			Trivia *tr = &t->lead[k];
			if (tr->kind == TV_NEWLINE) {
				atlinestart = true;
				continue;
			}
			if (tr->kind == TV_SPACE)
				continue;
			if (tr->kind == TV_COMMENT && atlinestart) {
				int line, c;
				lines_position(&lines, tr->offset, &line, &c);
				for (int m = 0; m < 2; m++) {
					const char *b = vibmarkers[m][0], *e = vibmarkers[m][1];
					if (begin < 0 && tr->len >= strlen(b) && !memcmp(src + tr->offset, b, strlen(b)))
						begin = line - 1, kind = m;
					else if (begin >= 0 && m == kind && tr->len >= strlen(e) &&
						 !memcmp(src + tr->offset, e, strlen(e))) {
						VPUSH(secs, begin);
						VPUSH(secs, line - 1);
						begin = -1;
					}
				}
			}
			atlinestart = false;
		}
	}
	return secs;
}

static void preservevib(const char *src, size_t n, Buf *out) {
	Ints ss = vibsections(src, n), os = vibsections(out->b, out->n);
	if (ss.n == 0 || ss.n != os.n)
		return;
	LineVec sl = splitlines(src, n), ol = splitlines(out->b, out->n);
	LineVec b = {0};
	int prev = 0;
	for (size_t i = 0; i < os.n; i += 2) {
		for (int k = prev; k < os.v[i]; k++)
			VPUSH(b, ol.v[k]);
		for (int k = ss.v[i]; k <= ss.v[i + 1]; k++)
			VPUSH(b, sl.v[k]);
		prev = os.v[i + 1] + 1;
	}
	for (size_t k = (size_t)prev; k < ol.n; k++)
		VPUSH(b, ol.v[k]);
	Buf r = {0};
	for (size_t i = 0; i < b.n; i++) {
		if (i)
			bufputc(&r, '\n');
		bufadd(&r, b.v[i].s, b.v[i].n);
	}
	if (!r.b)
		bufputs(&r, "");
	*out = r;
}

/* ------------------------------------------------------------------ */

bool format_source(const char *src, size_t n, Buf *out) {
	File *f = syn_parse(src, n);
	out->n = 0;
	if (f->errs.n) {
		Lines lines = lines_new(src, n);
		for (size_t i = 0; i < f->errs.n; i++) {
			int l, c;
			lines_position(&lines, f->errs.v[i].offset, &l, &c);
			if (i)
				bufputc(out, '\n');
			bufprintf(out, "%d:%d: %s", l, c + 1, f->errs.v[i].msg);
		}
		return false;
	}
	Printer pr = {0};
	Printer *p = &pr;
	p->f = f;
	p->toks = f->toks.v;
	p->ntoks = f->toks.n;
	p->tokline = xmalloc((p->ntoks + 1) * sizeof *p->tokline);
	for (size_t i = 0; i < p->ntoks; i++)
		p->tokline[i] = -1;
	for (size_t i = 0; i < f->decls.n; i++)
		decl(p, f->decls.v[i]);
	Place o = {0, false, false, false, false, false, -1};
	emit(p, f->eof, o);
	Buf res = p->b;
	if (!res.b)
		bufputs(&res, "");
	align(p, &res);
	preservevib(src, n, &res);
	while (res.n && strchr(" \t\n", res.b[res.n - 1]))
		res.n--;
	if (res.n)
		bufputc(&res, '\n');
	bufadd(out, res.b, res.n);
	if (!out->b)
		bufputs(out, "");
	return true;
}

/* ------------------------------------------------------------------ */
/* gcon fmt (icfmt's command) */

static bool flist, fwrite_, fdiff;
static int exitcode;

static void report(const char *msg) {
	fprintf(stderr, "%s\n", msg);
	exitcode = 2;
}

static const char *errtext(int e) {
	static char buf[256];
	snprintf(buf, sizeof buf, "%s", strerror(e));
	if (buf[0] >= 'A' && buf[0] <= 'Z' && !(buf[1] >= 'A' && buf[1] <= 'Z'))
		buf[0] = (char)(buf[0] - 'A' + 'a');
	return buf;
}

static bool readall(FILE *f, Buf *b) {
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(b, chunk, k);
	if (!b->b)
		bufputs(b, "");
	return !ferror(f);
}

/* unifieddiff runs diff -u on the two versions. */
static bool unifieddiff(const char *name, const Buf *a, const Buf *b, Buf *out) {
	const char *tmp = getenv("TMPDIR");
	Buf dir = {0};
	bufprintf(&dir, "%s/gconfmtXXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir.b))
		return false;
	Buf fa = {0}, fb = {0};
	bufprintf(&fa, "%s/orig", dir.b);
	bufprintf(&fb, "%s/new", dir.b);
	FILE *x = fopen(fa.b, "wb"), *y = fopen(fb.b, "wb");
	bool ok = x && y && fwrite(a->b, 1, a->n, x) == a->n && fwrite(b->b, 1, b->n, y) == b->n;
	if (x)
		fclose(x);
	if (y)
		fclose(y);
	if (ok) {
		int pfd[2];
		if (pipe(pfd) == 0) {
			pid_t pid = fork();
			if (pid == 0) {
				close(pfd[0]);
				dup2(pfd[1], 1);
				Buf la = {0};
				bufprintf(&la, "%s.orig", name);
				execlp("diff", "diff", "-u", "--label", la.b, "--label", name, fa.b, fb.b, (char *)NULL);
				_exit(127);
			}
			close(pfd[1]);
			FILE *r = fdopen(pfd[0], "rb");
			readall(r, out);
			fclose(r);
			int st;
			waitpid(pid, &st, 0);
		}
	}
	unlink(fa.b);
	unlink(fb.b);
	rmdir(dir.b);
	return ok;
}

static void process(const char *name, FILE *in) {
	Buf src = {0};
	if (!readall(in, &src)) {
		Buf m = {0};
		bufprintf(&m, "read %s: %s", name, errtext(errno));
		report(m.b);
		return;
	}
	Buf res = {0};
	if (!format_source(src.b, src.n, &res)) {
		Buf m = {0};
		bufprintf(&m, "%s:\n\t", name);
		for (size_t i = 0; i < res.n; i++) {
			bufputc(&m, res.b[i]);
			if (res.b[i] == '\n')
				bufputc(&m, '\t');
		}
		report(m.b);
		return;
	}
	bool changed = src.n != res.n || memcmp(src.b, res.b, src.n);
	if (flist && changed)
		printf("%s\n", name);
	if (fwrite_ && changed) {
		struct stat st;
		if (stat(name, &st) != 0) {
			Buf m = {0};
			bufprintf(&m, "stat %s: %s", name, errtext(errno));
			report(m.b);
			return;
		}
		FILE *w = fopen(name, "wb");
		if (!w || fwrite(res.b, 1, res.n, w) != res.n || fclose(w) != 0) {
			Buf m = {0};
			bufprintf(&m, "open %s: %s", name, errtext(errno));
			report(m.b);
			return;
		}
		chmod(name, st.st_mode & 0777);
	}
	if (fdiff && changed) {
		Buf d = {0};
		if (unifieddiff(name, &src, &res, &d))
			fwrite(d.b, 1, d.n, stdout);
	}
	if (!flist && !fwrite_ && !fdiff)
		fwrite(res.b, 1, res.n, stdout);
	fflush(stdout);
}

static void processfile(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		Buf m = {0};
		bufprintf(&m, "open %s: %s", path, errtext(errno));
		report(m.b);
		return;
	}
	process(path, f);
	fclose(f);
}

static int cmpname(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* walk visits path's .icn files depth first, entries in lexical order,
 * as Go's filepath.WalkDir does, without recursion. */
static void walk(const char *root) {
	VEC(char *) stk = {0};
	VPUSH(stk, xstrndup(root, strlen(root)));
	while (stk.n) {
		char *p = stk.v[--stk.n];
		struct stat st;
		if (lstat(p, &st) != 0) {
			Buf m = {0};
			bufprintf(&m, "lstat %s: %s", p, errtext(errno));
			report(m.b);
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			size_t n = strlen(p);
			if (n >= 4 && !strcmp(p + n - 4, ".icn"))
				processfile(p);
			continue;
		}
		DIR *d = opendir(p);
		if (!d) {
			Buf m = {0};
			bufprintf(&m, "open %s: %s", p, errtext(errno));
			report(m.b);
			continue;
		}
		VEC(char *) names = {0};
		struct dirent *e;
		while ((e = readdir(d)))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
				VPUSH(names, xstrndup(e->d_name, strlen(e->d_name)));
		closedir(d);
		qsort(names.v, names.n, sizeof *names.v, cmpname);
		for (size_t i = names.n; i-- > 0;) {
			Buf c = {0};
			bufputs(&c, p);
			if (p[strlen(p) - 1] != '/')
				bufputc(&c, '/');
			bufputs(&c, names.v[i]);
			VPUSH(stk, c.b);
		}
	}
}

int format_main(int argc, char **argv) {
	int i = 1;
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || !a[1])
			break;
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		const char *name = a + 1 + (a[1] == '-');
		if (!strcmp(name, "l"))
			flist = true;
		else if (!strcmp(name, "w"))
			fwrite_ = true;
		else if (!strcmp(name, "d"))
			fdiff = true;
		else if (help_isflag(a))
			help_page("fmt");
		else {
			fprintf(stderr, "flag provided but not defined: -%s\n", name);
			help_usage("fmt");
		}
	}
	if (i == argc) {
		if (fwrite_) {
			fprintf(stderr, "gcon fmt: cannot use -w with standard input\n");
			return 2;
		}
		process("<standard input>", stdin);
		return exitcode;
	}
	for (; i < argc; i++) {
		struct stat st;
		if (stat(argv[i], &st) != 0) {
			Buf m = {0};
			bufprintf(&m, "stat %s: %s", argv[i], errtext(errno));
			report(m.b);
			continue;
		}
		if (!S_ISDIR(st.st_mode))
			processfile(argv[i]);
		else
			walk(argv[i]);
	}
	return exitcode;
}
