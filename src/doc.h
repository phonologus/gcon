/*
 * gcon doc: the documentation tree (doc.c). design/design.md,
 * "Delivery".
 */
#ifndef GCON_DOC_H
#define GCON_DOC_H

/* doc_main is "gcon doc [flags] [name]"; argv[0] is "doc". */
int doc_main(int argc, char **argv);

/* The reference: the entries of the pages in the tree's ref directory,
 * each a "## name" heading (a function's name, or a keyword's with its
 * "&"), for gcon lsp's hover. */
typedef struct DocRef DocRef;

/* docref_load reads the reference in root's doc/ref; NULL if there is
 * none. */
DocRef *docref_load(const char *root);

/* docref_entry is name's entry, its Markdown without the heading and
 * the examples, or NULL. */
const char *docref_entry(DocRef *r, const char *name);

#endif
