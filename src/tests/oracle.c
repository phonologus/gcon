/*
 * oracle: the C front end's view of an Icon source file in the forms
 * of icont's own front end, as printed by tools/icontok, for
 * src/tests/oracle.sh, which compares the two over the reference
 * corpus. icont is the authority here, not the Go (docs/design.md,
 * "Two oracles"); this is the C of the Go's oracle tests
 * (internal/lexer/oracle_test.go, internal/syntax/icontree_test.go
 * and parser_test.go).
 *
 *   oracle tokens FILE      the token stream, as "icontok FILE"
 *   oracle trees FILE       a tree per procedure, as "icontok -t FILE"
 *   oracle needspp FILE SYM...
 *                           exit 0 if FILE needs icont's preprocessor:
 *                           it has a directive, or uses a predefined
 *                           symbol SYM
 *   oracle linedirs FILE    exit 0 if FILE has #line comments (so
 *                           icont's line numbers are not the file's)
 *   oracle strip tokens|trees
 *                           a filter: positions dropped from either
 *                           side's output, for files with #line
 *   oracle mutate SEED FILE a token deleted, duplicated or swapped with
 *                           the next, chosen by SEED; the change is
 *                           described on stderr; exit 3 if FILE has
 *                           errors or too few tokens
 *
 * tokens and trees exit 1, with the first error on stderr, if the file
 * has lexical (tokens) or any (trees) errors. Tokens print "spelling
 * TAB line [TAB image]", with the image for identifiers and numbers;
 * the line of ";" and end of file is written as 0, as icont's are
 * idiosyncratic (strip tokens does the same to icontok's).
 *
 * Test tooling: the tree writer recurses, which the corpus permits.
 */
#define _POSIX_C_SOURCE 200809L
#include "../syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Buf out;
static File *pf;

static Buf readfile(const char *name) {
	FILE *f = fopen(name, "rb");
	if (!f) {
		perror(name);
		exit(2);
	}
	Buf b = {0};
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(&b, chunk, k);
	fclose(f);
	if (!b.b)
		bufputs(&b, "");
	return b;
}

static size_t linestart(size_t off) {
	while (off > 0 && pf->src[off - 1] != '\n')
		off--;
	return off;
}

static int lineof(size_t off) {
	int line = 1;
	for (size_t i = 0; i < off; i++)
		line += pf->src[i] == '\n';
	return line;
}

/* ------------------------------------------------------------------ */
/* Tokens */

