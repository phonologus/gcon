/*
 * The formatter (gcon fmt): icontools' internal/format, the oracle its
 * output is checked against (icfmt), on gcon's C front end.
 *
 * Indentation and spacing in the style of the Icon Program Library,
 * brace placement and alignment in the style of gofmt. Line breaks are
 * kept as written, except around braces; indentation and spacing are
 * normalised. Formatting never changes the token sequence the Icon
 * translator sees, including inserted semicolons.
 */
#ifndef GCON_FMT_H
#define GCON_FMT_H

#include "util.h"

/* format_source formats src (n bytes). It returns true with the result
 * in out, or false with the syntax errors ("L:C: msg", one a line) in
 * out. */
bool format_source(const char *src, size_t n, Buf *out);

/* format_main is the gcon fmt command: gcon fmt [-l] [-w] [-d] [path ...]
 * (argv[0] is "fmt"); it returns the exit status. */
int format_main(int argc, char **argv);

#endif
