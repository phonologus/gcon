/*
 * dump: the C front end's view of an Icon source file in the canonical
 * text form of the Go reference's tools/refdump, for the differential
 * tests (src/tests/diff.sh): the two must be identical.
 *
 *   dump lex FILE    tokens, trivia, decoded literals, lexical errors
 *   dump parse FILE  the syntax tree, field by field, with tokens as
 *                    indexes; the token walk; the errors
 *   dump walk FILE   the same without the tree (nothing recursive: for
 *                    deeply nested input)
 *   dump sema FILE...
 *                    name resolution of the files as one program,
 *                    preprocessed and raw (tools/refdump/sema.go)
 *   dump build FILE...
 *                    the files compiled as one program: the encoding
 *                    (build information test=1), or the errors
 *                    (tools/refdump/build.go)
 *   dump pp FILE     the preprocessed text, line by line with the map,
 *                    and the errors ($PPDEFS: the predefined symbols,
 *                    each "1"; $PPPATH: the include path; both
 *                    separated by spaces)
 */
#include "../lexer.h"
#include "../lit.h"
#include "../syntax.h"
#include "../ipp.h"
#include "../sema.h"
#include "../compile.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Buf out;

/* esc: bytes outside printable ASCII, and the backslash, as \xHH */
static void esc(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20 || c >= 0x7f || c == '\\')
			bufprintf(&out, "\\x%02x", c);
		else
			bufputc(&out, c);
	}
}

static void escz(const char *s) { esc(s, strlen(s)); }