static int tokens(void) {
	Tokens *ts = &pf->toks;
	if (ts->nerrs) {
		fprintf(stderr, "%s\n", ts->errs[0].msg);
		return 1;
	}
	int line = 1;
	size_t at = 0;
	for (size_t i = 0; i < ts->n; i++) {
		Token *t = &ts->v[i];
		for (; at < t->offset; at++)
			line += pf->src[at] == '\n';
		bufputs(&out, tok_name(t->kind));
		bufprintf(&out, "\t%d", t->kind == T_SEMICOL || t->kind == T_EOF ? 0 : line);
		if (t->kind == T_IDENT || t->kind == T_INTLIT || t->kind == T_REALLIT) {
			bufputc(&out, '\t');
			bufadd(&out, pf->src + t->offset, t->len);
		}
		bufputc(&out, '\n');
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Trees: icont's, as its grammar's actions build them
 * (icon/src/icont/tgrammar.c) */

static void sp(void) {
	if (out.n > 0 && out.b[out.n - 1] != '(' && out.b[out.n - 1] != '\n')
		bufputc(&out, ' ');
}

static void open(const char *name) {
	sp();
	bufprintf(&out, "(%s", name);
}

static void close_(void) { bufputc(&out, ')'); }

static void empty(void) {
	sp();
	bufputs(&out, "()");
}

static void word(const char *s) {
	sp();
	bufputs(&out, s);
}

/* pos writes a token's position as icont does: line:column, 1-based
 * columns, tabs to multiples of 8. */
static void leaf(const char *kind, Token *t) {
	sp();
	if (!t) {
		bufprintf(&out, "(%s ?)", kind);
		return;
	}
	int col = 0;
	for (size_t i = linestart(t->offset); i < t->offset; i++) {
		switch (pf->src[i]) {
		case '\t': col = (col | 7) + 1; break;
		case '\b': if (col > 0) col--; break;
		default: col++;
		}
	}
	bufprintf(&out, "(%s %d:%d)", kind, lineof(t->offset), col + 1);
}

static void op(Token *t) { word(tok_name(t->kind)); }

static void expr(Node *x);

/* elist writes an exprlist: a single element, or a left-nested Elist. */
static void elist(NodeVec *es) {
	if (es->n == 0) {
		empty();
		return;
	}
	for (size_t i = 1; i < es->n; i++)
		open("Elist");
	for (size_t i = 0; i < es->n; i++) {
		expr(es->v[i]);
		if (i > 0)
			close_();
	}
}

static bool endswith(const char *s, const char *suf) {
	size_t n = strlen(s), m = strlen(suf);
	return n >= m && !strcmp(s + n - m, suf);
}

static void binary(Node *x) {
	TokKind k = x->binary.op->kind;
	const char *s = tok_name(k);
	switch (k) {
	case T_AND: case T_AUGAND:
		open("Conj");
		break;
	case T_QMARK: case T_AUGQMARK:
		open("Scan");
		break;
	case T_BAR:
		open("Alt");
		expr(x->binary.x);
		expr(x->binary.y);
		close_();
		return;
	case T_AT: case T_AUGAT:
		open("Activat");
		op(x->binary.op);
		expr(x->binary.y);
		expr(x->binary.x);
		close_();
		return;
	case T_BACKSLASH:
		open("Limit");
		expr(x->binary.x);
		expr(x->binary.y);
		close_();
		return;
	case T_BANG:
		open("Apply");
		expr(x->binary.x);
		expr(x->binary.y);
		close_();
		return;
	default:
		open(endswith(s, ":=") && strcmp(s, ":=") ? "Augop" : "Binop");
	}
	op(x->binary.op);
	expr(x->binary.x);
	expr(x->binary.y);
	close_();
}

static void expr(Node *x) {
	if (!x) {
		empty();
		return;
	}
	switch (x->kind) {
	case N_IDENT:
		leaf("Id", x->ident.name);
		break;
	case N_LITERAL: {
		TokKind k = x->literal.tok->kind;
		leaf(k == T_INTLIT ? "Int" : k == T_REALLIT ? "Real" : k == T_STRINGLIT ? "Str" : "Cset", x->literal.tok);
		break;
	}
	case N_KEYWORD:
		sp();
		bufputs(&out, "(Key ");
		bufadd(&out, pf->src + x->keyword.name->offset, x->keyword.name->len);
		bufputc(&out, ')');
		break;
	case N_UNARY:
		switch (x->unary.op->kind) {
		case T_NOT:
			open("Not");
			break;
		case T_BAR: case T_CONCAT: case T_LCONCAT:
			open("Bar");
			break;
		case T_AT:
			open("Activat");
			op(x->unary.op);
			expr(x->unary.x);
			empty();
			close_();
			return;
		default:
			open("Unop");
			op(x->unary.op);
		}
		expr(x->unary.x);
		close_();
		break;
	case N_BINARY:
		binary(x);
		break;
	case N_TOBY:
		open(x->toby.by ? "ToBy" : "To");
		expr(x->toby.x);
		expr(x->toby.y);
		if (x->toby.by)
			expr(x->toby.z);
		close_();
		break;
	case N_PAREN:
		if (x->group.list.elems.n > 1) {
			open("Invok");
			empty();
			elist(&x->group.list.elems);
			close_();
		} else
			elist(&x->group.list.elems);
		break;
	case N_BLOCK: {
		/* compound: nexpr | nexpr ; compound  =>  right-nested Slist */
		NodeVec *es = &x->group.list.elems;
		if (es->n == 0) {
			empty();
			break;
		}
		for (size_t i = 0; i + 1 < es->n; i++) {
			open("Slist");
			expr(es->v[i]);
		}
		expr(es->v[es->n - 1]);
		for (size_t i = 0; i + 1 < es->n; i++)
			close_();
		break;
	}
	case N_LISTLIT:
		open("List");
		elist(&x->group.list.elems);
		close_();
		break;
	case N_INDEX: {
		/* buildarray: a[i,j] => Binop([, Binop([, a, i), j) */
		NodeVec *es = &x->index.list.elems;
		size_t n = es->n ? es->n : 1;
		for (size_t i = 0; i < n; i++) {
			open("Binop");
			word("[");
		}
		expr(x->index.x);
		for (size_t i = 0; i < n; i++) {
			expr(es->n ? es->v[i] : NULL);
			close_();
		}
		break;
	}
	case N_SECTION:
		open("Sect");
		op(x->section.op);
		expr(x->section.x);
		expr(x->section.lo);
		expr(x->section.hi);
		close_();
		break;
	case N_CALL:
		open("Invok");
		expr(x->call.fun);
		elist(&x->call.args.elems);
		close_();
		break;
	case N_PDCO: {
		open("Invok");
		expr(x->call.fun);
		open("List");
		/* pdcolist: left-nested Elist of Create nodes */
		NodeVec *es = &x->call.args.elems;
		if (es->n == 0)
			empty();
		else {
			for (size_t i = 1; i < es->n; i++)
				open("Elist");
			for (size_t i = 0; i < es->n; i++) {
				open("Create");
				expr(es->v[i]);
				close_();
				if (i > 0)
					close_();
			}
		}
		close_();
		close_();
		break;
	}
	case N_FIELD:
		open("Field");
		expr(x->field.x);
		leaf("Id", x->field.name);
		close_();
		break;
	case N_CREATE:
		open("Create");
		expr(x->create.x);
		close_();
		break;
	case N_IF:
		open("If");
		expr(x->if_.cond);
		expr(x->if_.body);
		expr(x->if_.alt);
		close_();
		break;
	case N_LOOP:
		open("Loop");
		op(x->loop.keyword);
		if (x->loop.keyword->kind == T_REPEAT) {
			expr(x->loop.body);
			empty();
		} else {
			expr(x->loop.cond);
			expr(x->loop.body);
		}
		close_();
		break;
	case N_RETURN:
		if (x->ret.keyword->kind == T_SUSPEND) {
			open("Loop");
			op(x->ret.keyword);
			expr(x->ret.x);
			expr(x->ret.body);
		} else {
			open("Ret");
			op(x->ret.keyword);
			expr(x->ret.x);
		}
		close_();
		break;
	case N_BREAK:
		open("Break");
		expr(x->break_.x);
		close_();
		break;
	case N_NEXT:
		open("Next");
		close_();
		break;
	case N_CASE: {
		open("Case");
		expr(x->case_.x);
		/* caselist: left-nested Clist of Ccls */
		NodeVec *cs = &x->case_.clauses.elems;
		for (size_t i = 1; i < cs->n; i++)
			open("Clist");
		for (size_t i = 0; i < cs->n; i++) {
			Node *c = cs->v[i];
			open("Ccls");
			if (c->clause.default_)
				word("default");
			else
				expr(c->clause.label);
			expr(c->clause.body);
			close_();
			if (i > 0)
				close_();
		}
		close_();
		break;
	}
	default:
		sp();
		bufprintf(&out, "(?%s)", node_name(x->kind));
	}
}

static void proc(Node *d) {
	open("Proc");
	leaf("Id", d->proc.name);
	expr(d->proc.initial ? d->proc.initial->initial.x : NULL);
	/* procbody: nexpr ; procbody  =>  Slist(x, rest), ending in Empty */
	NodeVec *es = &d->proc.body.elems;
	for (size_t i = 0; i < es->n; i++) {
		open("Slist");
		expr(es->v[i]);
	}
	empty();
	for (size_t i = 0; i < es->n; i++)
		close_();
	word("end");
	close_();
	bufputc(&out, '\n');
}

static int trees(void) {
	if (pf->errs.n) {
		fprintf(stderr, "%d: %s\n", lineof(pf->errs.v[0].offset), pf->errs.v[0].msg);
		return 1;
	}
	for (size_t i = 0; i < pf->decls.n; i++)
		if (pf->decls.v[i]->kind == N_PROC)
			proc(pf->decls.v[i]);
	return 0;
}

/* ------------------------------------------------------------------ */

static bool hastrivia(TriviaKind k, const char *prefix) {
	for (size_t i = 0; i < pf->toks.n; i++) {
		Token *t = &pf->toks.v[i];
		for (size_t j = 0; j < t->nlead; j++) {
			Trivia *tv = &t->lead[j];
			if (tv->kind == k && tv->len >= strlen(prefix) &&
			    !memcmp(pf->src + tv->offset, prefix, strlen(prefix)))
				return true;
		}
	}
	return false;
}

static int needspp(int nsyms, char **syms) {
	if (hastrivia(TV_DIRECTIVE, ""))
		return 0;
	for (size_t i = 0; i < pf->toks.n; i++) {
		Token *t = &pf->toks.v[i];
		if (t->kind != T_IDENT)
			continue;
		for (int j = 0; j < nsyms; j++)
			if (strlen(syms[j]) == t->len && !memcmp(syms[j], pf->src + t->offset, t->len))
				return 0;
	}
	return 1;
}

/* strip: for tokens, every line number becomes 0; for trees, each
 * space-separated field that starts with a digit and holds a ':' (a
 * position, "3:7)") becomes "_" and what follows its first ')'; every
 * field is followed by a space, as the Go's stripPositions has it. */
static int strip(bool trees) {
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, stdin)) > 0) {
		if (line[n - 1] == '\n')
			line[--n] = 0;
		if (!trees) {
			char *tab = strchr(line, '\t');
			if (!tab) {
				puts(line);
				continue;
			}
			char *rest = strchr(tab + 1, '\t');
			printf("%.*s\t0%s\n", (int)(tab - line), line, rest ? rest : "");
			continue;
		}
		for (char *f = strtok(line, " \t"); f; f = strtok(NULL, " \t")) {
			if (f[0] >= '0' && f[0] <= '9' && strchr(f + 1, ':')) {
				char *rp = strchr(f, ')');
				printf("_%s ", rp ? rp : "");
			} else
				printf("%s ", f);
		}
		putchar('\n');
	}
	return 0;
}

