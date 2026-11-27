/*
 * gcon help: the tool's own manual, as go help is Go's. The pages are
 * plain text in src/help, compiled in by tools/help.sh, so help is
 * always the installed version's and needs no root.
 */
#ifndef GCON_HELP_H
#define GCON_HELP_H

#include <stdbool.h>

/* help_main is "gcon help [name]"; argv[0] is "help". */
int help_main(int argc, char **argv);

/* help_isflag reports whether a is a request for help: -h, -help,
 * --h or --help. */
bool help_isflag(const char *a);

/* help_page prints command name's page on standard output and exits
 * 0: what "gcon name -h" does. */
_Noreturn void help_page(const char *name);

/* help_usage prints command name's usage on standard error, with a
 * pointer to its page, and exits 2: what misuse of a command does. */
_Noreturn void help_usage(const char *name);

/* help_overview prints the overview on standard error and exits 2:
 * what gcon with no command, or an unknown one, does. */
_Noreturn void help_overview(void);

#endif
