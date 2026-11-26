/*
 * Reference documentation for built-ins (icontools' builtin/docs.go):
 * the Icon website's reference pages, fetched to a local cache (they
 * are not redistributable, so not part of gcon), rendered as Markdown
 * for the language server's hovers.
 */
#ifndef GCON_REFDOCS_H
#define GCON_REFDOCS_H

#include "util.h"

typedef struct RefDocs {
	StrMap functions; /* name -> Markdown */
	StrMap keywords;  /* name, without "&" -> Markdown */
} RefDocs;

/* refdocs_load reads whichever reference pages are present in dir;
 * NULL if there are none. */
RefDocs *refdocs_load(const char *dir);

/* refdocs_fetch downloads the pages into dir (with curl), reporting
 * progress on stderr; it returns the exit status. */
int refdocs_fetch(const char *dir);

#endif