/* A small deterministic generator (splitmix64): the mutants need only
 * be reproducible, not the Go's. */
static uint64_t rngstate;
static uint64_t rnd(void) {
	uint64_t z = (rngstate += 0x9e3779b97f4a7c15u);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
	return z ^ (z >> 31);
}

static int mutate(void) {
	if (pf->errs.n || pf->toks.n < 3)
		return 3;
	VEC(size_t) idx = {0};
	for (size_t i = 0; i < pf->toks.n; i++)
		if (!pf->toks.v[i].implicit && pf->toks.v[i].len > 0)
			VPUSH(idx, i);
	if (!idx.n)
		return 3;
	size_t j = rnd() % idx.n;
	Token *t = &pf->toks.v[idx.v[j]];
	const char *s = pf->src;
	size_t end = t->offset + t->len, n = pf->n;
	int what = (int)(rnd() % 3);
	if (what == 2 && j + 1 >= idx.n)
		what = 0;
	switch (what) {
	case 0:
		bufadd(&out, s, t->offset);
		bufadd(&out, s + end, n - end);
		fprintf(stderr, "deleted \"%.*s\" at %zu\n", (int)t->len, s + t->offset, t->offset);
		break;
	case 1:
		bufadd(&out, s, end);
		bufputc(&out, ' ');
		bufadd(&out, s + t->offset, t->len);
		bufadd(&out, s + end, n - end);
		fprintf(stderr, "duplicated \"%.*s\" at %zu\n", (int)t->len, s + t->offset, t->offset);
		break;
	default: {
		Token *u = &pf->toks.v[idx.v[j + 1]];
		size_t uend = u->offset + u->len;
		bufadd(&out, s, t->offset);
		bufadd(&out, s + u->offset, u->len);
		bufadd(&out, s + end, u->offset - end);
		bufadd(&out, s + t->offset, t->len);
		bufadd(&out, s + uend, n - uend);
		fprintf(stderr, "swapped \"%.*s\" and \"%.*s\" at %zu\n", (int)t->len, s + t->offset,
			(int)u->len, s + u->offset, t->offset);
	}
	}
	return 0;
}

static _Noreturn void usage(void) {
	fprintf(stderr, "usage: oracle tokens|trees|linedirs FILE | needspp FILE SYM... | "
			"strip tokens|trees | mutate SEED FILE\n");
	exit(2);
}

static void load(const char *name) {
	Buf b = readfile(name);
	pf = syn_parse(b.b, b.n);
}

int main(int argc, char **argv) {
	int rc;
	if (argc < 3)
		usage();
	const char *mode = argv[1];
	if (!strcmp(mode, "strip"))
		return strip(!strcmp(argv[2], "trees"));
	if (!strcmp(mode, "mutate")) {
		if (argc != 4)
			usage();
		rngstate = strtoull(argv[2], NULL, 10);
		load(argv[3]);
		rc = mutate();
	} else {
		load(argv[2]);
		if (!strcmp(mode, "tokens"))
			rc = tokens();
		else if (!strcmp(mode, "trees"))
			rc = trees();
		else if (!strcmp(mode, "needspp"))
			return needspp(argc - 3, argv + 3);
		else if (!strcmp(mode, "linedirs"))
			return hastrivia(TV_COMMENT, "#line") ? 0 : 1;
		else
			usage();
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
	return rc;
}
