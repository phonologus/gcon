/*
 * Doc comments (doccomment.c), as Go's: lightweight and heuristic. The
 * comment lines directly before a declaration, or an IPL-style "#:" on
 * its line, document it. Their text is read as Go reads a doc comment
 * (design/design.md, "Documentation"):
 *
 *   - a blank comment line separates paragraphs;
 *   - indented lines (more than the comment's own indentation) are kept
 *     as they are, as code;
 *   - a line beginning "-", "*", "+" or "1." is a list item;
 *   - "# Heading" (written "## Heading" in the source) is a heading;
 *   - a rule (a line of #, =, - or *) is dropped;
 *   - everything else is literal text: no other markup.
 *
 * The result is the documentation's Markdown (markdown.h), for gcon doc
 * and gcon lsp's hover.
 */
#ifndef GCON_DOCCOMMENT_H
#define GCON_DOCCOMMENT_H

#include "util.h"

/* doccomment_md reads n comment lines, each as written ("# text",
 * "#: summary"), and produces their Markdown ("" for none). */
char *doccomment_md(char *const *lines, size_t n);

/* A declaration of a source file, for gcon doc FILE. */
typedef struct DocDecl {
	char *kind;  /* "procedure", "record", "global" */
	char *name;  /* the first name, for a global */
	char *decl;  /* its heading, as written: "procedure f(a, b)" */
	char *doc;   /* its doc comment, as Markdown, or "" */
	int line;
} DocDecl;

typedef VEC(DocDecl) DocDecls;

/* doccomment_file reads a source file's declarations (those at the
 * start of a line) and their doc comments, and the file's own comment:
 * the comment at its top, unless that documents the first declaration
 * (no blank line between them). */
DocDecls doccomment_file(const char *src, size_t n, char **filedoc);

#endif
