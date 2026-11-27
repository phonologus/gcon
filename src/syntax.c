/*
 * The parser (syntax.h): the Go reference's internal/syntax/parser.go,
 * whose trees and error messages it reproduces, but not recursive:
 * expressions are parsed by a machine with its stack on the heap, as
 * icont's parser keeps its stack, so nesting has no ceiling but memory
 * (docs/design.md, "Two oracles").
 */
#include "syntax.h"

#include <stdlib.h>
#include <string.h>

static const char *const nodenames[N_NKINDS] = {
	[N_LINK] = "LinkDecl", [N_GLOBAL] = "GlobalDecl", [N_RECORD] = "RecordDecl",
	[N_INVOCABLE] = "InvocableDecl", [N_INVITEM] = "InvocableItem",
	[N_PROC] = "ProcDecl", [N_LOCAL] = "LocalDecl", [N_INITIAL] = "InitialClause",
	[N_BADDECL] = "BadDecl", [N_IDENT] = "Ident", [N_LITERAL] = "Literal",
	[N_KEYWORD] = "Keyword", [N_UNARY] = "Unary", [N_BINARY] = "Binary",
	[N_TOBY] = "ToBy", [N_PAREN] = "Paren", [N_BLOCK] = "Block",
	[N_LISTLIT] = "ListLit", [N_INDEX] = "Index", [N_SECTION] = "Section",
	[N_CALL] = "Call", [N_PDCO] = "PDCO", [N_FIELD] = "Field",
	[N_CREATE] = "Create", [N_IF] = "If", [N_LOOP] = "Loop",
	[N_RETURN] = "Return", [N_BREAK] = "Break", [N_NEXT] = "Next",
	[N_CASE] = "Case", [N_CASECLAUSE] = "CaseClause", [N_BADEXPR] = "BadExpr",
};

const char *node_name(NodeKind k) { return (unsigned)k < N_NKINDS ? nodenames[k] : "?"; }

static Node *newnode(NodeKind k) {
	Node *n = xcalloc(1, sizeof *n);
	n->kind = k;
	return n;
}

typedef struct Parser {
	File *f;
	Token *toks;
	size_t pos;     /* index of the current token */
	size_t lasterr; /* token index of the last syntax error, to suppress cascades */
	bool anyerr;
	VEC(struct Frame) stk; /* the expression machine's stack */
} Parser;

/* tok returns the current token. T_ILLEGAL tokens (already reported by
 * the lexer) are not skipped here; list-level loops fold them into bad
 * nodes. */
static Token *tok(Parser *p) { return &p->toks[p->pos]; }
static TokKind kind(Parser *p) { return p->toks[p->pos].kind; }

static Token *next(Parser *p) {
	Token *t = &p->toks[p->pos];
	if (t->kind != T_EOF)
		p->pos++;
	return t;
}

static Token *got(Parser *p, TokKind k) { return kind(p) == k ? next(p) : NULL; }

/* goquote writes s as Go's %q does, for the ASCII text it is given
 * here (token names, operator and reserved-word spellings). */
static void goquote(Buf *b, const char *s, size_t n) {
	bufputc(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': bufputs(b, "\\\""); break;
		case '\\': bufputs(b, "\\\\"); break;
		case '\a': bufputs(b, "\\a"); break;
		case '\b': bufputs(b, "\\b"); break;
		case '\f': bufputs(b, "\\f"); break;
		case '\n': bufputs(b, "\\n"); break;
		case '\r': bufputs(b, "\\r"); break;
		case '\t': bufputs(b, "\\t"); break;
		case '\v': bufputs(b, "\\v"); break;
		default:
			if (c < 0x20 || c >= 0x7f)
				bufprintf(b, "\\x%02x", c);
			else
				bufputc(b, c);
		}
	}
	bufputc(b, '"');
}

static void quotez(Buf *b, const char *s) { goquote(b, s, strlen(s)); }

static void describe(Parser *p, Buf *b, Token *t) {
	if (t->implicit)
		bufputs(b, "newline");
	else if (t->kind == T_EOF)
		bufputs(b, "end of file");
	else if (t->kind == T_IDENT) {
		bufputs(b, "identifier ");
		bufadd(b, p->f->src + t->offset, t->len);
	} else if (tok_keyword(t->kind) || tok_operator(t->kind))
		goquote(b, p->f->src + t->offset, t->len);
	else
		bufputs(b, tok_name(t->kind));
}

/* error records msg at the current token, unless the last error was
 * there too. */
static bool errat(Parser *p) {
	if (p->anyerr && p->lasterr == p->pos)
		return false;
	p->anyerr = true;
	p->lasterr = p->pos;
	return true;
}

static void adderr(Parser *p, Buf *b) {
	SynError e = {tok(p)->offset, b->b};
	VPUSH(p->f->errs, e);
}

/* "<pre><description of the current token>" */
static void errfound(Parser *p, const char *pre) {
	if (!errat(p))
		return;
	Buf b = {0};
	bufputs(&b, pre);
	describe(p, &b, tok(p));
	adderr(p, &b);
}

/* expect consumes a token of kind k, or reports an error and returns
 * NULL. */