static void lex(const char *src, size_t n) {
	Tokens ts = lex_tokenize(src, n);
	for (size_t k = 0; k < ts.n; k++) {
		Token *t = &ts.v[k];
		bufputs(&out, "T ");
		escz(tok_name(t->kind));
		bufprintf(&out, " %zu %zu", t->offset, t->len);
		if (t->implicit)
			bufputs(&out, " implicit");
		bufputc(&out, '\n');
		for (size_t i = 0; i < t->nlead; i++) {
			Trivia *tv = &t->lead[i];
			bufprintf(&out, "  t %s %zu ", trivia_name(tv->kind), tv->offset);
			esc(src + tv->offset, tv->len);
			bufputc(&out, '\n');
		}
		bufputs(&out, "  text ");
		esc(src + t->offset, t->len);
		bufputc(&out, '\n');
		const char *text = src + t->offset;
		if (t->kind == T_STRINGLIT || t->kind == T_CSETLIT) {
			char *s;
			LitError *err;
			size_t sn;
			LitUnit u;
			if ((err = lit_string(text, t->len, &s, &sn, &u))) {
				bufputs(&out, "  s err ");
				esc(err->msg, err->len);
			} else {
				bufprintf(&out, "  s %s ", lit_unit_name(u));
				esc(s, sn);
			}
			bufputc(&out, '\n');
			if (t->kind == T_CSETLIT) {
				LitCset c;
				if ((err = lit_cset(text, t->len, &c))) {
					bufputs(&out, "  c err ");
					esc(err->msg, err->len);
					bufputc(&out, '\n');
				} else {
					bufprintf(&out, "  c %s %zu\n", lit_unit_name(c.unit), c.nsegs);
					for (size_t i = 0; i < c.nsegs; i++) {
						bufputs(&out, "   seg ");
						esc(c.segs[i], c.seglens[i]);
						bufputc(&out, '\n');
					}
					for (size_t i = 0; i < c.nranges; i++)
						bufprintf(&out, "   range %d %d %s\n", (int)c.ranges[i].lo,
							(int)c.ranges[i].hi, c.ranges[i].lonebytes ? "true" : "false");
				}
			}
		} else if (t->kind == T_INTLIT || t->kind == T_REALLIT) {
			LitNumber num;
			LitError *err = lit_number(text, t->len, &num);
			if (err) {
				bufputs(&out, "  n err ");
				esc(err->msg, err->len);
				bufputc(&out, '\n');
			} else if (num.isreal) {
				uint64_t bits;
				memcpy(&bits, &num.real, 8);
				bufprintf(&out, "  n real %016" PRIx64 "\n", bits);
			} else if (num.isbig) {
				uint8_t *bytes;
				size_t nb = bignat_bytes(&num.big, &bytes);
				bufprintf(&out, "  n big %s ", bignat_decimal(&num.big));
				for (size_t i = 0; i < nb; i++)
					bufprintf(&out, "%02x", bytes[i]);
				bufputc(&out, '\n');
			} else
				bufprintf(&out, "  n int %" PRId64 "\n", num.i);
		}
	}
	for (size_t k = 0; k < ts.nerrs; k++) {
		bufprintf(&out, "E %zu ", ts.errs[k].offset);
		escz(ts.errs[k].msg);
		bufputc(&out, '\n');
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
}

/* The parse form (tools/refdump/parse.go): one line per field, indented
 * by depth. A token is its index in the file's tokens, an absent token
 * or node "-", a node its type name with its fields below it, and a
 * list "list N M" with each element and the separator after it. Then
 * the order of the token walk, and the errors. */
static File *pf;

static void line(int depth, const char *field) {
	for (int i = 0; i < depth; i++)
		bufputc(&out, ' ');
	bufputs(&out, field);
	bufputc(&out, ' ');
}

static void tokval(Token *t) {
	if (t)
		bufprintf(&out, "%zu", (size_t)(t - pf->toks.v));
	else
		bufputc(&out, '-');
}

static void ptok(int depth, const char *field, Token *t) {
	line(depth, field);
	tokval(t);
	bufputc(&out, '\n');
}

static void ptoks(int depth, const char *field, TokList *l) {
	line(depth, field);
	bufprintf(&out, "list %zu %zu\n", l->elems.n, l->seps.n);
	for (size_t i = 0; i < l->elems.n; i++) {
		ptok(depth + 1, "e", l->elems.v[i]);
		if (i < l->seps.n)
			ptok(depth + 1, "s", l->seps.v[i]);
	}
}

static void pnode(int depth, const char *field, Node *n);

static void pnodes(int depth, const char *field, NodeList *l) {
	line(depth, field);
	bufprintf(&out, "list %zu %zu\n", l->elems.n, l->seps.n);
	for (size_t i = 0; i < l->elems.n; i++) {
		pnode(depth + 1, "e", l->elems.v[i]);
		if (i < l->seps.n)
			ptok(depth + 1, "s", l->seps.v[i]);
	}
}

static void pnode(int depth, const char *field, Node *n) {
	line(depth, field);
	if (!n) {
		bufputs(&out, "-\n");
		return;
	}
	bufprintf(&out, "%s\n", node_name(n->kind));
	int k = depth + 1;
	switch (n->kind) {
	case N_LINK:
		ptok(k, "Link", n->link.link);
		ptoks(k, "Files", &n->link.files);
		break;
	case N_GLOBAL:
		ptok(k, "Global", n->global.global);
		ptoks(k, "Names", &n->global.names);
		break;
	case N_RECORD:
		ptok(k, "Record", n->record.record);
		ptok(k, "Name", n->record.name);
		ptok(k, "Lparen", n->record.lparen);
		ptoks(k, "Fields", &n->record.fields);
		ptok(k, "Rparen", n->record.rparen);
		break;
	case N_INVOCABLE:
		ptok(k, "Invocable", n->invocable.invocable);
		pnodes(k, "Items", &n->invocable.items);
		break;
	case N_INVITEM:
		ptok(k, "Name", n->invitem.name);
		ptok(k, "Colon", n->invitem.colon);
		ptok(k, "Arity", n->invitem.arity);
		break;
	case N_PROC:
		ptok(k, "Procedure", n->proc.procedure);
		ptok(k, "Name", n->proc.name);
		ptok(k, "Lparen", n->proc.lparen);
		ptoks(k, "Params", &n->proc.params);
		ptok(k, "Lbrack", n->proc.lbrack);
		ptok(k, "Rbrack", n->proc.rbrack);
		ptok(k, "Rparen", n->proc.rparen);
		ptok(k, "Semi", n->proc.semi);
		line(k, "Locals");
		bufprintf(&out, "%zu\n", n->proc.locals.n);
		for (size_t i = 0; i < n->proc.locals.n; i++)
			pnode(k + 1, "l", n->proc.locals.v[i]);
		pnode(k, "Initial", n->proc.initial);
		pnodes(k, "Body", &n->proc.body);
		ptok(k, "End", n->proc.end);
		break;
	case N_LOCAL:
		ptok(k, "Keyword", n->local.keyword);
		ptoks(k, "Names", &n->local.names);
		ptok(k, "Semi", n->local.semi);
		break;
	case N_INITIAL:
		ptok(k, "Initial", n->initial.initial);
		pnode(k, "X", n->initial.x);
		ptok(k, "Semi", n->initial.semi);
		break;
	case N_BADDECL:
	case N_BADEXPR:
		line(k, "elems");
		bufprintf(&out, "%zu\n", n->bad.elems.n);
		for (size_t i = 0; i < n->bad.elems.n; i++) {
			Elem *e = &n->bad.elems.v[i];
			if (e->tok)
				ptok(k + 1, "t", e->tok);
			else
				pnode(k + 1, "n", e->node);
		}
		break;
	case N_IDENT:
		ptok(k, "Name", n->ident.name);
		break;
	case N_LITERAL:
		ptok(k, "Tok", n->literal.tok);
		break;
	case N_KEYWORD:
		ptok(k, "Amp", n->keyword.amp);
		ptok(k, "Name", n->keyword.name);
		break;
	case N_UNARY:
		ptok(k, "Op", n->unary.op);
		pnode(k, "X", n->unary.x);
		break;
	case N_BINARY:
		pnode(k, "X", n->binary.x);
		ptok(k, "Op", n->binary.op);
		pnode(k, "Y", n->binary.y);
		break;
	case N_TOBY:
		pnode(k, "X", n->toby.x);
		ptok(k, "To", n->toby.to);
		pnode(k, "Y", n->toby.y);
		ptok(k, "By", n->toby.by);
		pnode(k, "Z", n->toby.z);
		break;
	case N_PAREN:
		ptok(k, "Lparen", n->group.open);
		pnodes(k, "List", &n->group.list);
		ptok(k, "Rparen", n->group.close);
		break;
	case N_BLOCK:
		ptok(k, "Lbrace", n->group.open);
		pnodes(k, "List", &n->group.list);
		ptok(k, "Rbrace", n->group.close);
		break;
	case N_LISTLIT:
		ptok(k, "Lbrack", n->group.open);
		pnodes(k, "List", &n->group.list);
		ptok(k, "Rbrack", n->group.close);
		break;
	case N_INDEX:
		pnode(k, "X", n->index.x);
		ptok(k, "Lbrack", n->index.lbrack);
		pnodes(k, "List", &n->index.list);
		ptok(k, "Rbrack", n->index.rbrack);
		break;
	case N_SECTION:
		pnode(k, "X", n->section.x);
		ptok(k, "Lbrack", n->section.lbrack);
		pnode(k, "Lo", n->section.lo);
		ptok(k, "Op", n->section.op);
		pnode(k, "Hi", n->section.hi);
		ptok(k, "Rbrack", n->section.rbrack);
		break;
	case N_CALL:
		pnode(k, "Fun", n->call.fun);
		ptok(k, "Lparen", n->call.open);
		pnodes(k, "Args", &n->call.args);
		ptok(k, "Rparen", n->call.close);
		break;
	case N_PDCO:
		pnode(k, "Fun", n->call.fun);
		ptok(k, "Lbrace", n->call.open);
		pnodes(k, "Args", &n->call.args);
		ptok(k, "Rbrace", n->call.close);
		break;
	case N_FIELD:
		pnode(k, "X", n->field.x);
		ptok(k, "Dot", n->field.dot);
		ptok(k, "Name", n->field.name);
		break;
	case N_CREATE:
		ptok(k, "Create", n->create.create);
		pnode(k, "X", n->create.x);
		break;
	case N_IF:
		ptok(k, "If", n->if_.if_);
		pnode(k, "Cond", n->if_.cond);
		ptok(k, "Then", n->if_.then);
		pnode(k, "Body", n->if_.body);
		ptok(k, "Else", n->if_.else_);
		pnode(k, "Alt", n->if_.alt);
		break;
	case N_LOOP:
		ptok(k, "Keyword", n->loop.keyword);
		pnode(k, "Cond", n->loop.cond);
		ptok(k, "Do", n->loop.do_);
		pnode(k, "Body", n->loop.body);
		break;
	case N_RETURN:
		ptok(k, "Keyword", n->ret.keyword);
		pnode(k, "X", n->ret.x);
		ptok(k, "Do", n->ret.do_);
		pnode(k, "Body", n->ret.body);
		break;
	case N_BREAK:
		ptok(k, "Break", n->break_.break_);
		pnode(k, "X", n->break_.x);
		break;
	case N_NEXT:
		ptok(k, "Next", n->next.next);
		break;
	case N_CASE:
		ptok(k, "Case", n->case_.case_);
		pnode(k, "X", n->case_.x);
		ptok(k, "Of", n->case_.of);
		ptok(k, "Lbrace", n->case_.lbrace);
		pnodes(k, "Clauses", &n->case_.clauses);
		ptok(k, "Rbrace", n->case_.rbrace);
		break;
	case N_CASECLAUSE:
		ptok(k, "Default", n->clause.default_);
		pnode(k, "Label", n->clause.label);
		ptok(k, "Colon", n->clause.colon);
		pnode(k, "Body", n->clause.body);
		break;
	case N_NKINDS:
		abort();
	}
}

/* walk writes the tree walk twice over: by syn_children, recursively
 * (only in the parse form: it is the children that are under test),
 * and by syn_tokens; the two must agree. */
static Buf walked;

static void walk(void *ctx, Token *t, Node *n) {
	(void)ctx;
	if (t)
		bufprintf(&walked, " %zu", (size_t)(t - pf->toks.v));
	else
		syn_children(n, walk, NULL);
}

static void walktok(void *ctx, Token *t) {
	(void)ctx;
	bufputc(&out, ' ');
	tokval(t);
}

static void parse(const char *src, size_t n, bool tree) {
	pf = syn_parse(src, n);
	bufprintf(&out, "File %zu tokens\n", pf->toks.n);
	if (tree) {
		for (size_t i = 0; i < pf->decls.n; i++)
			pnode(1, "d", pf->decls.v[i]);
		ptok(1, "EOF", pf->eof);
	}
	size_t at = out.n;
	bufputc(&out, 'W');
	syn_tokens(pf, walktok, NULL);
	if (tree) {
		syn_file_children(pf, walk, NULL);
		if (walked.n != out.n - at - 1 || memcmp(walked.b, out.b + at + 1, walked.n)) {
			fprintf(stderr, "dump: syn_children and syn_tokens disagree\n");
			exit(1);
		}
	}
	bufputc(&out, '\n');
	for (size_t i = 0; i < pf->errs.n; i++) {
		bufprintf(&out, "E %zu ", pf->errs.v[i].offset);
		escz(pf->errs.v[i].msg);
		bufputc(&out, '\n');
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
}

/* words splits $var at spaces. */
static size_t words(const char *var, const char ***out) {
	const char *s = getenv(var);
	VEC(const char *) v = {0};
	while (s && *s) {
		while (*s == ' ')
			s++;
		const char *e = s;
		while (*e && *e != ' ')
			e++;
		if (e > s)
			VPUSH(v, xstrndup(s, (size_t)(e - s)));
		s = e;
	}
	*out = v.v;
	return v.n;
}

static PPOptions ppoptions(void) {
	PPOptions o = {0};
	const char **names, **values;
	o.npredef = words("PPDEFS", &names);
	values = xcalloc(o.npredef + 1, sizeof *values);
	for (size_t i = 0; i < o.npredef; i++)
		values[i] = "1";
	o.prenames = names;
	o.prevalues = values;
	const char **path;
	o.nincpath = words("PPPATH", &path);
	o.incpath = path;
	return o;
}

static void pp(const char *name, const char *src, size_t n) {
	PPOptions o = ppoptions();
	PPResult *r = ipp_preprocess(name, src, n, &o);
	for (size_t off = 0; off < r->src.n;) {
		const char *nl = memchr(r->src.b + off, '\n', r->src.n - off);
		size_t len = nl ? (size_t)(nl - (r->src.b + off)) + 1 : r->src.n - off;
		const char *file;
		long line;
		ipp_locate(&r->map, off, &file, &line);
		bufprintf(&out, "L %zu ", off);
		escz(file);
		bufprintf(&out, " %ld ", line);
		esc(r->src.b + off, len);
		bufputc(&out, '\n');
		off += len;
	}
	for (size_t i = 0; i < r->errs.n; i++) {
		bufputs(&out, "E ");
		escz(r->errs.v[i].file);
		bufprintf(&out, " %ld ", r->errs.v[i].line);
		escz(r->errs.v[i].msg);
		bufputc(&out, '\n');
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
}

static Buf readall(const char *name);

static SemaFile **sfiles;
static size_t nsfiles;

static void symstr(Symbol *s) {
	if (!s) {
		bufputc(&out, '-');
		return;
	}
	for (const char *k = sym_kind_name(s->kind); *k; k++)
		bufputc(&out, *k == ' ' ? '_' : *k);
	bufprintf(&out, " %s ", s->name);
	if (s->file && s->decl) {
		size_t fi = 0;
		while (sfiles[fi] != s->file)
			fi++;
		bufprintf(&out, "%zu:%zu", fi, (size_t)(s->decl - s->file->syn->toks.v));
	} else
		bufputc(&out, '-');
}

static void sema(int n, char **names) {
	PPOptions o = ppoptions();
	for (int pass = 0; pass < 2; pass++) {
		bool pre = pass == 0;
		nsfiles = (size_t)n;
		sfiles = xcalloc(nsfiles, sizeof *sfiles);
		for (int i = 0; i < n; i++) {
			Buf b = readall(names[i]);
			SemaFile *f = xcalloc(1, sizeof *f);
			f->path = names[i];
			f->preprocessed = pre;
			if (pre) {
				PPResult *r = ipp_preprocess(names[i], b.b, b.n, &o);
				f->syn = syn_parse(r->src.b, r->src.n);
			} else
				f->syn = syn_parse(b.b, b.n);
			sfiles[i] = f;
		}
		Program *p = sema_program(sfiles, nsfiles, true);
		bufprintf(&out, "P preprocessed=%s\n", pre ? "true" : "false");
		for (size_t i = 0; i < nsfiles; i++) {
			SemaFile *f = sfiles[i];
			SemaInfo *info = sema_resolve(p, f);
			bufprintf(&out, "F %zu ", i);
			escz(f->path);
			bufputc(&out, '\n');
			for (size_t k = 0; k < info->diags.n; k++) {
				Diagnostic *d = &info->diags.v[k];
				bufprintf(&out, "D %zu %zu %d ", d->offset, d->end, (int)d->severity);
				escz(d->msg);
				bufputs(&out, " R ");
				symstr(d->related);
				bufputc(&out, '\n');
			}
			for (size_t k = 0; k < f->syn->toks.n; k++) {
				if (info->uses[k]) {
					bufprintf(&out, "U %zu ", k);
					symstr(info->uses[k]);
					bufputc(&out, '\n');
				}
				if (info->fielduses[k]) {
					SymVec *v = info->fielduses[k];
					bufprintf(&out, "FU %zu %zu", k, v->n);
					for (size_t j = 0; j < v->n; j++) {
						bufputs(&out, " [");
						symstr(v->v[j]);
						bufputc(&out, ']');
					}
					bufputc(&out, '\n');
				}
				if (info->iskeyword[k])
					bufprintf(&out, "K %zu %s\n", k, info->keywords[k] ? info->keywords[k]->name : "-");
			}
			for (size_t k = 0; k < info->locals.n; k++) {
				ProcLocals *pl = info->locals.v[k];
				/* sorted by name, as the Go's dump */
				size_t m = pl->syms.n;
				Symbol **v = xcalloc(m + 1, sizeof *v);
				memcpy(v, pl->syms.v, m * sizeof *v);
				for (size_t a = 1; a < m; a++)
					for (size_t c = a; c > 0 && strcmp(v[c - 1]->name, v[c]->name) > 0; c--) {
						Symbol *t = v[c];
						v[c] = v[c - 1];
						v[c - 1] = t;
					}
				bufprintf(&out, "L %zu", (size_t)(pl->proc->proc.procedure - f->syn->toks.v));
				for (size_t j = 0; j < m; j++) {
					bufputs(&out, " [");
					symstr(v[j]);
					bufputc(&out, ']');
				}
				bufputc(&out, '\n');
			}
		}
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
}

static Buf readall(const char *name) {
	FILE *f = fopen(name, "rb");
	if (!f) {
		perror(name);
		exit(1);
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

static void errline(Buf *b, const char *file, long line, const char *msg) {
	bufprintf(b, "File %s; Line %ld # %s", file, line, msg);
}

static int build(int n, char **names) {
	PPOptions o = ppoptions();
	StrVec errs = {0};
	SemaFile **files = xcalloc((size_t)n, sizeof *files);
	PPResult **pps = xcalloc((size_t)n, sizeof *pps);
	for (int i = 0; i < n; i++) {
		Buf src = readall(names[i]);
		pps[i] = ipp_preprocess(names[i], src.b, src.n, &o);
		for (size_t k = 0; k < pps[i]->errs.n; k++)
			VPUSH(errs, ipp_error(&pps[i]->errs.v[k]));
		File *f = syn_parse(pps[i]->src.b, pps[i]->src.n);
		for (size_t k = 0; k < f->errs.n; k++) {
			const char *file;
			long line;
			ipp_locate(&pps[i]->map, f->errs.v[k].offset, &file, &line);
			Buf b = {0};
			errline(&b, file, line, f->errs.v[k].msg);
			VPUSH(errs, b.b);
		}
		files[i] = xcalloc(1, sizeof *files[i]);
		files[i]->path = names[i];
		files[i]->syn = f;
		files[i]->preprocessed = true;
	}
	if (!errs.n) {
		Program *p = sema_program(files, (size_t)n, true);
		CompileFile *cfs = xcalloc((size_t)n, sizeof *cfs);
		for (int i = 0; i < n; i++) {
			cfs[i].info = sema_resolve(p, files[i]);
			cfs[i].map = &pps[i]->map;
			for (size_t k = 0; k < cfs[i].info->diags.n; k++) {
				Diagnostic *d = &cfs[i].info->diags.v[k];
				if (d->severity != SEV_ERROR)
					continue;
				const char *file;
				long line;
				ipp_locate(&pps[i]->map, d->offset, &file, &line);
				Buf b = {0};
				errline(&b, file, line, d->msg);
				VPUSH(errs, b.b);
			}
		}
		if (!errs.n) {
			Prog *prog = compile_program(p, cfs, (size_t)n, &errs);
			if (!errs.n) {
				static const char *const k[] = {"test"}, *const v[] = {"1"};
				Buf enc = {0};
				const char *err = encode(prog, k, v, 1, &enc);
				if (!err) {
					fwrite(enc.b, 1, enc.n, stdout);
					return 0;
				}
				VPUSH(errs, err);
			}
		}
	}
	for (size_t i = 0; i < errs.n; i++) {
		escz(errs.v[i]);
		bufputc(&out, '\n');
	}
	fwrite(out.b ? out.b : "", 1, out.n, stdout);
	return 1;
}

int main(int argc, char **argv) {
	if (argc >= 3 && !strcmp(argv[1], "build"))
		return build(argc - 2, argv + 2);
	if (argc >= 3 && !strcmp(argv[1], "sema")) {
		sema(argc - 2, argv + 2);
		return 0;
	}
	if (argc != 3 || (strcmp(argv[1], "lex") && strcmp(argv[1], "parse") && strcmp(argv[1], "walk") &&
			    strcmp(argv[1], "pp"))) {
		fprintf(stderr, "usage: dump lex|parse|walk|pp FILE\n");
		return 2;
	}
	FILE *f = fopen(argv[2], "rb");
	if (!f) {
		perror(argv[2]);
		return 1;
	}
	Buf src = {0};
	char chunk[65536];
	size_t k;
	while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
		bufadd(&src, chunk, k);
	fclose(f);
	if (!strcmp(argv[1], "pp"))
		pp(argv[2], src.b ? src.b : "", src.n);
	else if (!strcmp(argv[1], "parse") || !strcmp(argv[1], "walk"))
		parse(src.b ? src.b : "", src.n, !strcmp(argv[1], "parse"));
	else
		lex(src.b ? src.b : "", src.n);
	return 0;
}
