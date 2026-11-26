/*
 * The language server (gcon lsp): icontools' iconls (internal/lsp) on
 * gcon's C front end, with gcon's infrastructure (the root's library is
 * searched for linked and included files) and gcon's additions to the
 * language (lsp.c).
 */
#ifndef GCON_LSP_H
#define GCON_LSP_H

/* lsp_main is the gcon lsp command (argv[0] is "lsp"):
 *
 *	gcon lsp [-log file] [-docs dir]   serve LSP on standard input and output
 *	gcon lsp fetch-docs [-dir dir]      fetch the Icon reference pages
 *
 * It returns the exit status. */
int lsp_main(int argc, char **argv);

#endif
