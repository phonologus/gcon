/*
 * The parser (syntax.h): recursive descent over the token stream,
 * transliterated from the Go reference's internal/syntax/parser.go, so
 * that trees and error messages are identical.
 */
#include "syntax.h"

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
/* Declarations */

static bool isdeclstart(TokKind k) {
	return k == T_PROCEDURE || k == T_GLOBAL || k == T_RECORD || k == T_LINK || k == T_INVOCABLE;
}

static Node *expr(Parser *p);
static Node *nexpr(Parser *p);
static Node *exprrequired(Parser *p);

static void elemtok(ElemVec *v, Token *t) {
	Elem e = {t, NULL};
	VPUSH(*v, e);
}

static void elemnode(ElemVec *v, Node *n) {
	Elem e = {NULL, n};
	VPUSH(*v, e);
}

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

static Node *statement(Parser *p, TokKind sep);

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
		in->initial.x = exprrequired(p);
		in->initial.semi = expect(p, T_SEMICOL);
		d->proc.initial = in;
	}

	/* procbody: { nexpr ; } */
	while (kind(p) != T_END && kind(p) != T_EOF && !isdeclstart(kind(p))) {
		size_t start = p->pos;
		Node *x = statement(p, T_SEMICOL);
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

/* statement parses one element of a semicolon-separated sequence: a
 * possibly empty expression. If what follows is neither the separator
 * nor a token that ends the sequence, the rest of the element (up to a
 * separator, closing brace, "end" or declaration keyword, skipping
 * balanced brackets) is gathered into a bad expression. */
static Node *statement(Parser *p, TokKind sep) {
	Node *x = nexpr(p);
	TokKind k = kind(p);
	if (k == sep || k == T_END || k == T_EOF || k == T_RBRACE || isdeclstart(k))
		return x;
	if (k != T_ILLEGAL)
		errfound(p, "unexpected ");
	Node *bad = newnode(N_BADEXPR);
	if (x)
		elemnode(&bad->bad.elems, x);
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
	return bad;
}

/* ------------------------------------------------------------------ */
/* Expressions */

/* canstart reports whether k can begin an expression. */
static bool canstart(TokKind k) {
	switch (k) {
	case T_END: case T_DEFAULT: case T_INITIAL: case T_LOCAL: case T_STATIC:
		return false;
	default:
		return tok_beginner(k);
	}
}

/* nexpr parses an optional expression; it returns NULL if none is
 * present. */
static Node *nexpr(Parser *p) { return canstart(kind(p)) ? expr(p) : NULL; }

/* exprrequired parses an expression, reporting an error if there is
 * none. */
static Node *exprrequired(Parser *p) {
	if (!canstart(kind(p))) {
		errfound(p, "expected expression, found ");
		return NULL;
	}
	return expr(p);
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

static Node *binary(Parser *p, int lv);
static Node *unary(Parser *p);

static Node *expr(Parser *p) { return binary(p, 0); }

/* operand parses the right operand of a binary operator at level lv. */
static Node *operand(Parser *p, int lv) {
	if (!canstart(kind(p))) {
		errfound(p, "expected expression, found ");
		return NULL;
	}
	return binary(p, lv);
}

static Node *mkbinary(Node *x, Token *op, Node *y) {
	Node *n = newnode(N_BINARY);
	n->binary.x = x;
	n->binary.op = op;
	n->binary.y = y;
	return n;
}

static Node *binary(Parser *p, int lv) {
	if (lv == NLEVELS)
		return unary(p);
	Node *x = binary(p, lv + 1);
	if (lv == L_TO) {
		while (kind(p) == T_TO) {
			Node *n = newnode(N_TOBY);
			n->toby.x = x;
			n->toby.to = next(p);
			n->toby.y = operand(p, lv + 1);
			if ((n->toby.by = got(p, T_BY)))
				n->toby.z = operand(p, lv + 1);
			x = n;
		}
	} else if (rightassoc[lv]) {
		if (atlevel(kind(p), lv)) {
			Token *op = next(p);
			return mkbinary(x, op, operand(p, lv));
		}
	} else {
		while (atlevel(kind(p), lv)) {
			Token *op = next(p);
			x = mkbinary(x, op, operand(p, lv + 1));
		}
	}
	return x;
}

static Node *primary(Parser *p);
static Node *postfix(Parser *p, Node *x);

static Node *unary(Parser *p) {
	if (isunary(kind(p))) {
		Node *n = newnode(N_UNARY);
		n->unary.op = next(p);
		if (!canstart(kind(p))) {
			errfound(p, "expected expression, found ");
			return n;
		}
		n->unary.x = unary(p);
		return n;
	}
	return postfix(p, primary(p));
}

static NodeList exprlist(Parser *p, TokKind closer);
static NodeList exprlistfrom(Parser *p, Node *first, TokKind closer);
static NodeList compound(Parser *p);
static Node *caseexpr(Parser *p);

static Node *group(Parser *p, NodeKind k, TokKind closer) {
	Node *n = newnode(k);
	n->group.open = next(p);
	n->group.list = k == N_BLOCK ? compound(p) : exprlist(p, closer);
	n->group.close = expect(p, closer);
	return n;
}

/* primary parses the non-postfix forms of expr11. */
static Node *primary(Parser *p) {
	Node *n;
	switch (kind(p)) {
	case T_IDENT:
		n = newnode(N_IDENT);
		n->ident.name = next(p);
		return n;
	case T_INTLIT: case T_REALLIT: case T_STRINGLIT: case T_CSETLIT:
		n = newnode(N_LITERAL);
		n->literal.tok = next(p);
		return n;
	case T_AND:
		n = newnode(N_KEYWORD);
		n->keyword.amp = next(p);
		if (kind(p) == T_IDENT || kind(p) == T_FAIL)
			n->keyword.name = next(p);
		else
			errfound(p, "expected keyword name after \"&\", found ");
		return n;
	case T_LPAREN:
		return group(p, N_PAREN, T_RPAREN);
	case T_LBRACE:
		return group(p, N_BLOCK, T_RBRACE);
	case T_LBRACK:
		return group(p, N_LISTLIT, T_RBRACK);
	case T_CREATE:
		n = newnode(N_CREATE);
		n->create.create = next(p);
		n->create.x = exprrequired(p);
		return n;
	case T_NEXT:
		n = newnode(N_NEXT);
		n->next.next = next(p);
		return n;
	case T_BREAK:
		n = newnode(N_BREAK);
		n->break_.break_ = next(p);
		n->break_.x = nexpr(p);
		return n;
	case T_FAIL:
		n = newnode(N_RETURN);
		n->ret.keyword = next(p);
		return n;
	case T_RETURN:
		n = newnode(N_RETURN);
		n->ret.keyword = next(p);
		n->ret.x = nexpr(p);
		return n;
	case T_SUSPEND:
		n = newnode(N_RETURN);
		n->ret.keyword = next(p);
		n->ret.x = nexpr(p);
		if ((n->ret.do_ = got(p, T_DO)))
			n->ret.body = exprrequired(p);
		return n;
	case T_IF:
		n = newnode(N_IF);
		n->if_.if_ = next(p);
		n->if_.cond = exprrequired(p);
		n->if_.then = expect(p, T_THEN);
		n->if_.body = exprrequired(p);
		if ((n->if_.else_ = got(p, T_ELSE)))
			n->if_.alt = exprrequired(p);
		return n;
	case T_WHILE: case T_UNTIL: case T_EVERY:
		n = newnode(N_LOOP);
		n->loop.keyword = next(p);
		n->loop.cond = exprrequired(p);
		if ((n->loop.do_ = got(p, T_DO)))
			n->loop.body = exprrequired(p);
		return n;
	case T_REPEAT:
		n = newnode(N_LOOP);
		n->loop.keyword = next(p);
		n->loop.body = exprrequired(p);
		return n;
	case T_CASE:
		return caseexpr(p);
	default:
		/* Not reached for tokens satisfying canstart. */
		errfound(p, "expected expression, found ");
		return NULL;
	}
}

/* postfix parses the postfix forms of expr11 applied to x. */
static Node *postfix(Parser *p, Node *x) {
	if (!x)
		return NULL;
	for (;;) {
		Node *n;
		switch (kind(p)) {
		case T_LBRACK: {
			Token *lb = next(p);
			Node *first = nexpr(p);
			TokKind k = kind(p);
			if (k == T_COLON || k == T_PCOLON || k == T_MCOLON) {
				if (!first && errat(p)) {
					Buf b = {0};
					bufputs(&b, "expected expression before ");
					goquote(&b, p->f->src + tok(p)->offset, tok(p)->len);
					adderr(p, &b);
				}
				n = newnode(N_SECTION);
				n->section.x = x;
				n->section.lbrack = lb;
				n->section.lo = first;
				n->section.op = next(p);
				n->section.hi = exprrequired(p);
				n->section.rbrack = expect(p, T_RBRACK);
			} else {
				n = newnode(N_INDEX);
				n->index.x = x;
				n->index.lbrack = lb;
				n->index.list = exprlistfrom(p, first, T_RBRACK);
				n->index.rbrack = expect(p, T_RBRACK);
			}
			break;
		}
		case T_LPAREN:
		case T_LBRACE: {
			TokKind closer = kind(p) == T_LPAREN ? T_RPAREN : T_RBRACE;
			n = newnode(closer == T_RPAREN ? N_CALL : N_PDCO);
			n->call.fun = x;
			n->call.open = next(p);
			n->call.args = exprlist(p, closer);
			n->call.close = expect(p, closer);
			break;
		}
		case T_DOT:
			n = newnode(N_FIELD);
			n->field.x = x;
			n->field.dot = next(p);
			n->field.name = expect(p, T_IDENT);
			break;
		default:
			return x;
		}
		x = n;
	}
}

/* exprlist parses nexpr {, nexpr}, followed by the closer (not
 * consumed). */
static NodeList exprlist(Parser *p, TokKind closer) { return exprlistfrom(p, nexpr(p), closer); }

static NodeList exprlistfrom(Parser *p, Node *first, TokKind closer) {
	NodeList l = {0};
	VPUSH(l.elems, first);
	for (;;) {
		Token *c = got(p, T_COMMA);
		if (!c)
			break;
		VPUSH(l.seps, c);
		VPUSH(l.elems, nexpr(p));
	}
	if (l.elems.n == 1 && !l.elems.v[0])
		l.elems.n = 0;
	if (kind(p) != closer && errat(p)) {
		Buf b = {0};
		bufputs(&b, "expected \",\" or ");
		quotez(&b, tok_name(closer));
		bufputs(&b, ", found ");
		describe(p, &b, tok(p));
		adderr(p, &b);
	}
	return l;
}

/* compound parses nexpr {; nexpr} inside braces. */
static NodeList compound(Parser *p) {
	NodeList l = {0};
	for (;;) {
		VPUSH(l.elems, statement(p, T_SEMICOL));
		Token *c = got(p, T_SEMICOL);
		if (!c)
			break;
		VPUSH(l.seps, c);
	}
	if (l.elems.n == 1 && !l.elems.v[0])
		l.elems.n = 0;
	return l;
}

static Node *caseexpr(Parser *p) {
	Node *n = newnode(N_CASE);
	n->case_.case_ = next(p);
	n->case_.x = exprrequired(p);
	n->case_.of = expect(p, T_OF);
	n->case_.lbrace = expect(p, T_LBRACE);
	if (!n->case_.lbrace)
		return n;
	for (;;) {
		Node *cl = newnode(N_CASECLAUSE);
		if (!(cl->clause.default_ = got(p, T_DEFAULT)))
			cl->clause.label = exprrequired(p);
		cl->clause.colon = expect(p, T_COLON);
		cl->clause.body = exprrequired(p);
		VPUSH(n->case_.clauses.elems, cl);
		Token *c = got(p, T_SEMICOL);
		if (!c)
			break;
		VPUSH(n->case_.clauses.seps, c);
	}
	n->case_.rbrace = expect(p, T_RBRACE);
	return n;
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