static Token *expect(Parser *p, TokKind k) {
	Token *t = got(p, k);
	if (t)
		return t;
	if (errat(p)) {
		Buf b = {0};
		bufputs(&b, "expected ");
		quotez(&b, tok_name(k));
		bufputs(&b, ", found ");
		describe(p, &b, tok(p));
		adderr(p, &b);
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Expressions: tables */

/* canstart reports whether k can begin an expression. */
static bool canstart(TokKind k) {
	switch (k) {
	case T_END: case T_DEFAULT: case T_INITIAL: case T_LOCAL: case T_STATIC:
		return false;
	default:
		return tok_beginner(k);
	}
}

/* The binary operator levels of grammar.h, loosest first: expr,
 * expr1a, expr1, expr2, ..., expr9. binlevel[k] is the level of
 * operator k plus one (an operator is at one level at most); the "to
 * ... by" level has no operators of its own. */
enum { L_AND, L_QMARK, L_ASSIGN, L_TO, L_BAR, L_COMPARE, L_CONCAT, L_ADD, L_MUL, L_CARET, L_LIMIT, NLEVELS };

static const bool rightassoc[NLEVELS] = {[L_ASSIGN] = true, [L_BAR] = true, [L_CARET] = true};

static const unsigned char binlevel[T_NKINDS] = {
	[T_AND] = L_AND + 1,
	[T_QMARK] = L_QMARK + 1,
	[T_ASSIGN] = L_ASSIGN + 1, [T_SWAP] = L_ASSIGN + 1, [T_REVASSIGN] = L_ASSIGN + 1,
	[T_REVSWAP] = L_ASSIGN + 1, [T_AUGCONCAT] = L_ASSIGN + 1, [T_AUGLCONCAT] = L_ASSIGN + 1,
	[T_AUGDIFF] = L_ASSIGN + 1, [T_AUGUNION] = L_ASSIGN + 1, [T_AUGPLUS] = L_ASSIGN + 1,
	[T_AUGMINUS] = L_ASSIGN + 1, [T_AUGSTAR] = L_ASSIGN + 1, [T_AUGINTER] = L_ASSIGN + 1,
	[T_AUGSLASH] = L_ASSIGN + 1, [T_AUGMOD] = L_ASSIGN + 1, [T_AUGCARET] = L_ASSIGN + 1,
	[T_AUGNMEQ] = L_ASSIGN + 1, [T_AUGEQUIV] = L_ASSIGN + 1, [T_AUGNMGE] = L_ASSIGN + 1,
	[T_AUGNMGT] = L_ASSIGN + 1, [T_AUGNMLE] = L_ASSIGN + 1, [T_AUGNMLT] = L_ASSIGN + 1,
	[T_AUGNMNE] = L_ASSIGN + 1, [T_AUGNEQUIV] = L_ASSIGN + 1, [T_AUGSEQ] = L_ASSIGN + 1,
	[T_AUGSGE] = L_ASSIGN + 1, [T_AUGSGT] = L_ASSIGN + 1, [T_AUGSLE] = L_ASSIGN + 1,
	[T_AUGSLT] = L_ASSIGN + 1, [T_AUGSNE] = L_ASSIGN + 1, [T_AUGQMARK] = L_ASSIGN + 1,
	[T_AUGAND] = L_ASSIGN + 1, [T_AUGAT] = L_ASSIGN + 1,
	[T_BAR] = L_BAR + 1,
	[T_SEQ] = L_COMPARE + 1, [T_SGE] = L_COMPARE + 1, [T_SGT] = L_COMPARE + 1,
	[T_SLE] = L_COMPARE + 1, [T_SLT] = L_COMPARE + 1, [T_SNE] = L_COMPARE + 1,
	[T_NMEQ] = L_COMPARE + 1, [T_NMGE] = L_COMPARE + 1, [T_NMGT] = L_COMPARE + 1,
	[T_NMLE] = L_COMPARE + 1, [T_NMLT] = L_COMPARE + 1, [T_NMNE] = L_COMPARE + 1,
	[T_EQUIV] = L_COMPARE + 1, [T_NEQUIV] = L_COMPARE + 1,
	[T_CONCAT] = L_CONCAT + 1, [T_LCONCAT] = L_CONCAT + 1,
	[T_PLUS] = L_ADD + 1, [T_DIFF] = L_ADD + 1, [T_UNION] = L_ADD + 1, [T_MINUS] = L_ADD + 1,
	[T_STAR] = L_MUL + 1, [T_INTER] = L_MUL + 1, [T_SLASH] = L_MUL + 1, [T_MOD] = L_MUL + 1,
	[T_CARET] = L_CARET + 1,
	[T_BACKSLASH] = L_LIMIT + 1, [T_AT] = L_LIMIT + 1, [T_BANG] = L_LIMIT + 1,
};

static bool atlevel(TokKind k, int lv) { return binlevel[k] == lv + 1; }

/* The prefix operators of expr10. */
static bool isunary(TokKind k) {
	switch (k) {
	case T_AT: case T_NOT: case T_BAR: case T_CONCAT: case T_LCONCAT: case T_DOT:
	case T_BANG: case T_DIFF: case T_PLUS: case T_STAR: case T_SLASH: case T_CARET:
	case T_INTER: case T_TILDE: case T_MINUS: case T_NMEQ: case T_NMNE: case T_SEQ:
	case T_SNE: case T_EQUIV: case T_UNION: case T_QMARK: case T_NEQUIV:
	case T_BACKSLASH:
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------ */
/* Expressions: the machine
 *
 * The expression grammar is nested, and the Go parses it by recursive
 * descent. Here, as in icont (whose yacc parser keeps its stack on the
 * heap and grows it), nesting has no ceiling but memory: each function
 * of the Go's descent is a kind of Frame on the parser's own stack, with
 * a state saying where it resumes. A frame calls another by setting its
 * resume state and pushing the callee (call), and returns by popping
 * itself and leaving its result in *r (done), where the caller's next
 * step finds it. A frame may also become another in place (a tail call:
 * nexpr becoming the binary operator frame). The Go function each kind
 * stands for is named beside it; the parse is step for step the Go's,
 * so the trees and errors are the same.
 */

enum {
	F_NEXPR,     /* nexpr: an optional expression */
	F_EXPRREQ,   /* exprRequired */
	F_OPERAND,   /* operand(lv) */
	F_BIN,       /* binary(lv), all levels from the tightest up to lv */
	F_UNARY,     /* unary, and postfix of its primary */
	F_PRIMARY,   /* primary: dispatches to the frames below */
	F_GROUP,     /* ( list ), { compound }, [ list ] */
	F_CREATE,
	F_BREAK,
	F_RETURN,    /* return and suspend */
	F_IF,
	F_LOOP,
	F_CASE,      /* caseExpr */
	F_LIST,      /* exprList, exprListFrom (with a first element) */
	F_COMPOUND,  /* compound */
	F_STATEMENT, /* statement(sep) */
};

typedef struct Frame {
	unsigned char fn, st;
	signed char lv, l; /* F_BIN: the loosest level wanted, the level being applied */
	TokKind tk;        /* a closer or separator */
	Node *n, *x;       /* the node being built; F_BIN, F_UNARY: the operand so far */
	Token *t;          /* a pending operator or bracket */
	NodeList *dst;     /* F_LIST, F_COMPOUND: the list being filled */
} Frame;

static Frame frame(int fn) {
	Frame f = {0};
	f.fn = (unsigned char)fn;
	return f;
}

static Frame operandframe(int lv) {
	Frame f = frame(F_OPERAND);
	f.lv = (signed char)lv;
	return f;
}

static Frame listframe(NodeList *dst, TokKind closer) {
	Frame f = frame(F_LIST);
	f.dst = dst;
	f.tk = closer;
	return f;
}

static Frame stmtframe(TokKind sep) {
	Frame f = frame(F_STATEMENT);
	f.tk = sep;
	return f;
}

static Frame *top(Parser *p) { return &p->stk.v[p->stk.n - 1]; }

/* call resumes the current frame at state st once c has returned. The
 * push may move the stack: the caller must not touch its frame after. */
static void call(Parser *p, int st, Frame c) {
	top(p)->st = (unsigned char)st;
	VPUSH(p->stk, c);
}

static void done(Parser *p, Node **r, Node *v) {
	p->stk.n--;
	*r = v;
}

static Node *mkbinary(Node *x, Token *op, Node *y) {
	Node *n = newnode(N_BINARY);
	n->binary.x = x;
	n->binary.op = op;
	n->binary.y = y;
	return n;
}

static void finishlist(NodeList *l) {
	if (l->elems.n == 1 && !l->elems.v[0])
		l->elems.n = 0;
}

static bool isdeclstart(TokKind k) {
	return k == T_PROCEDURE || k == T_GLOBAL || k == T_RECORD || k == T_LINK || k == T_INVOCABLE;
}

static void elemtok(ElemVec *v, Token *t) {
	Elem e = {t, NULL};
	VPUSH(*v, e);
}

static void elemnode(ElemVec *v, Node *n) {
	Elem e = {NULL, n};
	VPUSH(*v, e);
}

/* step runs the top frame until it calls, returns or becomes another;
 * *r is the result of the frame that last returned. */
static void step(Parser *p, Node **r) {
	Frame *f = top(p);
	Node *v = *r, *n;
	switch (f->fn) {
	case F_NEXPR:
		if (!canstart(kind(p))) {
			done(p, r, NULL);
			return;
		}
		f->fn = F_BIN, f->lv = 0, f->st = 0;
		return;

	case F_EXPRREQ:
	case F_OPERAND:
		if (!canstart(kind(p))) {
			errfound(p, "expected expression, found ");
			done(p, r, NULL);
			return;
		}
		if (f->fn == F_EXPRREQ)
			f->lv = 0;
		f->fn = F_BIN, f->st = 0;
		return;

	case F_BIN:
		/* binary(lv) is apply(lv, apply(lv+1, ... apply(NLEVELS-1,
		 * unary()))), where apply(l, x) is the operator loop of level
		 * l on x: the climb applies each in turn. */
		switch (f->st) {
		case 0:
			call(p, 1, frame(F_UNARY));
			return;
		case 1:
			f->x = v;
			f->l = NLEVELS - 1;
			break;
		case 2:
			f->n->toby.y = v;
			if ((f->n->toby.by = got(p, T_BY))) {
				call(p, 3, operandframe(f->l + 1));
				return;
			}
			f->x = f->n;
			break;
		case 3:
			f->n->toby.z = v;
			f->x = f->n;
			break;
		case 4: /* right-associative: one operator, then the next level out */
			f->x = mkbinary(f->x, f->t, v);
			f->l--;
			break;
		case 5: /* left-associative: loop at the same level */
			f->x = mkbinary(f->x, f->t, v);
			break;
		}
		while (f->l >= f->lv) {
			int l = f->l;
			if (l == L_TO) {
				if (kind(p) == T_TO) {
					n = newnode(N_TOBY);
					n->toby.x = f->x;
					n->toby.to = next(p);
					f->n = n;
					call(p, 2, operandframe(l + 1));
					return;
				}
			} else if (atlevel(kind(p), l)) {
				f->t = next(p);
				if (rightassoc[l])
					call(p, 4, operandframe(l));
				else
					call(p, 5, operandframe(l + 1));
				return;
			}
			f->l--;
		}
		done(p, r, f->x);
		return;

	case F_UNARY:
		switch (f->st) {
		case 0:
			if (isunary(kind(p))) {
				n = newnode(N_UNARY);
				n->unary.op = next(p);
				if (!canstart(kind(p))) {
					errfound(p, "expected expression, found ");
					done(p, r, n);
					return;
				}
				f->n = n;
				call(p, 1, frame(F_UNARY));
				return;
			}
			call(p, 2, frame(F_PRIMARY));
			return;
		case 1:
			f->n->unary.x = v;
			done(p, r, f->n);
			return;
		case 2:
			if (!v) {
				done(p, r, NULL);
				return;
			}
			f->x = v;
			break;
		case 3: { /* after x[ first */
			TokKind k = kind(p);
			if (k == T_COLON || k == T_PCOLON || k == T_MCOLON) {
				if (!v && errat(p)) {
					Buf b = {0};
					bufputs(&b, "expected expression before ");
					goquote(&b, p->f->src + tok(p)->offset, tok(p)->len);
					adderr(p, &b);
				}
				n = newnode(N_SECTION);
				n->section.x = f->x;
				n->section.lbrack = f->t;
				n->section.lo = v;
				n->section.op = next(p);
				f->n = n;
				call(p, 4, frame(F_EXPRREQ));
			} else {
				n = newnode(N_INDEX);
				n->index.x = f->x;
				n->index.lbrack = f->t;
				f->n = n;
				Frame c = listframe(&n->index.list, T_RBRACK);
				c.st = 2, c.x = v;
				call(p, 5, c);
			}
			return;
		}
		case 4:
			f->n->section.hi = v;
			f->n->section.rbrack = expect(p, T_RBRACK);
			f->x = f->n;
			break;
		case 5:
			f->n->index.rbrack = expect(p, T_RBRACK);
			f->x = f->n;
			break;
		case 6:
			f->n->call.close = expect(p, f->n->kind == N_CALL ? T_RPAREN : T_RBRACE);
			f->x = f->n;
			break;
		}
		/* postfix */
		for (;;) {
			switch (kind(p)) {
			case T_LBRACK:
				f->t = next(p);
				call(p, 3, frame(F_NEXPR));
				return;
			case T_LPAREN:
			case T_LBRACE: {
				TokKind closer = kind(p) == T_LPAREN ? T_RPAREN : T_RBRACE;
				n = newnode(closer == T_RPAREN ? N_CALL : N_PDCO);
				n->call.fun = f->x;
				n->call.open = next(p);
				f->n = n;
				call(p, 6, listframe(&n->call.args, closer));
				return;
			}
			case T_DOT:
				n = newnode(N_FIELD);
				n->field.x = f->x;
				n->field.dot = next(p);
				n->field.name = expect(p, T_IDENT);
				f->x = n;
				break;
			default:
				done(p, r, f->x);
				return;
			}
		}

	case F_PRIMARY:
		switch (kind(p)) {
		case T_IDENT:
			n = newnode(N_IDENT);
			n->ident.name = next(p);
			done(p, r, n);
			return;
		case T_INTLIT: case T_REALLIT: case T_STRINGLIT: case T_CSETLIT: case T_USETLIT:
			n = newnode(N_LITERAL);
			n->literal.tok = next(p);
			done(p, r, n);
			return;
		case T_AND:
			n = newnode(N_KEYWORD);
			n->keyword.amp = next(p);
			if (kind(p) == T_IDENT || kind(p) == T_FAIL)
				n->keyword.name = next(p);
			else
				errfound(p, "expected keyword name after \"&\", found ");
			done(p, r, n);
			return;
		case T_LPAREN:
		case T_LBRACE:
		case T_LBRACK: {
			TokKind k = kind(p);
			n = newnode(k == T_LPAREN ? N_PAREN : k == T_LBRACE ? N_BLOCK : N_LISTLIT);
			n->group.open = next(p);
			f->fn = F_GROUP, f->n = n;
			if (k == T_LBRACE) {
				Frame c = frame(F_COMPOUND);
				c.dst = &n->group.list;
				call(p, 1, c);
			} else
				call(p, 1, listframe(&n->group.list, k == T_LPAREN ? T_RPAREN : T_RBRACK));
			return;
		}
		case T_CREATE:
			n = newnode(N_CREATE);
			n->create.create = next(p);
			f->fn = F_CREATE, f->n = n;
			call(p, 1, frame(F_EXPRREQ));
			return;
		case T_NEXT:
			n = newnode(N_NEXT);
			n->next.next = next(p);
			done(p, r, n);
			return;
		case T_BREAK:
			n = newnode(N_BREAK);
			n->break_.break_ = next(p);
			f->fn = F_BREAK, f->n = n;
			call(p, 1, frame(F_NEXPR));
			return;
		case T_FAIL:
			n = newnode(N_RETURN);
			n->ret.keyword = next(p);
			done(p, r, n);
			return;
		case T_RETURN:
		case T_SUSPEND:
			n = newnode(N_RETURN);
			n->ret.keyword = next(p);
			f->fn = F_RETURN, f->n = n;
			call(p, 1, frame(F_NEXPR));
			return;
		case T_IF:
			n = newnode(N_IF);
			n->if_.if_ = next(p);
			f->fn = F_IF, f->n = n;
			call(p, 1, frame(F_EXPRREQ));
			return;
		case T_WHILE: case T_UNTIL: case T_EVERY: case T_REPEAT: {
			bool repeat = kind(p) == T_REPEAT;
			n = newnode(N_LOOP);
			n->loop.keyword = next(p);
			f->fn = F_LOOP, f->n = n;
			call(p, repeat ? 2 : 1, frame(F_EXPRREQ));
			return;
		}
		case T_CASE:
			n = newnode(N_CASE);
			n->case_.case_ = next(p);
			f->fn = F_CASE, f->n = n;
			call(p, 1, frame(F_EXPRREQ));
			return;
		default:
			/* Not reached for tokens satisfying canstart. */
			errfound(p, "expected expression, found ");
			done(p, r, NULL);
			return;
		}

	case F_GROUP:
		n = f->n;
		n->group.close = expect(p, n->kind == N_PAREN ? T_RPAREN : n->kind == N_BLOCK ? T_RBRACE : T_RBRACK);
		done(p, r, n);
		return;

	case F_CREATE:
		f->n->create.x = v;
		done(p, r, f->n);
		return;

	case F_BREAK:
		f->n->break_.x = v;
		done(p, r, f->n);
		return;

	case F_RETURN:
		n = f->n;
		if (f->st == 1) {
			n->ret.x = v;
			if (n->ret.keyword->kind == T_SUSPEND && (n->ret.do_ = got(p, T_DO))) {
				call(p, 2, frame(F_EXPRREQ));
				return;
			}
		} else
			n->ret.body = v;
		done(p, r, n);
		return;

	case F_IF:
		n = f->n;
		switch (f->st) {
		case 1:
			n->if_.cond = v;
			n->if_.then = expect(p, T_THEN);
			call(p, 2, frame(F_EXPRREQ));
			return;
		case 2:
			n->if_.body = v;
			if ((n->if_.else_ = got(p, T_ELSE))) {
				call(p, 3, frame(F_EXPRREQ));
				return;
			}
			break;
		case 3:
			n->if_.alt = v;
			break;
		}
		done(p, r, n);
		return;

	case F_LOOP:
		n = f->n;
		if (f->st == 1) {
			n->loop.cond = v;
			if ((n->loop.do_ = got(p, T_DO))) {
				call(p, 2, frame(F_EXPRREQ));
				return;
			}
		} else
			n->loop.body = v;
		done(p, r, n);
		return;

	case F_CASE:
		n = f->n;
		switch (f->st) {
		case 1:
			n->case_.x = v;
			n->case_.of = expect(p, T_OF);
			n->case_.lbrace = expect(p, T_LBRACE);
			if (!n->case_.lbrace) {
				done(p, r, n);
				return;
			}
			break;
		case 2: /* after a label */
			f->x->clause.label = v;
			goto colon;
		case 3: /* after a clause's body */
			f->x->clause.body = v;
			VPUSH(n->case_.clauses.elems, f->x);
			Token *c = got(p, T_SEMICOL);
			if (!c) {
				n->case_.rbrace = expect(p, T_RBRACE);
				done(p, r, n);
				return;
			}
			VPUSH(n->case_.clauses.seps, c);
			break;
		}
		/* a clause */
		f->x = newnode(N_CASECLAUSE);
		if (!(f->x->clause.default_ = got(p, T_DEFAULT))) {
			call(p, 2, frame(F_EXPRREQ));
			return;
		}
	colon:
		f->x->clause.colon = expect(p, T_COLON);
		call(p, 3, frame(F_EXPRREQ));
		return;

	case F_LIST:
		/* states: 0 entry (exprList), 1 after an element, 2 entry with
		 * the first element in x (exprListFrom) */
		if (f->st == 0) {
			call(p, 1, frame(F_NEXPR));
			return;
		}
		if (f->st == 2)
			v = f->x;
		VPUSH(f->dst->elems, v);
		Token *comma = got(p, T_COMMA);
		if (comma) {
			VPUSH(f->dst->seps, comma);
			call(p, 1, frame(F_NEXPR));
			return;
		}
		finishlist(f->dst);
		if (kind(p) != f->tk && errat(p)) {
			Buf b = {0};
			bufputs(&b, "expected \",\" or ");
			quotez(&b, tok_name(f->tk));
			bufputs(&b, ", found ");
			describe(p, &b, tok(p));
			adderr(p, &b);
		}
		done(p, r, NULL);
		return;

	case F_COMPOUND:
		if (f->st == 1) {
			VPUSH(f->dst->elems, v);
			Token *semi = got(p, T_SEMICOL);
			if (!semi) {
				finishlist(f->dst);
				done(p, r, NULL);
				return;
			}
			VPUSH(f->dst->seps, semi);
		}
		call(p, 1, stmtframe(T_SEMICOL));
		return;

	case F_STATEMENT: {
		/* statement parses one element of a semicolon-separated
		 * sequence: a possibly empty expression. If what follows is
		 * neither the separator nor a token that ends the sequence,
		 * the rest of the element (up to a separator, closing brace,
		 * "end" or declaration keyword, skipping balanced brackets) is
		 * gathered into a bad expression. */
		if (f->st == 0) {
			call(p, 1, frame(F_NEXPR));
			return;
		}
		TokKind sep = f->tk, k = kind(p);
		if (k == sep || k == T_END || k == T_EOF || k == T_RBRACE || isdeclstart(k)) {
			done(p, r, v);
			return;
		}
		if (k != T_ILLEGAL)
			errfound(p, "unexpected ");
		Node *bad = newnode(N_BADEXPR);
		if (v)
			elemnode(&bad->bad.elems, v);
		int depth = 0;
		for (;;) {
			k = kind(p);
			if (k == T_EOF || isdeclstart(k) || k == T_END)
				break;
			if (depth == 0 && (k == sep || k == T_RBRACE))
				break;
			switch (k) {
			case T_LPAREN: case T_LBRACK: case T_LBRACE:
				depth++;
				break;
			case T_RPAREN: case T_RBRACK: case T_RBRACE:
				if (depth > 0)
					depth--;
				break;
			default:
				break;
			}
			elemtok(&bad->bad.elems, next(p));
		}
		done(p, r, bad);
		return;
	}
	}
}

/* run runs the machine from frame start to its result. */
static Node *run(Parser *p, Frame start) {
	Node *r = NULL;
	p->stk.n = 0;
	VPUSH(p->stk, start);
	while (p->stk.n)
		step(p, &r);
	return r;
}

/* ------------------------------------------------------------------ */
/* Declarations */

/* identlist parses IDENT {, IDENT}. */
static TokList identlist(Parser *p) {
	TokList l = {0};
	VPUSH(l.elems, expect(p, T_IDENT));
	for (;;) {
		Token *c = got(p, T_COMMA);
		if (!c)
			return l;
		VPUSH(l.seps, c);
		VPUSH(l.elems, expect(p, T_IDENT));
	}
}

static Node *link(Parser *p) {
	Node *d = newnode(N_LINK);
	d->link.link = next(p);
	for (;;) {
		if (kind(p) == T_IDENT || kind(p) == T_STRINGLIT)
			VPUSH(d->link.files.elems, next(p));
		else {
			errfound(p, "expected file name, found ");
			VPUSH(d->link.files.elems, (Token *)NULL);
		}
		Token *c = got(p, T_COMMA);
		if (!c)
			return d;
		VPUSH(d->link.files.seps, c);
	}
}

static Node *global(Parser *p) {
	Node *d = newnode(N_GLOBAL);
	d->global.global = next(p);
	d->global.names = identlist(p);
	return d;
}

static Node *record(Parser *p) {
	Node *d = newnode(N_RECORD);
	d->record.record = next(p);
	d->record.name = expect(p, T_IDENT);
	d->record.lparen = expect(p, T_LPAREN);
	if (kind(p) == T_IDENT)
		d->record.fields = identlist(p);
	d->record.rparen = expect(p, T_RPAREN);
	return d;
}

static Node *invocable(Parser *p) {
	Node *d = newnode(N_INVOCABLE);
	d->invocable.invocable = next(p);
	for (;;) {
		Node *it = newnode(N_INVITEM);
		switch (kind(p)) {
		case T_IDENT:
			it->invitem.name = next(p);
			break;
		case T_STRINGLIT:
			it->invitem.name = next(p);
			if ((it->invitem.colon = got(p, T_COLON)))
				it->invitem.arity = expect(p, T_INTLIT);
			break;
		default:
			errfound(p, "expected name or string, found ");
		}
		VPUSH(d->invocable.items.elems, it);
		Token *c = got(p, T_COMMA);
		if (!c)
			return d;
		VPUSH(d->invocable.items.seps, c);
	}
}

static Node *proc(Parser *p) {
	Node *d = newnode(N_PROC);
	d->proc.procedure = next(p);
	d->proc.name = expect(p, T_IDENT);
	d->proc.lparen = expect(p, T_LPAREN);
	if (kind(p) == T_IDENT) {
		d->proc.params = identlist(p);
		if ((d->proc.lbrack = got(p, T_LBRACK)))
			d->proc.rbrack = expect(p, T_RBRACK);
	}
	d->proc.rparen = expect(p, T_RPAREN);
	d->proc.semi = expect(p, T_SEMICOL);

	while (kind(p) == T_LOCAL || kind(p) == T_STATIC) {
		Node *l = newnode(N_LOCAL);
		l->local.keyword = next(p);
		l->local.names = identlist(p);
		l->local.semi = expect(p, T_SEMICOL);
		VPUSH(d->proc.locals, l);
	}
	if (kind(p) == T_INITIAL) {
		Node *in = newnode(N_INITIAL);
		in->initial.initial = next(p);
		in->initial.x = run(p, frame(F_EXPRREQ));
		in->initial.semi = expect(p, T_SEMICOL);
		d->proc.initial = in;
	}

	/* procbody: { nexpr ; } */
	while (kind(p) != T_END && kind(p) != T_EOF && !isdeclstart(kind(p))) {
		size_t start = p->pos;
		Node *x = run(p, stmtframe(T_SEMICOL));
		Token *semi = got(p, T_SEMICOL);
		if (p->pos == start) {
			/* No progress (e.g. a stray "}"): consume one token. */
			errfound(p, "unexpected ");
			x = newnode(N_BADEXPR);
			elemtok(&x->bad.elems, next(p));
		} else if (!semi && kind(p) != T_EOF && !isdeclstart(kind(p))) {
			/* (At EOF or a declaration, "missing end" is reported below.) */
			errfound(p, "expected \";\" or newline, found ");
		}
		VPUSH(d->proc.body.elems, x);
		VPUSH(d->proc.body.seps, semi);
	}
	if (!(d->proc.end = got(p, T_END)) && errat(p)) {
		Buf b = {0};
		bufputs(&b, "missing \"end\" of procedure");
		adderr(p, &b);
	}
	return d;
}

static void decls(Parser *p) {
	NodeVec *ds = &p->f->decls;
	while (kind(p) != T_EOF) {
		switch (kind(p)) {
		case T_LINK: VPUSH(*ds, link(p)); break;
		case T_GLOBAL: VPUSH(*ds, global(p)); break;
		case T_RECORD: VPUSH(*ds, record(p)); break;
		case T_INVOCABLE: VPUSH(*ds, invocable(p)); break;
		case T_PROCEDURE: VPUSH(*ds, proc(p)); break;
		default: {
			if (kind(p) != T_ILLEGAL)
				errfound(p, "expected declaration, found ");
			Node *bad = newnode(N_BADDECL);
			while (kind(p) != T_EOF && !isdeclstart(kind(p)))
				elemtok(&bad->bad.elems, next(p));
			VPUSH(*ds, bad);
		}
		}
	}
}

/* ------------------------------------------------------------------ */

File *syn_parse(const char *src, size_t n) {
	File *f = xcalloc(1, sizeof *f);
	f->src = src;
	f->n = n;
	f->toks = lex_tokenize(src, n);
	for (size_t i = 0; i < f->toks.nerrs; i++) {
		SynError e = {f->toks.errs[i].offset, f->toks.errs[i].msg};
		VPUSH(f->errs, e);
	}
	Parser p = {.f = f, .toks = f->toks.v};
	decls(&p);
	f->eof = next(&p);
	free(p.stk.v);
	return f;
}

/* ------------------------------------------------------------------ */
/* Children, in source order, absent ones skipped */

typedef struct Visit {
	SynVisit *f;
	void *ctx;
} Visit;

static void vt(Visit *v, Token *t) {
	if (t)
		v->f(v->ctx, t, NULL);
}

static void vn(Visit *v, Node *n) {
	if (n)
		v->f(v->ctx, NULL, n);
}

static void vtoks(Visit *v, TokList *l) {
	for (size_t i = 0; i < l->elems.n; i++) {
		vt(v, l->elems.v[i]);
		if (i < l->seps.n)
			vt(v, l->seps.v[i]);
	}
}

static void vnodes(Visit *v, NodeList *l) {
	for (size_t i = 0; i < l->elems.n; i++) {
		vn(v, l->elems.v[i]);
		if (i < l->seps.n)
			vt(v, l->seps.v[i]);
	}
}

void syn_children(Node *n, SynVisit *f, void *ctx) {
	Visit v = {f, ctx};
	switch (n->kind) {
	case N_LINK:
		vt(&v, n->link.link);
		vtoks(&v, &n->link.files);
		break;
	case N_GLOBAL:
		vt(&v, n->global.global);
		vtoks(&v, &n->global.names);
		break;
	case N_RECORD:
		vt(&v, n->record.record);
		vt(&v, n->record.name);
		vt(&v, n->record.lparen);
		vtoks(&v, &n->record.fields);
		vt(&v, n->record.rparen);
		break;
	case N_INVOCABLE:
		vt(&v, n->invocable.invocable);
		vnodes(&v, &n->invocable.items);
		break;
	case N_INVITEM:
		vt(&v, n->invitem.name);
		vt(&v, n->invitem.colon);
		vt(&v, n->invitem.arity);
		break;
	case N_PROC:
		vt(&v, n->proc.procedure);
		vt(&v, n->proc.name);
		vt(&v, n->proc.lparen);
		vtoks(&v, &n->proc.params);
		vt(&v, n->proc.lbrack);
		vt(&v, n->proc.rbrack);
		vt(&v, n->proc.rparen);
		vt(&v, n->proc.semi);
		for (size_t i = 0; i < n->proc.locals.n; i++)
			vn(&v, n->proc.locals.v[i]);
		vn(&v, n->proc.initial);
		vnodes(&v, &n->proc.body);
		vt(&v, n->proc.end);
		break;
	case N_LOCAL:
		vt(&v, n->local.keyword);
		vtoks(&v, &n->local.names);
		vt(&v, n->local.semi);
		break;
	case N_INITIAL:
		vt(&v, n->initial.initial);
		vn(&v, n->initial.x);
		vt(&v, n->initial.semi);
		break;
	case N_BADDECL:
	case N_BADEXPR:
		for (size_t i = 0; i < n->bad.elems.n; i++) {
			vt(&v, n->bad.elems.v[i].tok);
			vn(&v, n->bad.elems.v[i].node);
		}
		break;
	case N_IDENT:
		vt(&v, n->ident.name);
		break;
	case N_LITERAL:
		vt(&v, n->literal.tok);
		break;
	case N_KEYWORD:
		vt(&v, n->keyword.amp);
		vt(&v, n->keyword.name);
		break;
	case N_UNARY:
		vt(&v, n->unary.op);
		vn(&v, n->unary.x);
		break;
	case N_BINARY:
		vn(&v, n->binary.x);
		vt(&v, n->binary.op);
		vn(&v, n->binary.y);
		break;
	case N_TOBY:
		vn(&v, n->toby.x);
		vt(&v, n->toby.to);
		vn(&v, n->toby.y);
		vt(&v, n->toby.by);
		vn(&v, n->toby.z);
		break;
	case N_PAREN:
	case N_BLOCK:
	case N_LISTLIT:
		vt(&v, n->group.open);
		vnodes(&v, &n->group.list);
		vt(&v, n->group.close);
		break;
	case N_INDEX:
		vn(&v, n->index.x);
		vt(&v, n->index.lbrack);
		vnodes(&v, &n->index.list);
		vt(&v, n->index.rbrack);
		break;
	case N_SECTION:
		vn(&v, n->section.x);
		vt(&v, n->section.lbrack);
		vn(&v, n->section.lo);
		vt(&v, n->section.op);
		vn(&v, n->section.hi);
		vt(&v, n->section.rbrack);
		break;
	case N_CALL:
	case N_PDCO:
		vn(&v, n->call.fun);
		vt(&v, n->call.open);
		vnodes(&v, &n->call.args);
		vt(&v, n->call.close);
		break;
	case N_FIELD:
		vn(&v, n->field.x);
		vt(&v, n->field.dot);
		vt(&v, n->field.name);
		break;
	case N_CREATE:
		vt(&v, n->create.create);
		vn(&v, n->create.x);
		break;
	case N_IF:
		vt(&v, n->if_.if_);
		vn(&v, n->if_.cond);
		vt(&v, n->if_.then);
		vn(&v, n->if_.body);
		vt(&v, n->if_.else_);
		vn(&v, n->if_.alt);
		break;
	case N_LOOP:
		vt(&v, n->loop.keyword);
		vn(&v, n->loop.cond);
		vt(&v, n->loop.do_);
		vn(&v, n->loop.body);
		break;
	case N_RETURN:
		vt(&v, n->ret.keyword);
		vn(&v, n->ret.x);
		vt(&v, n->ret.do_);
		vn(&v, n->ret.body);
		break;
	case N_BREAK:
		vt(&v, n->break_.break_);
		vn(&v, n->break_.x);
		break;
	case N_NEXT:
		vt(&v, n->next.next);
		break;
	case N_CASE:
		vt(&v, n->case_.case_);
		vn(&v, n->case_.x);
		vt(&v, n->case_.of);
		vt(&v, n->case_.lbrace);
		vnodes(&v, &n->case_.clauses);
		vt(&v, n->case_.rbrace);
		break;
	case N_CASECLAUSE:
		vt(&v, n->clause.default_);
		vn(&v, n->clause.label);
		vt(&v, n->clause.colon);
		vn(&v, n->clause.body);
		break;
	case N_NKINDS:
		break;
	}
}

void syn_file_children(File *file, SynVisit *f, void *ctx) {
	for (size_t i = 0; i < file->decls.n; i++)
		f(ctx, NULL, file->decls.v[i]);
	if (file->eof)
		f(ctx, file->eof, NULL);
}

static void collect(void *ctx, Token *t, Node *n) {
	Elem e = {t, n};
	VPUSH(*(ElemVec *)ctx, e);
}

/* syn_tokens visits every token of the file in source order, without
 * recursion: an explicit stack of pending children. */
void syn_tokens(File *file, void (*f)(void *ctx, Token *t), void *ctx) {
	ElemVec stk = {0}, kids = {0};
	for (size_t i = file->decls.n; i-- > 0;)
		elemnode(&stk, file->decls.v[i]);
	while (stk.n) {
		Elem e = stk.v[--stk.n];
		if (e.tok) {
			f(ctx, e.tok);
			continue;
		}
		kids.n = 0;
		syn_children(e.node, collect, &kids);
		for (size_t i = kids.n; i-- > 0;)
			VPUSH(stk, kids.v[i]);
	}
	if (file->eof)
		f(ctx, file->eof);
	free(stk.v);
	free(kids.v);
}
